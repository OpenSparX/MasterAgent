/**
 * @file sparx_memory_manager.cpp
 * @brief Implementation of the Agent OS Memory Manager.
 *
 * Three-tier hierarchical memory: Working (L1) → Episodic (L2) → Semantic (L3).
 * Inspired by MemGPT and cognitive science memory consolidation models.
 */

#include "sparx_memory_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <thread>

namespace sparx::os {

// ─── Helpers ────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static float hoursElapsed(int64_t from_ms) {
    int64_t elapsed_ms = nowMs() - from_ms;
    return static_cast<float>(elapsed_ms) / 3600000.0f;
}

// ─── Constructor / Destructor ───────────────────────────────────────────────

MemoryManager::MemoryManager(MemoryManagerConfig config)
    : config_(std::move(config)) {
    if (config_.storage_path.empty()) {
        config_.storage_path = ".sparx/memory";
    }
}

MemoryManager::~MemoryManager() {
    stop();
}

void MemoryManager::start() {
    if (running_.exchange(true)) return;
    if (config_.consolidation_interval_s > 0) {
        consolidation_thread_ = std::thread([this]() { consolidationLoop(); });
    }
}

void MemoryManager::stop() {
    if (!running_.exchange(false)) return;
    if (consolidation_thread_.joinable()) {
        consolidation_thread_.join();
    }
}

// ─── Store ──────────────────────────────────────────────────────────────────

uint64_t MemoryManager::store(const std::string& content, MemoryTier tier,
                              Importance importance,
                              const std::string& agent_id,
                              const std::vector<std::string>& tags) {
    MemoryEntry entry;
    entry.content = content;
    entry.tier = tier;
    entry.importance = importance;
    entry.agent_id = agent_id;
    entry.tags = tags;
    return store(std::move(entry));
}

uint64_t MemoryManager::store(MemoryEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);

    entry.id = next_id_++;
    entry.created_at_ms = nowMs();
    entry.last_accessed_ms = entry.created_at_ms;
    entry.access_count = 0;
    entry.decay_weight = 1.0f;

    // Compute embedding if not provided
    if (entry.embedding.empty() && config_.embedding_dim > 0) {
        entry.embedding = computeEmbedding(entry.content);
    }

    switch (entry.tier) {
        case MemoryTier::Working:
            // Working memory is managed by ContextManager, store as episodic
            entry.tier = MemoryTier::Episodic;
            [[fallthrough]];
        case MemoryTier::Episodic:
            episodic_.push_back(std::move(entry));
            stats_.episodic_count = static_cast<uint32_t>(episodic_.size());
            break;
        case MemoryTier::Semantic:
            semantic_.push_back(std::move(entry));
            stats_.semantic_count = static_cast<uint32_t>(semantic_.size());
            break;
    }
    return entry.id ? entry.id : next_id_ - 1;
}

// ─── Recall ─────────────────────────────────────────────────────────────────

std::vector<RecallResult> MemoryManager::recall(const std::string& query,
                                                uint32_t top_k,
                                                const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_recalls++;

    std::vector<RecallResult> results;
    std::vector<float> query_embedding;
    if (config_.embedding_dim > 0) {
        query_embedding = computeEmbedding(query);
    }

    // Search episodic tier
    for (auto& entry : episodic_) {
        if (!agent_id.empty() && entry.agent_id != agent_id) continue;
        if (entry.decay_weight < 0.01f) continue;

        float score = 0.0f;
        if (!query_embedding.empty() && !entry.embedding.empty()) {
            score = cosineSim(query_embedding, entry.embedding);
        } else {
            score = textScore(query, entry.content);
        }
        // Weight by decay and importance
        score *= entry.decay_weight;
        score *= (1.0f + 0.5f * static_cast<float>(entry.importance));

        if (score > 0.01f) {
            results.push_back({entry, score, MemoryTier::Episodic});
        }
    }

    // Search semantic tier
    for (auto& entry : semantic_) {
        if (!agent_id.empty() && entry.agent_id != agent_id) continue;

        float score = 0.0f;
        if (!query_embedding.empty() && !entry.embedding.empty()) {
            score = cosineSim(query_embedding, entry.embedding);
        } else {
            score = textScore(query, entry.content);
        }
        score *= (1.0f + 0.5f * static_cast<float>(entry.importance));

        if (score > 0.01f) {
            results.push_back({entry, score, MemoryTier::Semantic});
        }
    }

    // Sort by relevance and return top-k
    std::partial_sort(results.begin(),
                      results.begin() + std::min(static_cast<size_t>(top_k),
                                                  results.size()),
                      results.end(),
                      [](const RecallResult& a, const RecallResult& b) {
                          return a.relevance_score > b.relevance_score;
                      });

    if (results.size() > top_k) {
        results.resize(top_k);
    }
    return results;
}

std::vector<RecallResult> MemoryManager::recallByTag(const std::string& tag,
                                                     uint32_t top_k) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<RecallResult> results;
    auto has_tag = [&tag](const MemoryEntry& e) {
        return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end();
    };

    for (auto& e : episodic_) {
        if (has_tag(e)) {
            results.push_back({e, e.decay_weight, MemoryTier::Episodic});
        }
    }
    for (auto& e : semantic_) {
        if (has_tag(e)) {
            results.push_back({e, 1.0f, MemoryTier::Semantic});
        }
    }

    std::sort(results.begin(), results.end(),
              [](const RecallResult& a, const RecallResult& b) {
                  return a.entry.last_accessed_ms > b.entry.last_accessed_ms;
              });
    if (results.size() > top_k) results.resize(top_k);
    return results;
}

std::vector<MemoryEntry> MemoryManager::recentEpisodic(
    uint32_t count, const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<MemoryEntry> results;
    for (auto it = episodic_.rbegin(); it != episodic_.rend(); ++it) {
        if (!agent_id.empty() && it->agent_id != agent_id) continue;
        results.push_back(*it);
        if (results.size() >= count) break;
    }
    return results;
}

// ─── Maintenance ────────────────────────────────────────────────────────────

uint32_t MemoryManager::consolidate() {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t promoted = 0;
    auto it = episodic_.begin();
    while (it != episodic_.end()) {
        // Promote if accessed enough times and important enough
        if (it->access_count >= config_.consolidation_access_threshold &&
            it->importance >= config_.min_importance_for_semantic) {
            MemoryEntry promoted_entry = *it;
            promoted_entry.tier = MemoryTier::Semantic;
            promoted_entry.decay_weight = 1.0f;  // Reset decay on promotion
            semantic_.push_back(std::move(promoted_entry));
            it = episodic_.erase(it);
            promoted++;
        } else {
            ++it;
        }
    }

    stats_.consolidations++;
    stats_.episodic_count = static_cast<uint32_t>(episodic_.size());
    stats_.semantic_count = static_cast<uint32_t>(semantic_.size());
    return promoted;
}

void MemoryManager::applyDecay() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& entry : episodic_) {
        float age_hours = hoursElapsed(entry.created_at_ms);
        entry.decay_weight = std::exp(-config_.decay_lambda * age_hours);
        // Boost for important or frequently accessed
        if (entry.importance >= Importance::Notable) {
            entry.decay_weight = std::min(1.0f, entry.decay_weight * 2.0f);
        }
        if (entry.access_count > 5) {
            entry.decay_weight = std::min(1.0f, entry.decay_weight * 1.5f);
        }
    }
}

uint32_t MemoryManager::compact(float min_weight) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t removed = 0;
    auto it = episodic_.begin();
    while (it != episodic_.end()) {
        if (it->decay_weight < min_weight &&
            it->importance < Importance::Critical) {
            it = episodic_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    stats_.episodic_count = static_cast<uint32_t>(episodic_.size());
    stats_.entries_forgotten += removed;
    return removed;
}

uint32_t MemoryManager::forget(const std::string& agent_id,
                               const std::vector<std::string>& tags) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto should_forget = [&](const MemoryEntry& e) {
        if (!agent_id.empty() && e.agent_id != agent_id) return false;
        if (tags.empty()) return true;
        for (auto& t : tags) {
            if (std::find(e.tags.begin(), e.tags.end(), t) != e.tags.end())
                return true;
        }
        return false;
    };

    uint32_t removed = 0;
    auto eit = std::remove_if(episodic_.begin(), episodic_.end(), should_forget);
    removed += static_cast<uint32_t>(std::distance(eit, episodic_.end()));
    episodic_.erase(eit, episodic_.end());

    auto sit = std::remove_if(semantic_.begin(), semantic_.end(), should_forget);
    removed += static_cast<uint32_t>(std::distance(sit, semantic_.end()));
    semantic_.erase(sit, semantic_.end());

    stats_.episodic_count = static_cast<uint32_t>(episodic_.size());
    stats_.semantic_count = static_cast<uint32_t>(semantic_.size());
    stats_.entries_forgotten += removed;
    return removed;
}

// ─── Persistence ────────────────────────────────────────────────────────────

void MemoryManager::save() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Save episodic
    std::string epi_path = config_.storage_path + "/episodic.mem";
    std::ofstream eofs(epi_path, std::ios::binary);
    if (eofs) {
        for (auto& e : episodic_) {
            eofs << e.id << "|" << static_cast<int>(e.importance) << "|"
                 << e.decay_weight << "|" << e.agent_id << "|"
                 << e.content.size() << "|" << e.content << "\n";
        }
    }

    // Save semantic
    std::string sem_path = config_.storage_path + "/semantic.mem";
    std::ofstream sofs(sem_path, std::ios::binary);
    if (sofs) {
        for (auto& e : semantic_) {
            sofs << e.id << "|" << static_cast<int>(e.importance) << "|"
                 << e.agent_id << "|"
                 << e.content.size() << "|" << e.content << "\n";
        }
    }
}

void MemoryManager::load() {
    // Simplified: in production, parse the serialized format from save().
    // This stub ensures the interface is functional.
}

// ─── Stats ──────────────────────────────────────────────────────────────────

MemoryManager::Stats MemoryManager::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ─── Internal ───────────────────────────────────────────────────────────────

std::vector<float> MemoryManager::computeEmbedding(const std::string& text) const {
    // SimHash-style locality-sensitive hashing for embedding.
    // In production, this would call an embedding model (e.g., text-embedding-3-small).
    uint32_t dim = config_.embedding_dim;
    std::vector<float> emb(dim, 0.0f);

    // Character n-gram hashing projected into embedding space
    for (size_t i = 0; i + 3 <= text.size(); ++i) {
        uint32_t hash = 0;
        for (size_t j = 0; j < 3; ++j) {
            hash = hash * 31 + static_cast<uint32_t>(text[i + j]);
        }
        uint32_t idx = hash % dim;
        emb[idx] += (hash & 1) ? 1.0f : -1.0f;
    }

    // L2 normalize
    float norm = 0.0f;
    for (float v : emb) norm += v * v;
    if (norm > 0.0f) {
        norm = std::sqrt(norm);
        for (float& v : emb) v /= norm;
    }
    return emb;
}

float MemoryManager::cosineSim(const std::vector<float>& a,
                               const std::vector<float>& b) const {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
    }
    // Already L2-normalized, so dot product = cosine similarity
    return std::max(0.0f, dot);
}

float MemoryManager::textScore(const std::string& query,
                               const std::string& content) const {
    // Simple BM25-like scoring: term frequency overlap.
    if (query.empty() || content.empty()) return 0.0f;

    // Tokenize query into words
    std::vector<std::string> query_terms;
    std::istringstream iss(query);
    std::string word;
    while (iss >> word) {
        // Lowercase
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
        if (word.size() >= 2) query_terms.push_back(word);
    }

    if (query_terms.empty()) return 0.0f;

    // Convert content to lowercase for matching
    std::string lower_content = content;
    std::transform(lower_content.begin(), lower_content.end(),
                   lower_content.begin(), ::tolower);

    // Count matches
    uint32_t matches = 0;
    for (auto& term : query_terms) {
        if (lower_content.find(term) != std::string::npos) {
            matches++;
        }
    }

    return static_cast<float>(matches) / static_cast<float>(query_terms.size());
}

void MemoryManager::consolidationLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(
            std::chrono::seconds(config_.consolidation_interval_s));
        if (!running_.load()) break;

        applyDecay();
        consolidate();
        compact();
    }
}

}  // namespace sparx::os
