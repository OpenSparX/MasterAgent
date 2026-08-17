/**
 * @file sparx_context_manager.cpp
 * @brief Implementation of the Agent OS Context Manager.
 *
 * Provides context switching, compression, and paging for concurrent agents.
 * Analogous to an OS managing virtual memory pages for processes.
 */

#include "sparx_context_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <numeric>
#include <sstream>

namespace sparx::os {

// ─── Helpers ────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─── Constructor / Destructor ───────────────────────────────────────────────

ContextManager::ContextManager(ContextManagerConfig config)
    : config_(std::move(config)) {
    if (config_.page_directory.empty()) {
        config_.page_directory = ".sparx/contexts";
    }
}

ContextManager::~ContextManager() = default;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

ContextHandle ContextManager::create(const std::string& agent_name,
                                     const std::string& system_prompt,
                                     uint32_t max_tokens) {
    std::lock_guard<std::mutex> lock(mutex_);

    ContextHandle h = next_handle_++;
    ContextSnapshot ctx;
    ctx.handle = h;
    ctx.agent_name = agent_name;
    ctx.system_prompt = system_prompt;
    ctx.max_tokens = max_tokens;
    ctx.last_active_ms = nowMs();
    ctx.total_tokens = estimateTokens(system_prompt);

    // Add system prompt as first message
    ContextMessage sys_msg;
    sys_msg.role = ContextMessage::Role::System;
    sys_msg.content = system_prompt;
    sys_msg.timestamp_ms = ctx.last_active_ms;
    sys_msg.token_count = ctx.total_tokens;
    sys_msg.pinned = true;  // System prompt always pinned
    ctx.messages.push_back(std::move(sys_msg));

    resident_[h] = std::move(ctx);
    stats_.total_contexts++;
    stats_.resident_contexts++;
    return h;
}

void ContextManager::destroy(ContextHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it != resident_.end()) {
        resident_.erase(it);
        stats_.resident_contexts--;
        stats_.total_contexts--;
        return;
    }
    auto pit = paged_.find(handle);
    if (pit != paged_.end()) {
        // Remove disk file
        // std::filesystem::remove(pit->second.disk_path);
        paged_.erase(pit);
        stats_.paged_contexts--;
        stats_.total_contexts--;
    }
}

// ─── Message Management ─────────────────────────────────────────────────────

void ContextManager::appendMessage(ContextHandle handle, ContextMessage msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return;

    auto& ctx = it->second;
    msg.timestamp_ms = nowMs();
    msg.token_count = estimateTokens(msg.content);
    ctx.total_tokens += msg.token_count;
    ctx.turn_count++;
    ctx.last_active_ms = msg.timestamp_ms;
    ctx.messages.push_back(std::move(msg));
}

std::vector<ContextMessage> ContextManager::getMessages(ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return {};
    return it->second.messages;
}

uint32_t ContextManager::tokenUsage(ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return 0;
    return it->second.total_tokens;
}

bool ContextManager::needsCompression(ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return false;
    return it->second.total_tokens >= config_.compression_threshold;
}

// ─── Context Switching ──────────────────────────────────────────────────────

std::optional<ContextSnapshot> ContextManager::snapshot(ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return std::nullopt;
    return it->second;
}

bool ContextManager::restore(ContextHandle handle, const ContextSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return false;

    it->second = snap;
    it->second.last_active_ms = nowMs();
    return true;
}

// ─── Compression ────────────────────────────────────────────────────────────

uint32_t ContextManager::compress(ContextHandle handle) {
    return compress(handle, config_.strategy);
}

uint32_t ContextManager::compress(ContextHandle handle,
                                  CompressionStrategy strategy) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return 0;

    auto& ctx = it->second;
    uint32_t reclaimed = 0;

    switch (strategy) {
        case CompressionStrategy::Summarize:
            reclaimed = compressSummarize(ctx);
            break;
        case CompressionStrategy::SlidingWindow:
            reclaimed = compressSlidingWindow(ctx);
            break;
        case CompressionStrategy::PinnedPlusRecent:
            reclaimed = compressPinnedRecent(ctx);
            break;
        case CompressionStrategy::Hierarchical:
            reclaimed = compressHierarchical(ctx);
            break;
    }

    ctx.compressed = true;
    stats_.compressions_performed++;
    stats_.tokens_reclaimed += reclaimed;
    return reclaimed;
}

void ContextManager::setSummarizer(SummarizerFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    summarizer_ = std::move(fn);
}

uint32_t ContextManager::compressSlidingWindow(ContextSnapshot& ctx) {
    // Keep only the most recent messages within the token budget.
    uint32_t target_tokens = config_.compression_threshold * 2 / 3;
    uint32_t reclaimed = 0;

    while (ctx.total_tokens > target_tokens && ctx.messages.size() > config_.min_messages_after_compress) {
        // Find the first non-pinned message
        auto victim = std::find_if(ctx.messages.begin(), ctx.messages.end(),
            [](const ContextMessage& m) { return !m.pinned; });
        if (victim == ctx.messages.end()) break;

        reclaimed += victim->token_count;
        ctx.total_tokens -= victim->token_count;
        ctx.messages.erase(victim);
    }
    return reclaimed;
}

uint32_t ContextManager::compressSummarize(ContextSnapshot& ctx) {
    // Collect non-pinned messages in the first half for summarization.
    size_t midpoint = ctx.messages.size() / 2;
    std::vector<ContextMessage> to_summarize;
    uint32_t tokens_removed = 0;

    std::vector<ContextMessage> remaining;
    for (size_t i = 0; i < ctx.messages.size(); ++i) {
        auto& msg = ctx.messages[i];
        if (i < midpoint && !msg.pinned) {
            to_summarize.push_back(msg);
            tokens_removed += msg.token_count;
        } else {
            remaining.push_back(std::move(msg));
        }
    }

    if (to_summarize.empty()) return 0;

    // Generate summary
    std::string summary_text;
    if (summarizer_) {
        summary_text = summarizer_(to_summarize);
    } else {
        // Default: concatenate role:content pairs (lossy but functional)
        std::ostringstream oss;
        oss << "[Summary of " << to_summarize.size() << " messages] ";
        for (auto& m : to_summarize) {
            oss << m.content.substr(0, 50) << "... ";
        }
        summary_text = oss.str();
    }

    // Insert summary as a pinned system message after the system prompt
    ContextMessage summary_msg;
    summary_msg.role = ContextMessage::Role::System;
    summary_msg.content = std::move(summary_text);
    summary_msg.timestamp_ms = nowMs();
    summary_msg.token_count = estimateTokens(summary_msg.content);
    summary_msg.pinned = true;

    uint32_t tokens_added = summary_msg.token_count;

    // Insert after first pinned system message
    auto insert_pos = remaining.begin();
    if (!remaining.empty() && remaining.front().pinned) {
        ++insert_pos;
    }
    remaining.insert(insert_pos, std::move(summary_msg));

    ctx.messages = std::move(remaining);
    uint32_t net_reclaimed = tokens_removed > tokens_added
                             ? tokens_removed - tokens_added : 0;
    ctx.total_tokens -= net_reclaimed;
    return net_reclaimed;
}

uint32_t ContextManager::compressPinnedRecent(ContextSnapshot& ctx) {
    // Keep all pinned messages + last K turns.
    uint32_t keep_turns = config_.min_messages_after_compress;
    uint32_t reclaimed = 0;

    std::vector<ContextMessage> pinned;
    std::vector<ContextMessage> unpinned;
    for (auto& m : ctx.messages) {
        if (m.pinned) {
            pinned.push_back(std::move(m));
        } else {
            unpinned.push_back(std::move(m));
        }
    }

    // Keep only the last keep_turns unpinned messages
    while (unpinned.size() > keep_turns) {
        reclaimed += unpinned.front().token_count;
        unpinned.erase(unpinned.begin());
    }

    // Rebuild messages: pinned first, then recent
    ctx.messages.clear();
    ctx.messages.insert(ctx.messages.end(),
                        std::make_move_iterator(pinned.begin()),
                        std::make_move_iterator(pinned.end()));
    ctx.messages.insert(ctx.messages.end(),
                        std::make_move_iterator(unpinned.begin()),
                        std::make_move_iterator(unpinned.end()));

    ctx.total_tokens -= reclaimed;
    return reclaimed;
}

uint32_t ContextManager::compressHierarchical(ContextSnapshot& ctx) {
    // Three-tier: recent messages kept verbatim, mid-range summarized,
    // oldest reduced to keywords/tags only.
    size_t total = ctx.messages.size();
    if (total <= config_.min_messages_after_compress) return 0;

    // Tiers: last 1/3 = full, middle 1/3 = summarized, first 1/3 = keywords
    size_t tier_size = total / 3;
    uint32_t reclaimed = 0;

    // First pass: compress the oldest tier to keywords
    for (size_t i = 0; i < tier_size; ++i) {
        auto& msg = ctx.messages[i];
        if (msg.pinned) continue;
        uint32_t old_tokens = msg.token_count;
        // Reduce to first 30 chars as keyword hint
        if (msg.content.size() > 30) {
            msg.content = msg.content.substr(0, 30) + "...";
            msg.token_count = estimateTokens(msg.content);
            reclaimed += old_tokens - msg.token_count;
        }
    }

    // Second pass: compress the middle tier to ~50% of original
    for (size_t i = tier_size; i < tier_size * 2; ++i) {
        auto& msg = ctx.messages[i];
        if (msg.pinned) continue;
        uint32_t old_tokens = msg.token_count;
        size_t half = msg.content.size() / 2;
        if (half > 50) {
            msg.content = msg.content.substr(0, half) + " [...]";
            msg.token_count = estimateTokens(msg.content);
            reclaimed += old_tokens - msg.token_count;
        }
    }

    ctx.total_tokens -= reclaimed;
    return reclaimed;
}

// ─── Paging ─────────────────────────────────────────────────────────────────

bool ContextManager::pageOut(ContextHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = resident_.find(handle);
    if (it == resident_.end()) return false;

    ContextPage page;
    page.handle = handle;
    page.serialized_data = serialize(it->second);
    page.original_tokens = it->second.total_tokens;
    page.paged_at_ms = nowMs();
    page.disk_path = config_.page_directory + "/" +
                     std::to_string(handle) + ".ctx";
    page.dirty = false;

    // Write to disk
    std::ofstream ofs(page.disk_path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(page.serialized_data.data(),
              static_cast<std::streamsize>(page.serialized_data.size()));
    if (!ofs) return false;

    // Remove from resident set
    resident_.erase(it);
    paged_[handle] = std::move(page);

    stats_.resident_contexts--;
    stats_.paged_contexts++;
    stats_.page_outs++;
    return true;
}

bool ContextManager::pageIn(ContextHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = paged_.find(handle);
    if (it == paged_.end()) return false;

    // Read from disk
    std::ifstream ifs(it->second.disk_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    auto sz = ifs.tellg();
    ifs.seekg(0);
    std::string data(static_cast<size_t>(sz), '\0');
    ifs.read(data.data(), sz);

    ContextSnapshot ctx = deserialize(data);
    ctx.last_active_ms = nowMs();
    resident_[handle] = std::move(ctx);

    paged_.erase(it);
    stats_.resident_contexts++;
    stats_.paged_contexts--;
    stats_.page_ins++;
    return true;
}

bool ContextManager::isPagedOut(ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paged_.count(handle) > 0;
}

// ─── Metadata ───────────────────────────────────────────────────────────────

void ContextManager::setMetadata(ContextHandle handle,
                                 const std::string& key,
                                 const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resident_.find(handle);
    if (it != resident_.end()) {
        it->second.metadata[key] = value;
    }
}

std::optional<std::string> ContextManager::getMetadata(
    ContextHandle handle, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resident_.find(handle);
    if (it == resident_.end()) return std::nullopt;
    auto mit = it->second.metadata.find(key);
    if (mit == it->second.metadata.end()) return std::nullopt;
    return mit->second;
}

// ─── Tool Management ────────────────────────────────────────────────────────

void ContextManager::setActiveTools(ContextHandle handle,
                                    const std::vector<std::string>& tools) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resident_.find(handle);
    if (it != resident_.end()) {
        it->second.active_tools = tools;
    }
}

std::vector<std::string> ContextManager::getActiveTools(
    ContextHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resident_.find(handle);
    if (it == resident_.end()) return {};
    return it->second.active_tools;
}

// ─── Stats ──────────────────────────────────────────────────────────────────

ContextManager::Stats ContextManager::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = stats_;
    s.total_tokens_managed = 0;
    for (auto& [h, ctx] : resident_) {
        s.total_tokens_managed += ctx.total_tokens;
    }
    return s;
}

// ─── Maintenance ────────────────────────────────────────────────────────────

void ContextManager::maintenance() {
    // Auto-page inactive contexts
    if (config_.auto_page_timeout_ms <= 0) return;

    int64_t now = nowMs();
    std::vector<ContextHandle> to_page;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [h, ctx] : resident_) {
            if ((now - ctx.last_active_ms) > config_.auto_page_timeout_ms) {
                to_page.push_back(h);
            }
        }
    }

    for (auto h : to_page) {
        pageOut(h);
    }
}

// ─── Internal ───────────────────────────────────────────────────────────────

uint32_t ContextManager::estimateTokens(const std::string& text) const {
    // Rough estimation: chars / chars_per_token
    return static_cast<uint32_t>(
        static_cast<float>(text.size()) / config_.chars_per_token + 0.5f);
}

std::string ContextManager::serialize(const ContextSnapshot& ctx) const {
    // Simple serialization: in production, use protobuf or flatbuffers.
    // For now, a basic text format for correctness.
    std::ostringstream oss;
    oss << "CTX:" << ctx.handle << "\n";
    oss << "NAME:" << ctx.agent_name << "\n";
    oss << "TOKENS:" << ctx.total_tokens << "/" << ctx.max_tokens << "\n";
    oss << "TURNS:" << ctx.turn_count << "\n";
    oss << "MSGS:" << ctx.messages.size() << "\n";
    for (auto& msg : ctx.messages) {
        oss << static_cast<int>(msg.role) << "|"
            << msg.token_count << "|"
            << msg.pinned << "|"
            << msg.content.size() << "|"
            << msg.content << "\n";
    }
    return oss.str();
}

ContextSnapshot ContextManager::deserialize(const std::string& data) const {
    // Simplified deserialization matching serialize() format.
    ContextSnapshot ctx;
    std::istringstream iss(data);
    std::string line;

    if (std::getline(iss, line) && line.substr(0, 4) == "CTX:") {
        ctx.handle = std::stoull(line.substr(4));
    }
    if (std::getline(iss, line) && line.substr(0, 5) == "NAME:") {
        ctx.agent_name = line.substr(5);
    }
    if (std::getline(iss, line) && line.substr(0, 7) == "TOKENS:") {
        auto sep = line.find('/');
        ctx.total_tokens = std::stoul(line.substr(7, sep - 7));
        ctx.max_tokens = std::stoul(line.substr(sep + 1));
    }
    if (std::getline(iss, line) && line.substr(0, 6) == "TURNS:") {
        ctx.turn_count = std::stoul(line.substr(6));
    }
    if (std::getline(iss, line) && line.substr(0, 5) == "MSGS:") {
        size_t msg_count = std::stoull(line.substr(5));
        for (size_t i = 0; i < msg_count && std::getline(iss, line); ++i) {
            ContextMessage msg;
            // Parse: role|token_count|pinned|content_len|content
            size_t p1 = line.find('|');
            size_t p2 = line.find('|', p1 + 1);
            size_t p3 = line.find('|', p2 + 1);
            size_t p4 = line.find('|', p3 + 1);
            if (p1 != std::string::npos && p4 != std::string::npos) {
                msg.role = static_cast<ContextMessage::Role>(
                    std::stoi(line.substr(0, p1)));
                msg.token_count = std::stoul(line.substr(p1 + 1, p2 - p1 - 1));
                msg.pinned = (line[p2 + 1] == '1');
                msg.content = line.substr(p4 + 1);
            }
            ctx.messages.push_back(std::move(msg));
        }
    }
    return ctx;
}

}  // namespace sparx::os
