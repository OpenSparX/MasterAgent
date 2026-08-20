/**
 * @file sparx_speculative.h
 * @brief Speculative Agent Execution — predictive pre-computation on idle NPU.
 *
 * Research basis:
 *   - SLED: Speculative LLM Decoding for Efficient Edge Serving (2506.09397)
 *   - Delay-Adaptive Speculation Control for Edge-Cloud (2606.20591)
 *   - Distributed Speculative Decoding (2512.16273)
 *
 * Core insight: On-device agents have a unique advantage over cloud APIs —
 * they can observe user patterns and speculatively pre-compute likely next
 * requests during NPU idle time. When the user's actual request matches a
 * speculation, the response is instant (0ms inference latency).
 *
 * Architecture:
 *
 *   ┌─────────────┐    ┌──────────────┐    ┌────────────────┐
 *   │ Intent      │───▶│ Speculation  │───▶│ Speculative    │
 *   │ Predictor   │    │ Cache        │    │ Executor       │
 *   │ (Markov/    │    │ (LRU, TTL,   │    │ (idle NPU,     │
 *   │  n-gram)    │    │  confidence) │    │  priority < P1)│
 *   └─────────────┘    └──────────────┘    └────────────────┘
 *         │                    │                     │
 *         │                    ▼                     │
 *         │            ┌──────────────┐              │
 *         └───────────▶│ Speculation  │◀─────────────┘
 *                      │ Validator    │
 *                      │ (seal check, │
 *                      │  freshness)  │
 *                      └──────────────┘
 *
 * Guarantees:
 *   1. Correctness: speculative results are NEVER committed without full seal
 *      validation. If user context changed, speculation is discarded.
 *   2. Non-interference: speculation runs at P3 priority (below all real work)
 *      and is preempted immediately when real inference is needed.
 *   3. Privacy: predictions are derived from local history only.
 *   4. Measurability: hit rate, latency savings, and NPU utilization are tracked.
 *
 * Prediction models (ordered by sophistication):
 *   Level 1: Bigram frequency — P(next_intent | current_intent)
 *   Level 2: Trigram + time-of-day — P(intent | prev_2_intents, hour)
 *   Level 3: Recurrent context — lightweight LSTM on intent sequence embeddings
 *   Level 4: Mamba-2 SSM — selective state space model for long-range patterns
 *
 * The prediction model trains online from the user's own interaction history,
 * stored in ~/.sparx/speculation/history.jsonl (encrypted, same key as learning).
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sparx::speculation {

// ---------------------------------------------------------------------------
// Intent Prediction
// ---------------------------------------------------------------------------

/// A recorded intent from user interaction history.
struct IntentRecord {
    std::string intent_name;        // skill or inference category
    std::string raw_input;          // user's actual text
    std::int64_t timestamp_utc;     // when it happened
    std::uint8_t hour_of_day;       // 0-23, local time
    std::uint8_t day_of_week;       // 0-6, Mon=0
    bool was_deterministic;         // resolved without model?
    std::uint32_t latency_ms;       // actual response time
};

/// A prediction of what the user will likely ask next.
struct IntentPrediction {
    std::string predicted_intent;   // skill name or category
    float confidence;               // 0.0 – 1.0
    std::string predicted_input;    // most likely phrasing (for cache key)
    std::string rationale;          // why this was predicted (for debugging)
};

/// Configures the prediction engine.
struct PredictionConfig {
    /// Minimum confidence to trigger speculation (0.0 – 1.0).
    /// Higher = fewer speculations but higher hit rate.
    float min_confidence = 0.6f;
    /// Maximum number of concurrent speculations.
    std::uint32_t max_concurrent = 3;
    /// History window for n-gram models (number of recent intents).
    std::uint32_t history_window = 50;
    /// Time-of-day weighting factor.
    float temporal_weight = 0.3f;
    /// Minimum interactions before predictions begin.
    std::uint32_t cold_start_threshold = 10;
    /// History file path. Encrypted with device key.
    std::string history_path;  // defaults to ~/.sparx/speculation/history.jsonl

    // --- Level 3 LSTM ensemble weights ---
    /// Weight for bigram+temporal (Level 1&2) in the ensemble.
    float ngram_weight = 0.5f;
    /// Weight for LSTM (Level 3) in the ensemble.
    float lstm_weight = 0.3f;
    /// Weight for Mamba-2 SSM (Level 4) in the ensemble.
    float mamba_weight = 0.2f;
    /// Minimum history length before LSTM predictions are used.
    std::uint32_t lstm_min_history = 10;
    /// LSTM learning rate for online SGD.
    float lstm_learning_rate = 0.01f;
    /// Gradient clipping norm for LSTM training.
    float lstm_grad_clip = 5.0f;
    /// Path for LSTM binary weights file.
    std::string lstm_weights_path;  // defaults to ~/.sparx/speculation/lstm.bin

    // --- Level 4 Mamba-2 SSM parameters ---
    /// Minimum history length before Mamba-2 predictions are used.
    std::uint32_t mamba_min_history = 8;
    /// Mamba-2 input dimension (must match kEmbeddingDim).
    std::uint32_t mamba_input_dim = 64;
    /// Mamba-2 state dimension for the SSM.
    std::uint32_t mamba_state_dim = 32;
    /// Mamba-2 learning rate for online truncated BPTT.
    float mamba_learning_rate = 0.005f;
    /// Truncated BPTT sequence length for Mamba-2 online training.
    std::uint32_t mamba_bptt_length = 16;
    /// Path for Mamba-2 binary weights file.
    std::string mamba_weights_path;  // defaults to ~/.sparx/speculation/mamba.bin
};

// ---------------------------------------------------------------------------
// Embedding Index (lightweight on-device text similarity)
// ---------------------------------------------------------------------------

/// Embedding dimension — a 64-dim SimHash is sufficient for short input texts
/// while fitting comfortably in L1 cache (64 × 4 = 256 bytes per vector).
constexpr size_t kEmbeddingDim = 64;

/// A dense embedding vector (fixed-size for cache-friendliness).
using EmbeddingVec = std::array<float, kEmbeddingDim>;

// ---------------------------------------------------------------------------
// NPU Inference Backend Dispatch Interface
// ---------------------------------------------------------------------------

/// Callback for dispatching pre-computed results to the NPU inference backend.
/// Called when speculation confidence exceeds the dispatch threshold (default 0.8).
/// Parameters:
///   - prompt: the predicted user input
///   - cached_result: the pre-computed speculative output
///   - confidence: prediction confidence [0.0, 1.0]
/// Returns true if the backend accepted the dispatch (KV cache pre-warmed).
using NpuDispatchFn = std::function<bool(const std::string& prompt,
                                         const std::string& cached_result,
                                         float confidence)>;

/**
 * @brief Lightweight on-device text embedder using SimHash over character n-grams.
 *
 * No external model dependency — pure CPU computation at ~1us per embedding.
 * Uses locality-sensitive hashing: similar texts produce similar vectors.
 *
 * Algorithm:
 *   1. Extract all character trigrams from normalized input
 *   2. Hash each trigram to kEmbeddingDim random projections (deterministic)
 *   3. Accumulate weighted projections (TF weighting by trigram frequency)
 *   4. L2-normalize the resulting vector
 *
 * This is a SimHash variant (Charikar 2002) adapted for short agent commands.
 * Empirically achieves >0.9 cosine similarity for paraphrases of the same intent
 * and <0.6 for semantically different commands.
 */
class EmbeddingIndex {
public:
    /// HNSW parameters for approximate nearest neighbor search.
    struct HnswParams {
        size_t M = 16;                  // max connections per node per layer
        size_t efConstruction = 200;    // search width during construction
        size_t efSearch = 50;           // search width during query
        // Probability of layer assignment: P = 1/ln(M)
        double levelMultiplier() const { return 1.0 / std::log(static_cast<double>(M)); }
    };

    EmbeddingIndex() = default;
    explicit EmbeddingIndex(HnswParams params) : hnsw_params_(params) {}

    /// Compute embedding for a text input.
    EmbeddingVec embed(const std::string& text) const;

    /// Cosine similarity between two embedding vectors [-1, 1].
    static float cosineSimilarity(const EmbeddingVec& a, const EmbeddingVec& b);

    /// Store an embedding with associated cache key. Uses HNSW for indexing.
    void insert(const std::string& cache_key, const EmbeddingVec& vec);

    /// Find the nearest neighbor above threshold using HNSW graph traversal.
    struct NearestResult {
        std::string cache_key;
        float similarity = 0.0f;
    };
    std::optional<NearestResult> findNearest(
        const EmbeddingVec& query, float threshold) const;

    /// Remove an entry by cache key.
    void remove(const std::string& cache_key);

    /// Clear all entries and HNSW graph.
    void clear();

    /// Number of indexed entries.
    size_t size() const { return nodes_.size(); }

private:
    /// Hash a trigram to a deterministic random projection vector.
    static EmbeddingVec hashTrigram(const std::string& trigram);

    /// Normalize text for embedding (lowercase, strip punctuation, collapse whitespace).
    static std::string normalizeForEmbedding(const std::string& text);

    // --- HNSW graph structures ---

    /// A node in the HNSW graph. Each node exists on layers [0, level].
    struct HnswNode {
        std::string cache_key;
        EmbeddingVec vec;
        int level = 0;  // max layer this node belongs to
        // Connections per layer: neighbors[layer] = list of node indices
        std::vector<std::vector<size_t>> neighbors;
    };

    HnswParams hnsw_params_;
    std::vector<HnswNode> nodes_;
    int max_level_ = -1;        // current max layer in the graph
    size_t entry_point_ = 0;    // index of the entry point node

    // Map from cache_key to node index for O(1) removal lookup
    std::map<std::string, size_t> key_to_index_;

    /// Assign a random level to a new node (geometric distribution).
    int randomLevel() const;

    /// Greedy search on a single layer: from entry, find ef closest nodes to query.
    /// Returns vector of (node_index, similarity) sorted descending by similarity.
    std::vector<std::pair<size_t, float>> searchLayer(
        const EmbeddingVec& query, size_t entry_id, size_t ef, int layer) const;

    /// Select neighbors with heuristic pruning (Algorithm 4 from HNSW paper).
    /// Picks up to M neighbors that are both close to the query AND well-distributed.
    std::vector<size_t> selectNeighborsHeuristic(
        const EmbeddingVec& query, const std::vector<std::pair<size_t, float>>& candidates,
        size_t M) const;

    // Legacy flat storage for backward compatibility (used during removal)
    struct IndexEntry {
        std::string cache_key;
        EmbeddingVec vec;
    };
};

/**
 * @brief Predicts user's next intent from interaction history.
 *
 * Uses a multi-level model:
 *   1. Bigram: P(B|A) from transition counts
 *   2. Temporal: P(B|A, hour) — time-weighted bigram
 *   3. Sequence: P(B|A_{t-2}, A_{t-1}, hour, day) — LSTM on embeddings
 *   4. SSM: Mamba-2 selective state space model — captures long-range patterns
 *
 * The final prediction is a weighted ensemble of all levels.
 */
class LstmPredictor;   // Defined in sparx_speculative.cpp
class MambaPredictor;  // Defined in sparx_speculative.cpp

// ---------------------------------------------------------------------------
// NpuScanDispatcher — stub interface for future QNN delegate binding
// ---------------------------------------------------------------------------

/**
 * @brief NPU dispatch interface for SSD parallel scan offloading.
 *
 * When available, the structured state space duality (SSD) scan can be
 * dispatched to a Qualcomm QNN delegate for hardware-accelerated parallel
 * prefix computation. This stub provides the interface contract; the actual
 * binding is provided by the platform-specific NPU backend.
 */
class NpuScanDispatcher {
public:
    virtual ~NpuScanDispatcher() = default;

    /// Returns true if an NPU backend capable of scan offload is present.
    virtual bool isAvailable() const = 0;

    /// Dispatches a chunk of the parallel scan to the NPU.
    /// @param decay_data  Pointer to decay values [chunk_size * state_dim], row-major.
    /// @param input_data  Pointer to B_bar values [chunk_size * state_dim], row-major.
    /// @param h_init      Pointer to initial state [state_dim].
    /// @param chunk_size  Number of timesteps in this chunk.
    /// @param state_dim   Dimension of the SSM hidden state.
    /// @param out_states  Output pointer for computed states [(chunk_size+1) * state_dim].
    virtual void dispatchChunk(const float* decay_data,
                               const float* input_data,
                               const float* h_init,
                               size_t chunk_size,
                               size_t state_dim,
                               float* out_states) = 0;

    /// Blocks until all dispatched chunks have completed.
    virtual void syncResults() = 0;
};

/// Benchmark result comparing sequential vs parallel scan.
struct ScanBenchmark {
    double sequential_us = 0.0;   // microseconds for sequential path
    double parallel_us = 0.0;     // microseconds for SSD parallel path
    double speedup() const {
        return parallel_us > 0.0 ? sequential_us / parallel_us : 0.0;
    }
};

class IntentPredictor {
public:
    explicit IntentPredictor(PredictionConfig config = {});
    ~IntentPredictor();

    /// Records a new intent observation (called after each user turn).
    void observe(const IntentRecord& record);

    /// Predicts the top-k most likely next intents.
    std::vector<IntentPrediction> predict(std::uint32_t top_k = 3) const;

    /// Returns true if enough history exists for meaningful predictions.
    bool isWarmedUp() const;

    /// Total number of observations recorded.
    std::uint32_t observationCount() const { return observation_count_; }

    /// Prediction accuracy over last N predictions.
    float recentHitRate(std::uint32_t window = 20) const;

    /// Persists model state to disk.
    void save() const;

    /// Loads model state from disk.
    void load();

private:
    PredictionConfig config_;

    // Weighted transition counts with exponential decay: transitions_weighted_[A][B] = weight
    std::map<std::string, std::map<std::string, float>> transitions_weighted_;

    // Temporal bigram with decay: temporal_weighted_[hour][A][B] = weight
    std::map<std::uint8_t,
        std::map<std::string, std::map<std::string, float>>>
        temporal_transitions_weighted_;

    // Trigram with decay: trigram_weighted_[A+B][C] = weight
    std::map<std::string, std::map<std::string, float>> trigrams_weighted_;

    // Recent history for context
    std::deque<IntentRecord> recent_history_;

    // Accuracy tracking
    mutable std::deque<bool> prediction_hits_;
    std::uint32_t observation_count_ = 0;

    // Intent → most common phrasing (for pre-generating prompts)
    std::map<std::string, std::string> canonical_phrasing_;

    // Level 3: Lightweight LSTM intent predictor
    mutable std::unique_ptr<LstmPredictor> lstm_;
    // Level 4: Mamba-2 selective state space model predictor
    mutable std::unique_ptr<MambaPredictor> mamba_;
    EmbeddingIndex intent_embedder_;  // shared embedder for intent names
};

// ---------------------------------------------------------------------------
// Speculation Cache
// ---------------------------------------------------------------------------

/// A cached speculative result, ready for instant delivery.
struct SpeculativeResult {
    std::string cache_key;          // intent + normalized input hash
    std::string raw_output;         // pre-computed model response
    std::string intent_name;        // which intent this serves
    float prediction_confidence;    // how confident was the prediction
    std::int64_t computed_at_utc;   // when speculation was computed
    std::int64_t ttl_seconds;       // time-to-live (stale after this)
    std::uint32_t token_count;      // output size
    std::string model_id;           // model used for speculation
    std::string context_hash;       // hash of conversation state at compute time
    bool validated = false;         // set true when seal-checked at delivery
};

/// Configuration for the speculation cache.
struct CacheConfig {
    /// Maximum cached speculations.
    std::uint32_t max_entries = 16;
    /// Default TTL for cached results (seconds).
    std::int64_t default_ttl_seconds = 300;  // 5 minutes
    /// Maximum cache memory (bytes). Entries evicted LRU when exceeded.
    std::size_t max_memory_bytes = 4 * 1024 * 1024;  // 4 MB
    /// Invalidate on context change (new skill added, model changed, etc.)
    bool invalidate_on_context_change = true;
    /// Enable embedding-based similarity matching (fuzzy cache hit).
    bool enable_similarity_match = true;
    /// Minimum cosine similarity for fuzzy hit (0.0 = any match, 1.0 = exact).
    float similarity_threshold = 0.85f;
};

/**
 * @brief LRU cache for speculative computation results.
 *
 * Thread-safe. Entries expire by TTL or context-hash mismatch.
 * A cache hit avoids the full inference path (0ms model latency).
 */
class SpeculationCache {
public:
    explicit SpeculationCache(CacheConfig config = {});

    /// Stores a speculative result. May evict oldest entry.
    void put(SpeculativeResult result);

    /// Looks up a speculation for the given intent/input. Returns nullopt on miss.
    std::optional<SpeculativeResult> get(const std::string& intent,
                                          const std::string& input_hash,
                                          const std::string& context_hash) const;

    /// Invalidates all entries (e.g., on model change).
    void invalidateAll();

    /// Invalidates entries for a specific intent.
    void invalidate(const std::string& intent);

    /// Cache statistics.
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t similarity_hits = 0;  // fuzzy matches via embedding
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t expirations = 0;
        std::uint32_t current_entries = 0;
        std::size_t current_memory = 0;
        float hit_rate() const {
            auto total = hits + misses;
            return total > 0 ? static_cast<float>(hits) / total : 0.0f;
        }
    };
    Stats stats() const;

private:
    mutable std::mutex mutex_;
    CacheConfig config_;
    std::deque<SpeculativeResult> entries_;
    mutable Stats stats_;
    EmbeddingIndex embedding_index_;  // similarity search index

    /// Map from input text to its embedding (computed at put time).
    std::map<std::string, EmbeddingVec> embeddings_;
};

// ---------------------------------------------------------------------------
// Speculative Executor
// ---------------------------------------------------------------------------

/// Priority level for speculative work (always below real requests).
enum class SpeculationPriority : std::uint8_t {
    /// Run only when NPU is completely idle (no real work pending).
    IdleOnly = 0,
    /// Run when NPU utilization < 20%.
    LowLoad = 1,
    /// Run when NPU utilization < 50% (aggressive, not recommended).
    MediumLoad = 2,
};

struct ExecutorConfig {
    SpeculationPriority priority = SpeculationPriority::IdleOnly;
    /// Maximum tokens to generate per speculation (caps compute cost).
    std::uint32_t max_speculation_tokens = 256;
    /// Cancel speculation if real request arrives within this window.
    std::chrono::milliseconds preemption_grace_ms{50};
    /// Maximum time to spend on one speculation.
    std::chrono::milliseconds max_speculation_time_ms{5000};
    /// Confidence threshold above which we dispatch to NPU for KV cache pre-warming.
    float npu_dispatch_threshold = 0.8f;
    /// Optional NPU dispatch callback. When set and confidence > threshold,
    /// pre-warms the inference backend's KV cache with the speculative result.
    NpuDispatchFn npu_dispatch_fn = nullptr;
};

/// Metrics for speculation effectiveness.
struct SpeculationMetrics {
    std::uint64_t total_speculations = 0;
    std::uint64_t successful_hits = 0;
    std::uint64_t preempted = 0;
    std::uint64_t expired = 0;
    std::uint64_t context_mismatched = 0;
    std::uint64_t npu_dispatches = 0;        // successful NPU pre-warm dispatches
    std::uint64_t npu_dispatch_failures = 0; // dispatch attempts that were rejected
    float avg_latency_saved_ms = 0.0f;
    float npu_idle_utilization_pct = 0.0f;
    float hit_rate() const {
        return total_speculations > 0
            ? static_cast<float>(successful_hits) / total_speculations
            : 0.0f;
    }
};

/**
 * @brief Drives speculative pre-computation on idle NPU cycles.
 *
 * Lifecycle:
 *   1. After each user turn, IntentPredictor produces top-k predictions
 *   2. Executor checks which predictions are not already cached
 *   3. During idle time, executor runs inference for predicted intents
 *   4. Results stored in SpeculationCache with TTL and context hash
 *   5. On next user turn, cache is checked BEFORE real inference
 *   6. On hit: instant response + seal validation
 *   7. On miss: normal inference path (no degradation)
 *
 * The executor coordinates with the inference framework's preemption system:
 * speculative jobs use P3 priority and are instantly preempted for real P1 work.
 */
class SpeculativeExecutor {
public:
    using InferenceCallback = std::function<std::optional<std::string>(
        const std::string& prompt, std::uint32_t max_tokens)>;

    SpeculativeExecutor(IntentPredictor& predictor,
                        SpeculationCache& cache,
                        ExecutorConfig config = {});

    ~SpeculativeExecutor();

    /// Called after each user turn. Triggers prediction + async speculation.
    void afterTurn(const IntentRecord& record,
                   const std::string& context_hash,
                   InferenceCallback infer_fn);

    /// Check cache for a speculation matching the user's actual input.
    /// Called at the start of each turn, before real inference.
    std::optional<SpeculativeResult> checkHit(
        const std::string& intent,
        const std::string& input_hash,
        const std::string& context_hash) const;

    /// Signals that real inference is starting (preempts active speculation).
    void preempt();

    /// Sets or replaces the NPU dispatch callback at runtime.
    void setNpuDispatchFn(NpuDispatchFn fn);

    /// Returns speculation effectiveness metrics.
    SpeculationMetrics metrics() const;

    /// Resets all metrics (for benchmarking).
    void resetMetrics();

    /// Check if the system is idle enough for speculation.
    bool isSystemIdle() const;

private:
    IntentPredictor& predictor_;
    SpeculationCache& cache_;
    ExecutorConfig config_;
    mutable std::mutex mutex_;
    mutable SpeculationMetrics metrics_;
    std::atomic<bool> active_speculation_{false};
    std::atomic<bool> preempt_requested_{false};
    std::atomic<bool> shutdown_{false};

    // Async worker
    struct SpeculationTask {
        std::string predicted_input;
        std::string intent_name;
        float confidence;
        std::string context_hash;
        InferenceCallback infer_fn;
    };
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<SpeculationTask> task_queue_;
    std::thread worker_thread_;

    void workerLoop();
    void executeTask(const SpeculationTask& task);
    static float querySystemLoad();
    static float queryBatteryLevel();
    static bool queryIsCharging();
};

// ---------------------------------------------------------------------------
// Integration helpers
// ---------------------------------------------------------------------------

/// Computes a context hash for invalidation (model + skills + session state).
std::string computeContextHash(const std::string& model_path,
                               const std::vector<std::string>& skills,
                               std::uint64_t session_turn);

/// Normalizes user input for cache key matching (lowercase, trim, collapse ws).
std::string normalizeForCacheKey(const std::string& input);

}  // namespace sparx::speculation
