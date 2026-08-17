#pragma once
/**
 * @file sparx_memory_manager.h
 * @brief Memory Manager — Hierarchical memory system for Agent OS.
 *
 * Research basis:
 *   - MemGPT: Towards LLMs as Operating Systems (arXiv:2310.08560)
 *   - AIOS Memory Management (arXiv:2403.16971)
 *   - "Cognitive Architectures for Language Agents" (CoALA, arXiv:2309.02427)
 *   - Atkinson-Shiffrin memory model (sensory → short-term → long-term)
 *
 * Three-tier memory hierarchy (analogous to CPU cache / RAM / Disk):
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │  Working Memory (L1)                                        │
 *   │  = Current context window tokens                            │
 *   │  Capacity: context_length tokens (~4K-128K)                 │
 *   │  Latency: 0 (already in prompt)                             │
 *   │  Eviction: managed by Context Manager compression           │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │  Episodic Memory (L2)                                       │
 *   │  = Recent interaction history with TTL decay                │
 *   │  Capacity: last N sessions / last T hours                   │
 *   │  Latency: ~1ms (in-process, indexed)                       │
 *   │  Eviction: exponential decay, LRU within decay tier         │
 *   ├─────────────────────────────────────────────────────────────┤
 *   │  Semantic Memory (L3)                                       │
 *   │  = Long-term knowledge, vectorized for retrieval            │
 *   │  Capacity: unbounded (disk-backed)                          │
 *   │  Latency: ~5-50ms (vector search)                           │
 *   │  Eviction: importance scoring, periodic compaction          │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Memory operations:
 *   - store(fact, tier)    — write to a specific tier
 *   - recall(query, k)    — retrieve top-k relevant memories across tiers
 *   - consolidate()       — promote episodic → semantic (like sleep consolidation)
 *   - forget(criteria)    — explicit deletion (privacy, corrections)
 *   - page(L2 → L3)      — background migration of aging episodic memories
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sparx::os {

// ─── Memory Entry ───────────────────────────────────────────────────────────

/// Memory tier classification.
enum class MemoryTier : uint8_t {
    Working,    // L1: in-context, managed by ContextManager
    Episodic,   // L2: recent history, in-process indexed
    Semantic,   // L3: long-term vectorized knowledge
};

/// Importance score (determines retention priority).
enum class Importance : uint8_t {
    Trivial   = 0,   // Routine greetings, filler
    Normal    = 1,   // Standard interactions
    Notable   = 2,   // User preferences, corrections
    Critical  = 3,   // Explicit instructions, safety rules
};

/// A single memory entry.
struct MemoryEntry {
    uint64_t id = 0;
    std::string content;             // The fact/observation
    std::string source;              // Where it came from (agent, user, tool)
    std::string agent_id;            // Which agent stored this
    MemoryTier tier = MemoryTier::Episodic;
    Importance importance = Importance::Normal;

    // ── Temporal metadata ──
    int64_t created_at_ms = 0;
    int64_t last_accessed_ms = 0;
    uint32_t access_count = 0;
    float decay_weight = 1.0f;       // Current weight after decay

    // ── Embedding (for semantic search) ──
    std::vector<float> embedding;    // Dense vector (dim varies by model)

    // ── Tags for structured recall ──
    std::vector<std::string> tags;
    std::map<std::string, std::string> metadata;
};

/// Result of a recall (memory retrieval) operation.
struct RecallResult {
    MemoryEntry entry;
    float relevance_score = 0.0f;    // Cosine similarity or BM25 score
    MemoryTier source_tier;
};

// ─── Memory Manager Configuration ──────────────────────────────────────────

struct MemoryManagerConfig {
    /// Maximum episodic entries (L2) before compaction.
    uint32_t max_episodic_entries = 1000;
    /// Maximum semantic entries (L3) before compaction.
    uint32_t max_semantic_entries = 10000;
    /// Episodic TTL: entries older than this decay to 0 weight (hours).
    float episodic_ttl_hours = 168.0f;  // 7 days
    /// Decay lambda (exponential): weight = exp(-lambda * age_hours).
    float decay_lambda = 0.01f;
    /// Embedding dimension (0 = no embeddings, text-only search).
    uint32_t embedding_dim = 64;
    /// Consolidation threshold: promote episodic → semantic if accessed N+ times.
    uint32_t consolidation_access_threshold = 3;
    /// Minimum importance for semantic promotion.
    Importance min_importance_for_semantic = Importance::Normal;
    /// Persistence directory.
    std::string storage_path;  // defaults to ~/.sparx/memory/
    /// Auto-consolidate interval (seconds, 0 = manual only).
    uint32_t consolidation_interval_s = 3600;  // Every hour
};

// ─── Memory Manager ─────────────────────────────────────────────────────────

/**
 * @brief Hierarchical memory system for Agent OS.
 *
 * Provides persistent, searchable memory across agent sessions.
 * Each agent has isolated memory by default; shared memories require
 * explicit grants through the Access Control system.
 *
 * Thread-safe. Background consolidation runs periodically.
 */
class MemoryManager {
public:
    explicit MemoryManager(MemoryManagerConfig config = {});
    ~MemoryManager();

    /// Start background consolidation.
    void start();
    /// Stop background consolidation.
    void stop();

    // ── Store ──

    /// Store a new memory. Returns its ID.
    uint64_t store(const std::string& content, MemoryTier tier,
                   Importance importance = Importance::Normal,
                   const std::string& agent_id = "",
                   const std::vector<std::string>& tags = {});

    /// Store with pre-computed embedding.
    uint64_t store(MemoryEntry entry);

    // ── Recall ──

    /// Recall top-k memories relevant to a query (searches all tiers).
    std::vector<RecallResult> recall(const std::string& query,
                                     uint32_t top_k = 5,
                                     const std::string& agent_id = "") const;

    /// Recall by tag.
    std::vector<RecallResult> recallByTag(const std::string& tag,
                                          uint32_t top_k = 10) const;

    /// Recall recent episodic memories (chronological).
    std::vector<MemoryEntry> recentEpisodic(uint32_t count = 10,
                                            const std::string& agent_id = "") const;

    // ── Maintenance ──

    /// Consolidate: promote qualifying episodic → semantic.
    uint32_t consolidate();

    /// Apply decay weights to all episodic memories.
    void applyDecay();

    /// Compact: remove entries below weight threshold.
    uint32_t compact(float min_weight = 0.01f);

    /// Forget: explicitly remove memories matching criteria.
    uint32_t forget(const std::string& agent_id = "",
                    const std::vector<std::string>& tags = {});

    // ── Persistence ──

    /// Save all memory to disk.
    void save() const;

    /// Load memory from disk.
    void load();

    // ── Stats ──

    struct Stats {
        uint32_t episodic_count = 0;
        uint32_t semantic_count = 0;
        uint64_t total_recalls = 0;
        uint64_t consolidations = 0;
        uint64_t entries_forgotten = 0;
        float avg_recall_latency_ms = 0.0f;
    };
    Stats stats() const;

private:
    MemoryManagerConfig config_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread consolidation_thread_;
    uint64_t next_id_ = 1;

    // L2: Episodic memory (in-process)
    std::vector<MemoryEntry> episodic_;

    // L3: Semantic memory (in-process, disk-backed)
    std::vector<MemoryEntry> semantic_;

    mutable Stats stats_;

    // ── Internal ──

    /// Compute embedding for text (SimHash or delegate to external model).
    std::vector<float> computeEmbedding(const std::string& text) const;

    /// Cosine similarity between two embeddings.
    float cosineSim(const std::vector<float>& a,
                    const std::vector<float>& b) const;

    /// BM25-like text scoring (fallback when no embeddings).
    float textScore(const std::string& query,
                    const std::string& content) const;

    /// Background consolidation loop.
    void consolidationLoop();
};

}  // namespace sparx::os
