#pragma once
/**
 * @file sparx_context_manager.h
 * @brief Context Manager — OS-level context switching for concurrent Agents.
 *
 * Research basis:
 *   - AIOS Context Management (arXiv:2403.16971) — snapshot/restore LLM state
 *   - "MemGPT: Towards LLMs as Operating Systems" (arXiv:2310.08560)
 *   - Virtual Memory paging analogy for context window management
 *
 * In a traditional OS, context switch saves/restores CPU registers.
 * In an Agent OS, context switch saves/restores:
 *   - Conversation history (messages)
 *   - System prompt + active tools
 *   - Pending tool call results
 *   - Agent-local working memory
 *
 * This module provides:
 *   1. Context Snapshot: serialize agent state to a compact representation
 *   2. Context Restore: reload a previously suspended agent's state
 *   3. Context Compression: summarize old messages to free token budget
 *   4. Context Isolation: agents cannot read each other's contexts
 *   5. Context Paging: swap inactive contexts to disk, reload on demand
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sparx::os {

// ─── Context Types ──────────────────────────────────────────────────────────

using ContextHandle = uint64_t;

/// A single message in the conversation context.
struct ContextMessage {
    enum class Role : uint8_t { System, User, Assistant, Tool };
    Role role;
    std::string content;
    std::string tool_call_id;       // For tool responses
    std::string name;               // Tool/function name
    int64_t timestamp_ms = 0;       // When this message was added
    uint32_t token_count = 0;       // Estimated tokens for this message
    bool pinned = false;            // Pinned messages survive compression
};

/// Snapshot of an agent's full execution context.
struct ContextSnapshot {
    ContextHandle handle = 0;
    std::string agent_name;
    std::vector<ContextMessage> messages;
    std::string system_prompt;
    std::vector<std::string> active_tools;    // Currently available tools
    std::map<std::string, std::string> metadata; // Agent-local KV store
    uint32_t total_tokens = 0;                // Current token usage
    uint32_t max_tokens = 0;                  // Context window limit
    int64_t last_active_ms = 0;               // Last interaction timestamp
    uint32_t turn_count = 0;                  // Number of turns completed
    bool compressed = false;                  // Has been through compression
};

/// A page of context (for paging to disk).
struct ContextPage {
    ContextHandle handle = 0;
    std::string serialized_data;     // Compact binary/JSON representation
    uint32_t original_tokens = 0;    // Token count before paging
    int64_t paged_at_ms = 0;         // When it was swapped out
    std::string disk_path;           // Where on disk it was stored
    bool dirty = false;              // Modified since last page-out
};

/// Compression strategy for context window management.
enum class CompressionStrategy : uint8_t {
    /// Summarize oldest N messages into a single summary message.
    Summarize,
    /// Drop oldest non-pinned messages (sliding window).
    SlidingWindow,
    /// Keep only pinned messages + last K turns.
    PinnedPlusRecent,
    /// Hierarchical: summarize in levels (recent=full, old=summary, ancient=keywords).
    Hierarchical,
};

// ─── Context Manager Configuration ─────────────────────────────────────────

struct ContextManagerConfig {
    /// Maximum concurrent contexts in memory.
    uint32_t max_resident_contexts = 8;
    /// Context window size (tokens) before triggering compression.
    uint32_t compression_threshold = 3072;  // 75% of typical 4096 window
    /// Minimum messages to keep after compression.
    uint32_t min_messages_after_compress = 4;
    /// Default compression strategy.
    CompressionStrategy strategy = CompressionStrategy::Hierarchical;
    /// Directory for paged-out contexts.
    std::string page_directory;  // defaults to ~/.sparx/contexts/
    /// Auto-page inactive contexts after this duration (ms, 0 = never).
    int64_t auto_page_timeout_ms = 300000;  // 5 minutes
    /// Token estimation ratio (chars per token, approximate).
    float chars_per_token = 3.5f;
};

// ─── Context Manager ────────────────────────────────────────────────────────

/**
 * @brief Manages context switching and memory for concurrent Agents.
 *
 * Lifecycle:
 *   1. createContext(agent_name, system_prompt) → ContextHandle
 *   2. appendMessage(handle, message) — add messages during execution
 *   3. snapshot(handle) — save current state (before context switch)
 *   4. restore(handle) — reload state (after switch back)
 *   5. compress(handle) — reclaim token space when context is full
 *   6. pageOut(handle) — swap to disk for inactive agents
 *   7. pageIn(handle) — reload from disk when agent is rescheduled
 *   8. destroy(handle) — cleanup when agent terminates
 *
 * Thread-safe. Contexts are isolated — no cross-agent reads without explicit
 * sharing through the Memory Manager.
 */
class ContextManager {
public:
    explicit ContextManager(ContextManagerConfig config = {});
    ~ContextManager();

    // ── Lifecycle ──

    /// Create a new context for an agent. Returns handle.
    ContextHandle create(const std::string& agent_name,
                         const std::string& system_prompt,
                         uint32_t max_tokens = 4096);

    /// Destroy a context (agent terminated).
    void destroy(ContextHandle handle);

    // ── Message Management ──

    /// Append a message to the context.
    void appendMessage(ContextHandle handle, ContextMessage msg);

    /// Get current messages (for building inference request).
    std::vector<ContextMessage> getMessages(ContextHandle handle) const;

    /// Get token usage for a context.
    uint32_t tokenUsage(ContextHandle handle) const;

    /// Check if context needs compression.
    bool needsCompression(ContextHandle handle) const;

    // ── Context Switching ──

    /// Snapshot the current context state (for suspend/switch).
    std::optional<ContextSnapshot> snapshot(ContextHandle handle) const;

    /// Restore a context from a snapshot (for resume/switch-back).
    bool restore(ContextHandle handle, const ContextSnapshot& snap);

    // ── Compression ──

    /// Compress the context to reclaim tokens.
    /// Uses the configured strategy. Returns tokens reclaimed.
    uint32_t compress(ContextHandle handle);

    /// Compress with a specific strategy.
    uint32_t compress(ContextHandle handle, CompressionStrategy strategy);

    /// Set a custom summarizer function (for Summarize strategy).
    using SummarizerFn = std::function<std::string(
        const std::vector<ContextMessage>& messages_to_summarize)>;
    void setSummarizer(SummarizerFn fn);

    // ── Paging ──

    /// Page out an inactive context to disk. Frees memory.
    bool pageOut(ContextHandle handle);

    /// Page in a context from disk. Must have been previously paged out.
    bool pageIn(ContextHandle handle);

    /// Check if a context is currently paged out.
    bool isPagedOut(ContextHandle handle) const;

    // ── Metadata ──

    /// Set agent-local metadata (key-value store within context).
    void setMetadata(ContextHandle handle,
                     const std::string& key, const std::string& value);

    /// Get agent-local metadata.
    std::optional<std::string> getMetadata(ContextHandle handle,
                                           const std::string& key) const;

    // ── Tool Management ──

    /// Set the active tools for a context.
    void setActiveTools(ContextHandle handle,
                        const std::vector<std::string>& tools);

    /// Get active tools.
    std::vector<std::string> getActiveTools(ContextHandle handle) const;

    // ── Stats ──

    struct Stats {
        uint32_t total_contexts = 0;
        uint32_t resident_contexts = 0;   // In memory
        uint32_t paged_contexts = 0;      // On disk
        uint64_t total_tokens_managed = 0;
        uint64_t compressions_performed = 0;
        uint64_t tokens_reclaimed = 0;
        uint64_t page_outs = 0;
        uint64_t page_ins = 0;
    };
    Stats stats() const;

    // ── Maintenance ──

    /// Auto-page inactive contexts (called by scheduler tick).
    void maintenance();

private:
    ContextManagerConfig config_;
    mutable std::mutex mutex_;
    ContextHandle next_handle_ = 1;

    // Resident contexts (in memory)
    std::map<ContextHandle, ContextSnapshot> resident_;

    // Paged-out contexts (metadata only, content on disk)
    std::map<ContextHandle, ContextPage> paged_;

    mutable Stats stats_;
    SummarizerFn summarizer_;

    // ── Internal ──

    /// Estimate token count for a message.
    uint32_t estimateTokens(const std::string& text) const;

    /// Serialize a context to compact format for disk storage.
    std::string serialize(const ContextSnapshot& ctx) const;

    /// Deserialize a context from disk format.
    ContextSnapshot deserialize(const std::string& data) const;

    /// Perform hierarchical compression.
    uint32_t compressHierarchical(ContextSnapshot& ctx);

    /// Perform sliding window compression.
    uint32_t compressSlidingWindow(ContextSnapshot& ctx);

    /// Perform summarize compression.
    uint32_t compressSummarize(ContextSnapshot& ctx);

    /// Perform pinned+recent compression.
    uint32_t compressPinnedRecent(ContextSnapshot& ctx);
};

}  // namespace sparx::os
