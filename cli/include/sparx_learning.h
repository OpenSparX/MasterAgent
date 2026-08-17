/**
 * @file sparx_learning.h
 * @brief On-Device Continual Learning — privacy-preserving personalization.
 *
 * Architecture:
 *   1. Feedback capture: user corrections produce (input, preferred_output) pairs
 *   2. Encrypted storage: AES-256-GCM encrypted at rest in
 *      ~/.sparx/learning/pairs/, key derived from device-bound seed
 *   3. Privacy-preserving training: Differential Privacy (DP-SGD) with
 *      configurable epsilon budget ensures individual corrections cannot be
 *      extracted from the trained adapter
 *   4. Idle-time scheduling: training triggers only when device is idle
 *      (NPU load < threshold, battery > minimum, thermal budget available)
 *   5. Quality guard: perplexity validation before/after ensures the adapter
 *      improves responses; automatic rollback if quality degrades
 *   6. Progressive adapter merging: new adapters are merged with previous
 *      versions using weighted interpolation, preventing catastrophic forgetting
 *   7. Adapter loading: personalized adapter merged at inference time via
 *      llama-server's --lora flag, <100ms overhead
 *
 * Data never leaves the device. The adapter is a few MB and loads in <100ms.
 * Privacy budget (epsilon) is tracked across training runs — once exhausted,
 * training stops until the budget is refreshed (weekly by default).
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sparx::learning {

/// A single correction from the user: "I said X, you said Y, I wanted Z."
struct TrainingPair {
    std::string id;              // UUID
    std::string input;           // user prompt
    std::string model_output;    // what the model actually said
    std::string preferred;       // what the user wanted instead
    std::string agent_name;      // which agent produced this
    std::string model_id;        // which model was active
    std::int64_t timestamp_utc;  // unix seconds
    std::uint32_t turn_number;   // which turn in the session
};

/// Status of the learning subsystem for a given agent.
struct LearningStatus {
    std::string agent_name;
    std::uint32_t total_pairs;
    std::uint32_t pairs_since_last_train;
    std::uint32_t training_threshold;   // pairs needed to trigger training
    bool adapter_available;
    std::string adapter_path;           // empty if none trained yet
    std::string last_trained_utc;       // ISO 8601 or empty
    std::uint32_t adapter_version;      // increments each training run
    // Privacy budget
    float privacy_epsilon_spent;        // cumulative epsilon consumed
    float privacy_epsilon_budget;       // total budget per refresh period
    bool privacy_budget_exhausted;      // true = no more training until refresh
    // Quality metrics
    float last_perplexity_before;       // base model perplexity on eval set
    float last_perplexity_after;        // adapted model perplexity
    float quality_improvement_pct;      // (before - after) / before * 100
};

// ---------------------------------------------------------------------------
// Privacy & Differential Privacy
// ---------------------------------------------------------------------------

/// Differential Privacy configuration for training.
/// Uses DP-SGD: gradients are clipped and Gaussian noise is added per-sample.
struct PrivacyConfig {
    /// Total epsilon budget per refresh period. Lower = more private.
    /// Recommended: 1.0–8.0 for useful learning with strong privacy.
    float epsilon_budget = 4.0f;
    /// Delta parameter (probability of privacy breach). Should be << 1/N.
    float delta = 1e-5f;
    /// Gradient clipping norm (C). Bounds per-sample gradient contribution.
    float max_grad_norm = 1.0f;
    /// Noise multiplier (σ). Higher = more noise = more private but slower learning.
    float noise_multiplier = 1.1f;
    /// Budget refresh period in seconds (default: 7 days).
    std::int64_t budget_refresh_seconds = 7 * 24 * 3600;
    /// Path to the privacy ledger file (tracks cumulative epsilon).
    std::string ledger_path;  // defaults to ~/.sparx/learning/privacy_ledger.json
};

/// Tracks cumulative privacy expenditure across training runs.
class PrivacyAccountant {
public:
    explicit PrivacyAccountant(const PrivacyConfig& config,
                               const std::filesystem::path& base_dir);

    /// Returns remaining epsilon budget.
    float remainingBudget() const;

    /// Returns true if a training run with `n_steps` at current noise level
    /// would stay within budget.
    bool canTrain(std::uint32_t n_steps, std::uint32_t n_samples) const;

    /// Records epsilon spent by a training run. Uses Rényi DP accounting.
    void recordSpend(float epsilon_spent);

    /// Checks if budget should be refreshed (time-based).
    void maybeRefresh();

    /// Current cumulative epsilon.
    float epsilonSpent() const { return epsilon_spent_; }

    /// Computes epsilon for a given number of steps using RDP → (ε,δ)-DP.
    float computeEpsilon(std::uint32_t n_steps,
                         std::uint32_t n_samples) const;

private:
    PrivacyConfig config_;
    std::filesystem::path ledger_path_;
    float epsilon_spent_ = 0.0f;
    std::int64_t last_refresh_utc_ = 0;

    void loadLedger();
    void saveLedger() const;
};

// ---------------------------------------------------------------------------
// Device Idle Scheduling
// ---------------------------------------------------------------------------

/// Device resource snapshot for idle-time training decisions.
struct DeviceResources {
    float npu_load_pct = 0.0f;      // 0–100, current NPU utilization
    float cpu_load_pct = 0.0f;      // 0–100, current CPU utilization
    float battery_pct = 100.0f;     // 0–100, current battery level
    float thermal_headroom = 1.0f;  // 0–1, available thermal budget
    bool is_charging = true;        // plugged in?
    bool screen_off = false;        // screen inactive?
};

/// Policy for when training is allowed.
struct IdlePolicy {
    float max_npu_load = 10.0f;      // only train if NPU < this %
    float max_cpu_load = 30.0f;      // only train if CPU < this %
    float min_battery = 30.0f;       // minimum battery to start training
    float min_thermal_headroom = 0.3f; // minimum thermal budget
    bool require_charging = false;    // require plugged-in?
    bool require_screen_off = false;  // require screen off?
};

/// Checks device state and decides whether training should proceed.
class IdleScheduler {
public:
    explicit IdleScheduler(IdlePolicy policy = {});

    /// Queries current device resources (platform-specific).
    DeviceResources queryResources() const;

    /// Returns true if training is allowed given current device state.
    bool isIdle() const;

    /// Returns a human-readable reason if training is blocked.
    std::string blockReason() const;

    /// Register a callback for when device becomes idle.
    using IdleCallback = std::function<void()>;
    void onIdle(IdleCallback cb);

private:
    IdlePolicy policy_;
    std::vector<IdleCallback> callbacks_;
};

// ---------------------------------------------------------------------------
// Quality Guard
// ---------------------------------------------------------------------------

/// Validates adapter quality before committing.
struct QualityMetrics {
    float perplexity_before = 0.0f;  // base model on held-out set
    float perplexity_after = 0.0f;   // adapted model on same set
    float accuracy_before = 0.0f;    // % correct on training pairs
    float accuracy_after = 0.0f;     // % correct with adapter
    bool improved = false;           // overall judgment
    std::string rejection_reason;    // if !improved, why
};

/// Evaluates adapter quality and decides whether to commit or rollback.
class QualityGuard {
public:
    /// Maximum allowed perplexity increase (ratio). If after/before > this,
    /// the adapter is rejected. Default 1.05 = max 5% degradation allowed.
    explicit QualityGuard(float max_degradation_ratio = 1.05f);

    /// Evaluate a newly trained adapter against the base model.
    /// Uses held-out pairs (last 20% of training data) as eval set.
    QualityMetrics evaluate(const std::string& base_model_path,
                            const std::filesystem::path& adapter_path,
                            const std::vector<TrainingPair>& eval_pairs) const;

    /// Returns true if the adapter should be committed.
    bool shouldCommit(const QualityMetrics& metrics) const;

private:
    float max_degradation_ratio_;
};

// ---------------------------------------------------------------------------
// Adapter Merging (Anti-Catastrophic-Forgetting)
// ---------------------------------------------------------------------------

/// Strategy for combining old and new adapters.
enum class MergeStrategy : std::uint8_t {
    /// Replace: new adapter fully replaces old. Simple but forgets.
    Replace,
    /// WeightedAverage: new = α*new + (1-α)*old. Balances old/new knowledge.
    WeightedAverage,
    /// TaskArithmetic: difference vectors are combined additively.
    TaskArithmetic,
};

struct MergeConfig {
    MergeStrategy strategy = MergeStrategy::WeightedAverage;
    /// Weight for the new adapter (0–1). Higher = more weight to recent learning.
    float new_weight = 0.7f;
    /// Maximum number of adapters to keep (for rollback). Oldest are pruned.
    std::uint32_t max_versions = 5;
};

/// Merges LoRA adapters to prevent catastrophic forgetting.
class AdapterMerger {
public:
    explicit AdapterMerger(MergeConfig config = {});

    /// Merges new adapter with the previous version.
    /// Returns path to the merged adapter.
    std::optional<std::filesystem::path> merge(
        const std::filesystem::path& old_adapter,
        const std::filesystem::path& new_adapter,
        const std::filesystem::path& output_path) const;

    /// Prunes old adapter versions beyond max_versions.
    void pruneOldVersions(const std::string& agent_name,
                          const std::filesystem::path& adapters_dir) const;

    const MergeConfig& config() const { return config_; }

private:
    MergeConfig config_;
};

// ---------------------------------------------------------------------------
// Core Components (Storage, Management, Orchestration)
// ---------------------------------------------------------------------------

/// Persists training pairs to ~/.sparx/learning/pairs/<agent>/
/// Files are encrypted with a device-local key derived from hardware ID.
class TrainingPairStore {
public:
    explicit TrainingPairStore(const std::filesystem::path& base_dir);

    /// Appends a new training pair. Returns the assigned ID.
    std::string append(const TrainingPair& pair);

    /// Loads all pairs for a given agent, decrypted.
    std::vector<TrainingPair> loadAll(const std::string& agent_name) const;

    /// Count of pairs for an agent (without decrypting contents).
    std::uint32_t count(const std::string& agent_name) const;

    /// Removes pairs that have been incorporated into an adapter.
    void markTrained(const std::string& agent_name,
                     const std::vector<std::string>& pair_ids);

    /// Base directory for this store.
    const std::filesystem::path& baseDir() const { return base_dir_; }

private:
    std::filesystem::path base_dir_;
    std::string deriveKey() const;
};

/// Manages LoRA adapters in ~/.sparx/adapters/<agent>/
class AdapterManager {
public:
    explicit AdapterManager(const std::filesystem::path& adapters_dir);

    /// Returns the path to the latest adapter for an agent, or empty.
    std::optional<std::filesystem::path> latestAdapter(
        const std::string& agent_name) const;

    /// Returns the version number of the latest adapter (0 if none).
    std::uint32_t latestVersion(const std::string& agent_name) const;

    /// Path where the next adapter should be written.
    std::filesystem::path nextAdapterPath(
        const std::string& agent_name) const;

    /// Registers a newly trained adapter (moves from temp to final location).
    void commit(const std::string& agent_name,
                const std::filesystem::path& trained_path);

    /// Removes all adapters for an agent (reset personalization).
    void reset(const std::string& agent_name);

    const std::filesystem::path& baseDir() const { return adapters_dir_; }

private:
    std::filesystem::path adapters_dir_;
};

/// Training configuration for QLoRA fine-tuning.
struct TrainingConfig {
    std::uint32_t min_pairs = 5;        // minimum pairs before training
    std::uint32_t lora_rank = 8;        // LoRA rank (r)
    std::uint32_t lora_alpha = 16;      // LoRA alpha
    float lora_dropout = 0.05f;
    std::uint32_t epochs = 3;
    std::uint32_t batch_size = 1;       // on-device = small batch
    float learning_rate = 2e-4f;
    std::uint32_t max_seq_length = 512;
    std::string quantization = "q4_0";  // QLoRA quantization level
    // Advanced
    PrivacyConfig privacy;
    IdlePolicy idle_policy;
    MergeConfig merge;
    float quality_max_degradation = 1.05f;
};

/// Orchestrates the full training pipeline with privacy, quality, and scheduling.
class TrainingOrchestrator {
public:
    TrainingOrchestrator(TrainingPairStore& store,
                         AdapterManager& adapters,
                         TrainingConfig config = {});

    /// Checks if training should run (enough pairs accumulated).
    bool shouldTrain(const std::string& agent_name) const;

    /// Full preflight: enough pairs + device idle + privacy budget available.
    bool canTrainNow(const std::string& agent_name) const;

    /// Exports pairs to JSONL format for the training script.
    std::filesystem::path exportTrainingData(
        const std::string& agent_name) const;

    /// Full training pipeline:
    ///   1. Check idle & privacy budget
    ///   2. Export data with DP noise injection
    ///   3. Run QLoRA fine-tuning
    ///   4. Quality validation (perplexity check)
    ///   5. Adapter merging with previous version
    ///   6. Commit or rollback
    /// Returns the path to the new adapter on success.
    std::optional<std::filesystem::path> train(
        const std::string& agent_name,
        const std::string& base_model_path);

    /// Returns learning status for display.
    LearningStatus status(const std::string& agent_name) const;

    const TrainingConfig& config() const { return config_; }

    /// Access sub-components for fine-grained control.
    PrivacyAccountant& privacy() { return privacy_; }
    IdleScheduler& scheduler() { return scheduler_; }
    QualityGuard& quality() { return quality_; }
    AdapterMerger& merger() { return merger_; }

private:
    TrainingPairStore& store_;
    AdapterManager& adapters_;
    TrainingConfig config_;
    PrivacyAccountant privacy_;
    IdleScheduler scheduler_;
    QualityGuard quality_;
    AdapterMerger merger_;
};

/// Resolves the adapter path to pass to llama-server's --lora flag.
/// Returns nullopt if no adapter exists for this agent.
std::optional<std::string> resolveAdapterForInference(
    const std::string& agent_name,
    const std::filesystem::path& adapters_dir =
        std::filesystem::path{});

}  // namespace sparx::learning
