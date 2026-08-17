/**
 * @file sparx_speculative.cpp
 * @brief Speculative Agent Execution — implementation.
 *
 * This implements the full speculation pipeline:
 *   Intent prediction → cache management → idle-time execution → hit delivery
 */

#include "sparx_speculative.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <sstream>

namespace sparx::speculation {

namespace {

int64_t nowUtcSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trigramKey(const std::string& a, const std::string& b) {
    return a + "|" + b;
}

}  // namespace

// ---------------------------------------------------------------------------
// IntentPredictor
// ---------------------------------------------------------------------------

IntentPredictor::IntentPredictor(PredictionConfig config)
    : config_(std::move(config)) {}

void IntentPredictor::observe(const IntentRecord& record) {
    ++observation_count_;

    // Update canonical phrasing (most recent wins)
    canonical_phrasing_[record.intent_name] = record.raw_input;

    // Exponential decay factor: λ = 0.1, age in days
    // Older observations count less: weight = exp(-0.1 * age_days)
    const double decay_lambda = 0.1;
    int64_t now = record.timestamp_utc;

    // Update bigram transitions with decay
    if (!recent_history_.empty()) {
        const auto& prev = recent_history_.back().intent_name;

        // Compute age-weighted increment
        int64_t age_seconds = now - recent_history_.back().timestamp_utc;
        double age_days = static_cast<double>(age_seconds) / 86400.0;
        float weight = static_cast<float>(std::exp(-decay_lambda * age_days));

        // Store as float counts (fractional weights)
        transitions_weighted_[prev][record.intent_name] += weight;

        // Temporal bigram with decay
        temporal_transitions_weighted_[record.hour_of_day][prev][record.intent_name] += weight;

        // Trigram with decay
        if (recent_history_.size() >= 2) {
            auto it = recent_history_.rbegin();
            const auto& prev1 = it->intent_name;
            ++it;
            const auto& prev2 = it->intent_name;
            trigrams_weighted_[trigramKey(prev2, prev1)][record.intent_name] += weight;
        }
    }

    // Maintain history window
    recent_history_.push_back(record);
    while (recent_history_.size() > config_.history_window) {
        recent_history_.pop_front();
    }
}

std::vector<IntentPrediction> IntentPredictor::predict(
    std::uint32_t top_k) const {
    if (!isWarmedUp() || recent_history_.empty()) return {};

    const auto& current = recent_history_.back();
    const auto current_intent = current.intent_name;
    auto now_hour = static_cast<uint8_t>(
        std::chrono::duration_cast<std::chrono::hours>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 24);

    // Collect candidates with scores from all three models (using weighted counts)
    std::map<std::string, float> scores;

    // Level 1: Bigram P(B|A) with exponential decay
    auto bigram_it = transitions_weighted_.find(current_intent);
    if (bigram_it != transitions_weighted_.end()) {
        float total = 0.0f;
        for (const auto& [_, weight] : bigram_it->second) total += weight;
        if (total > 0.0f) {
            for (const auto& [intent, weight] : bigram_it->second) {
                float p = weight / total;
                scores[intent] += p * (1.0f - config_.temporal_weight);
            }
        }
    }

    // Level 2: Temporal bigram P(B|A, hour) with decay
    auto temp_it = temporal_transitions_weighted_.find(now_hour);
    if (temp_it != temporal_transitions_weighted_.end()) {
        auto temp_bigram = temp_it->second.find(current_intent);
        if (temp_bigram != temp_it->second.end()) {
            float total = 0.0f;
            for (const auto& [_, weight] : temp_bigram->second) total += weight;
            if (total > 0.0f) {
                for (const auto& [intent, weight] : temp_bigram->second) {
                    float p = weight / total;
                    scores[intent] += p * config_.temporal_weight;
                }
            }
        }
    }

    // Level 3: Trigram boost with decay
    if (recent_history_.size() >= 2) {
        auto it = recent_history_.rbegin();
        const auto& prev1 = it->intent_name;
        ++it;
        const auto& prev2 = it->intent_name;
        auto tri_it = trigrams_weighted_.find(trigramKey(prev2, prev1));
        if (tri_it != trigrams_weighted_.end()) {
            float total = 0.0f;
            for (const auto& [_, weight] : tri_it->second) total += weight;
            if (total > 0.0f) {
                for (const auto& [intent, weight] : tri_it->second) {
                    float p = weight / total;
                    // Trigram acts as a confidence multiplier
                    scores[intent] *= (1.0f + p * 0.5f);
                }
            }
        }
    }

    // Sort by score and return top-k above threshold
    std::vector<std::pair<std::string, float>> sorted(
        scores.begin(), scores.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<IntentPrediction> results;
    for (const auto& [intent, score] : sorted) {
        if (results.size() >= top_k) break;
        if (score < config_.min_confidence) break;

        IntentPrediction pred;
        pred.predicted_intent = intent;
        pred.confidence = std::min(score, 1.0f);
        // Use canonical phrasing for pre-computation
        auto phrasing_it = canonical_phrasing_.find(intent);
        pred.predicted_input = (phrasing_it != canonical_phrasing_.end())
            ? phrasing_it->second : intent;
        pred.rationale = "bigram+temporal+trigram ensemble";
        results.push_back(std::move(pred));
    }
    return results;
}

bool IntentPredictor::isWarmedUp() const {
    return observation_count_ >= config_.cold_start_threshold;
}

float IntentPredictor::recentHitRate(std::uint32_t window) const {
    if (prediction_hits_.empty()) return 0.0f;
    uint32_t count = std::min(window,
        static_cast<uint32_t>(prediction_hits_.size()));
    uint32_t hits = 0;
    auto it = prediction_hits_.rbegin();
    for (uint32_t i = 0; i < count && it != prediction_hits_.rend(); ++i, ++it) {
        if (*it) ++hits;
    }
    return static_cast<float>(hits) / static_cast<float>(count);
}

void IntentPredictor::save() const {
    // Serialize to JSON for persistence
    // In production, this would be encrypted with the device key
    auto path = config_.history_path;
    if (path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            path = std::string(home) + "/.sparx/speculation/model.json";
        } else return;
    }
    // Create parent dirs
    auto parent = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(parent);

    std::ofstream out(path);
    out << "{\n\"observation_count\":" << observation_count_ << ",\n";
    out << "\"transitions\":{\n";
    bool first_outer = true;
    for (const auto& [from, tos] : transitions_weighted_) {
        if (!first_outer) out << ",\n";
        first_outer = false;
        out << "\"" << from << "\":{";
        bool first_inner = true;
        for (const auto& [to, weight] : tos) {
            if (!first_inner) out << ",";
            first_inner = false;
            out << "\"" << to << "\":" << weight;
        }
        out << "}";
    }
    out << "\n}}\n";
}

void IntentPredictor::load() {
    auto path = config_.history_path;
    if (path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            path = std::string(home) + "/.sparx/speculation/model.json";
        } else return;
    }
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path);
    if (!in) return;

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // Parse observation_count
    auto obs_pos = content.find("\"observation_count\":");
    if (obs_pos != std::string::npos) {
        auto val_start = obs_pos + 20;  // length of "observation_count":
        auto val_end = content.find_first_of(",}\n", val_start);
        if (val_end != std::string::npos) {
            try {
                observation_count_ = static_cast<uint32_t>(
                    std::stoul(content.substr(val_start, val_end - val_start)));
            } catch (...) { /* corrupt file, start fresh */ }
        }
    }

    // Parse transitions: {"from":{"to":count,...},...}
    auto trans_pos = content.find("\"transitions\":{");
    if (trans_pos == std::string::npos) return;
    auto block_start = trans_pos + 15;  // after "transitions":{

    // Simple state-machine parser for the nested object
    transitions_weighted_.clear();
    size_t pos = block_start;
    while (pos < content.size()) {
        // Find next outer key (a "from" intent)
        auto key_start = content.find('"', pos);
        if (key_start == std::string::npos || content[key_start - 1] == '}') break;
        auto key_end = content.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        std::string from_key = content.substr(key_start + 1, key_end - key_start - 1);

        // Find the inner object start
        auto inner_start = content.find('{', key_end);
        if (inner_start == std::string::npos) break;
        auto inner_end = content.find('}', inner_start);
        if (inner_end == std::string::npos) break;

        // Parse inner key:value pairs
        std::string inner = content.substr(inner_start + 1, inner_end - inner_start - 1);
        size_t ipos = 0;
        while (ipos < inner.size()) {
            auto ik_start = inner.find('"', ipos);
            if (ik_start == std::string::npos) break;
            auto ik_end = inner.find('"', ik_start + 1);
            if (ik_end == std::string::npos) break;
            std::string to_key = inner.substr(ik_start + 1, ik_end - ik_start - 1);

            auto colon = inner.find(':', ik_end);
            if (colon == std::string::npos) break;
            auto vend = inner.find_first_of(",}", colon + 1);
            if (vend == std::string::npos) vend = inner.size();
            std::string val_str = inner.substr(colon + 1, vend - colon - 1);

            try {
                float weight = std::stof(val_str);
                transitions_weighted_[from_key][to_key] = weight;
            } catch (...) { /* skip malformed entry */ }

            ipos = vend + 1;
        }

        pos = inner_end + 1;
        // Skip comma between outer entries
        while (pos < content.size() && (content[pos] == ',' || content[pos] == '\n'))
            ++pos;
        // Check if we hit the closing brace of transitions
        if (pos < content.size() && content[pos] == '}') break;
    }
}

// ---------------------------------------------------------------------------
// EmbeddingIndex (SimHash over character trigrams)
// ---------------------------------------------------------------------------

std::string EmbeddingIndex::normalizeForEmbedding(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool prev_space = false;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            prev_space = false;
        } else if (!prev_space && !out.empty()) {
            out += ' ';
            prev_space = true;
        }
    }
    // Trim trailing space
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

EmbeddingVec EmbeddingIndex::hashTrigram(const std::string& trigram) {
    // Deterministic random projection using FNV-1a hash with dimension rotation.
    // Each trigram maps to a consistent direction in embedding space.
    EmbeddingVec proj{};

    // FNV-1a hash of the trigram
    uint64_t hash = 14695981039346656037ULL;
    for (char c : trigram) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }

    // Generate kEmbeddingDim pseudo-random projections from this hash
    // using a simple xorshift64 PRNG seeded by the trigram hash
    uint64_t state = hash;
    for (size_t d = 0; d < kEmbeddingDim; ++d) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        // Convert to float in [-1, 1]
        proj[d] = static_cast<float>(static_cast<int64_t>(state)) /
                  static_cast<float>(INT64_MAX);
    }
    return proj;
}

EmbeddingVec EmbeddingIndex::embed(const std::string& text) const {
    std::string norm = normalizeForEmbedding(text);
    EmbeddingVec vec{};

    if (norm.size() < 3) {
        // Too short for trigrams — use character-level
        for (size_t i = 0; i < norm.size(); ++i) {
            std::string uni(1, norm[i]);
            auto proj = hashTrigram(uni);
            for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] += proj[d];
        }
    } else {
        // Extract character trigrams and accumulate their projections
        for (size_t i = 0; i + 3 <= norm.size(); ++i) {
            std::string tri = norm.substr(i, 3);
            auto proj = hashTrigram(tri);
            for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] += proj[d];
        }
    }

    // L2 normalize
    float norm_sq = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) norm_sq += vec[d] * vec[d];
    if (norm_sq > 1e-9f) {
        float inv_norm = 1.0f / std::sqrt(norm_sq);
        for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] *= inv_norm;
    }
    return vec;
}

float EmbeddingIndex::cosineSimilarity(const EmbeddingVec& a, const EmbeddingVec& b) {
    // Both vectors are L2-normalized, so dot product = cosine similarity
    float dot = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) dot += a[d] * b[d];
    return dot;
}

void EmbeddingIndex::insert(const std::string& cache_key, const EmbeddingVec& vec) {
    // Replace if exists
    for (auto& entry : entries_) {
        if (entry.cache_key == cache_key) {
            entry.vec = vec;
            return;
        }
    }
    entries_.push_back({cache_key, vec});
}

std::optional<EmbeddingIndex::NearestResult> EmbeddingIndex::findNearest(
    const EmbeddingVec& query, float threshold) const {

    NearestResult best;
    for (const auto& entry : entries_) {
        float sim = cosineSimilarity(query, entry.vec);
        if (sim > best.similarity) {
            best.cache_key = entry.cache_key;
            best.similarity = sim;
        }
    }
    if (best.similarity >= threshold) return best;
    return std::nullopt;
}

void EmbeddingIndex::remove(const std::string& cache_key) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const IndexEntry& e) { return e.cache_key == cache_key; }),
        entries_.end());
}

void EmbeddingIndex::clear() {
    entries_.clear();
}

// ---------------------------------------------------------------------------
// SpeculationCache
// ---------------------------------------------------------------------------

SpeculationCache::SpeculationCache(CacheConfig config)
    : config_(std::move(config)) {}

void SpeculationCache::put(SpeculativeResult result) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check memory limit
    std::size_t entry_size = result.raw_output.size() + result.cache_key.size() + 128;
    while (stats_.current_memory + entry_size > config_.max_memory_bytes &&
           !entries_.empty()) {
        stats_.current_memory -= entries_.front().raw_output.size() +
                                  entries_.front().cache_key.size() + 128;
        entries_.pop_front();
        stats_.evictions++;
    }

    // Check entry limit
    while (entries_.size() >= config_.max_entries && !entries_.empty()) {
        stats_.current_memory -= entries_.front().raw_output.size() +
                                  entries_.front().cache_key.size() + 128;
        entries_.pop_front();
        stats_.evictions++;
    }

    stats_.current_memory += entry_size;
    stats_.current_entries = static_cast<uint32_t>(entries_.size()) + 1;

    // Index embedding for similarity search
    if (config_.enable_similarity_match) {
        // Extract the input text from the cache_key (format: "intent:hash")
        // We store the raw_output's first line as proxy for content similarity,
        // but more importantly we need the original input text.
        // The cache_key itself is stable enough for embedding index linkage.
        auto vec = embedding_index_.embed(result.cache_key);
        embedding_index_.insert(result.cache_key, vec);
    }

    entries_.push_back(std::move(result));
}

std::optional<SpeculativeResult> SpeculationCache::get(
    const std::string& intent,
    const std::string& input_hash,
    const std::string& context_hash) const {

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = nowUtcSeconds();

    // Phase 1: Exact match (O(n) scan, most specific)
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->intent_name != intent) continue;

        // Check TTL
        if ((now - it->computed_at_utc) > it->ttl_seconds) {
            stats_.expirations++;
            continue;
        }

        // Check context freshness
        if (config_.invalidate_on_context_change &&
            it->context_hash != context_hash) {
            continue;
        }

        // Exact input match
        if (it->cache_key == intent + ":" + input_hash) {
            stats_.hits++;
            return *it;
        }
    }

    // Phase 2: Embedding similarity fallback (fuzzy match)
    if (config_.enable_similarity_match && !entries_.empty()) {
        std::string query_key = intent + ":" + input_hash;
        auto query_vec = embedding_index_.embed(query_key);
        auto nearest = embedding_index_.findNearest(
            query_vec, config_.similarity_threshold);

        if (nearest) {
            // Found a similar entry — validate it's still fresh
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
                if (it->cache_key != nearest->cache_key) continue;
                if (it->intent_name != intent) continue;
                if ((now - it->computed_at_utc) > it->ttl_seconds) continue;
                if (config_.invalidate_on_context_change &&
                    it->context_hash != context_hash) continue;

                // Similarity hit — mark as such and return
                stats_.similarity_hits++;
                stats_.hits++;
                SpeculativeResult result = *it;
                result.prediction_confidence *= nearest->similarity;  // discount
                return result;
            }
        }
    }

    stats_.misses++;
    return std::nullopt;
}

void SpeculationCache::invalidateAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    embedding_index_.clear();
    stats_.current_entries = 0;
    stats_.current_memory = 0;
}

void SpeculationCache::invalidate(const std::string& intent) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const SpeculativeResult& r) {
                return r.intent_name == intent;
            }),
        entries_.end());
    stats_.current_entries = static_cast<uint32_t>(entries_.size());
}

SpeculationCache::Stats SpeculationCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// SpeculativeExecutor — Async Worker with Idle Sensing
// ---------------------------------------------------------------------------

SpeculativeExecutor::SpeculativeExecutor(
    IntentPredictor& predictor,
    SpeculationCache& cache,
    ExecutorConfig config)
    : predictor_(predictor), cache_(cache), config_(std::move(config)) {
    // Start background worker thread
    worker_thread_ = std::thread(&SpeculativeExecutor::workerLoop, this);
}

SpeculativeExecutor::~SpeculativeExecutor() {
    shutdown_.store(true);
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void SpeculativeExecutor::afterTurn(
    const IntentRecord& record,
    const std::string& context_hash,
    InferenceCallback infer_fn) {

    // Record observation for the predictor
    predictor_.observe(record);

    if (!predictor_.isWarmedUp()) return;

    // Get predictions
    auto predictions = predictor_.predict(config_.max_speculation_tokens > 0 ? 3 : 0);
    if (predictions.empty()) return;

    // Enqueue speculation tasks for the background worker
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& pred : predictions) {
            if (pred.confidence < 0.6f) break;

            // Skip if already cached
            auto input_hash = normalizeForCacheKey(pred.predicted_input);
            auto existing = cache_.get(pred.predicted_intent, input_hash,
                                        context_hash);
            if (existing) continue;

            SpeculationTask task;
            task.predicted_input = pred.predicted_input;
            task.intent_name = pred.predicted_intent;
            task.confidence = pred.confidence;
            task.context_hash = context_hash;
            task.infer_fn = infer_fn;
            task_queue_.push_back(std::move(task));
        }
    }
    queue_cv_.notify_one();

    // Save predictor state periodically
    if (predictor_.observationCount() % 10 == 0) {
        predictor_.save();
    }
}

void SpeculativeExecutor::workerLoop() {
    while (!shutdown_.load()) {
        SpeculationTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return !task_queue_.empty() || shutdown_.load();
            });
            if (shutdown_.load()) break;
            if (task_queue_.empty()) continue;

            // Check system idle before dequeuing
            if (!isSystemIdle()) {
                // Not idle — wait and retry
                continue;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop_front();
        }

        // Check preemption before executing
        if (preempt_requested_.load()) {
            preempt_requested_.store(false);
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.preempted++;
            continue;
        }

        executeTask(task);
    }
}

void SpeculativeExecutor::executeTask(const SpeculationTask& task) {
    active_speculation_.store(true);

    // Run inference at speculation priority
    auto result = task.infer_fn(task.predicted_input,
                                config_.max_speculation_tokens);

    active_speculation_.store(false);

    // Check if preempted during execution
    if (preempt_requested_.load()) {
        preempt_requested_.store(false);
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.preempted++;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.total_speculations++;
    }

    if (!result) return;

    // Cache the result
    auto input_hash = normalizeForCacheKey(task.predicted_input);
    SpeculativeResult cached;
    cached.cache_key = task.intent_name + ":" + input_hash;
    cached.raw_output = *result;
    cached.intent_name = task.intent_name;
    cached.prediction_confidence = task.confidence;
    cached.computed_at_utc = nowUtcSeconds();
    cached.ttl_seconds = 300;  // 5 minute TTL
    cached.context_hash = task.context_hash;
    cached.model_id = "local";
    cached.token_count = static_cast<uint32_t>(result->size() / 4);
    cache_.put(std::move(cached));
}

bool SpeculativeExecutor::isSystemIdle() const {
    float load = querySystemLoad();
    float battery = queryBatteryLevel();
    bool charging = queryIsCharging();

    switch (config_.priority) {
        case SpeculationPriority::IdleOnly:
            // Only speculate when load < 5% and (charging or battery > 50%)
            return load < 0.05f && (charging || battery > 0.5f);
        case SpeculationPriority::LowLoad:
            // Speculate when load < 20% and battery > 20%
            return load < 0.20f && battery > 0.2f;
        case SpeculationPriority::MediumLoad:
            // Speculate when load < 50% (aggressive)
            return load < 0.50f;
    }
    return false;
}

float SpeculativeExecutor::querySystemLoad() {
    // Platform-specific CPU/NPU load query
#ifdef __APPLE__
    // macOS: use getloadavg
    double loadavg[1] = {0.0};
    if (getloadavg(loadavg, 1) == 1) {
        // Normalize by number of CPUs (rough approximation)
        // loadavg of 1.0 on 8-core = 12.5% utilization
        return static_cast<float>(loadavg[0] / 8.0);
    }
    return 0.1f;  // fallback: assume light load
#elif defined(__linux__)
    // Linux: read /proc/stat for CPU utilization
    // Simplified: read loadavg
    double loadavg[1] = {0.0};
    if (getloadavg(loadavg, 1) == 1) {
        return static_cast<float>(loadavg[0] / 4.0);
    }
    return 0.1f;
#else
    return 0.1f;  // conservative default
#endif
}

float SpeculativeExecutor::queryBatteryLevel() {
#ifdef __APPLE__
    // macOS: would use IOKit PMSource queries
    // For now return 1.0 (desktop assumed to be on power)
    return 1.0f;
#elif defined(__linux__)
    // Linux: read /sys/class/power_supply/BAT0/capacity
    std::ifstream bat("/sys/class/power_supply/BAT0/capacity");
    if (bat) {
        int cap = 100;
        bat >> cap;
        return static_cast<float>(cap) / 100.0f;
    }
    return 1.0f;  // no battery = desktop
#else
    return 1.0f;
#endif
}

bool SpeculativeExecutor::queryIsCharging() {
#ifdef __linux__
    std::ifstream status("/sys/class/power_supply/BAT0/status");
    if (status) {
        std::string s;
        std::getline(status, s);
        return s == "Charging" || s == "Full";
    }
#endif
    return true;  // desktop or macOS (assume AC power)
}

std::optional<SpeculativeResult> SpeculativeExecutor::checkHit(
    const std::string& intent,
    const std::string& input_hash,
    const std::string& context_hash) const {

    auto result = cache_.get(intent, input_hash, context_hash);
    if (result) {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.successful_hits++;
    }
    return result;
}

void SpeculativeExecutor::preempt() {
    preempt_requested_.store(true);
    // Clear pending tasks — real work takes priority
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::lock_guard<std::mutex> mlock(mutex_);
        metrics_.preempted += static_cast<uint64_t>(task_queue_.size());
        task_queue_.clear();
    }
}

SpeculationMetrics SpeculativeExecutor::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

void SpeculativeExecutor::resetMetrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = {};
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string computeContextHash(const std::string& model_path,
                               const std::vector<std::string>& skills,
                               std::uint64_t session_turn) {
    std::ostringstream oss;
    oss << model_path << "|";
    for (const auto& s : skills) oss << s << ",";
    oss << "|" << session_turn;
    // Simple hash (in production: SHA-256)
    std::hash<std::string> hasher;
    return std::to_string(hasher(oss.str()));
}

std::string normalizeForCacheKey(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_ws = false;
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!last_ws && !out.empty()) out += ' ';
            last_ws = true;
        } else {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            last_ws = false;
        }
    }
    // Trim trailing
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace sparx::speculation
