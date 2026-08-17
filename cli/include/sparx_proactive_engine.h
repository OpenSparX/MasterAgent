#pragma once
/**
 * @file sparx_proactive_engine.h
 * @brief Proactive Context Engine — situation-aware autonomous task triggering.
 *
 * Research basis:
 *   - "Proactive Agents: Agents that Anticipate" (arXiv:2404.01345)
 *   - "Ambient Intelligence: From Context-Awareness to Proactive Computing"
 *     (Augusto & McCullagh, 2007)
 *   - Linux inotify / epoll event-driven architecture
 *   - Reactive Extensions (Rx) — observable stream composition
 *
 * MasterAgent supports two task origination modes:
 *
 *   ┌─────────────────────────────────────────────────────────────────┐
 *   │  REACTIVE (被动)                                                │
 *   │  User Input → Intent Parse → DAG Plan → Execute                │
 *   ├─────────────────────────────────────────────────────────────────┤
 *   │  PROACTIVE (主动)                                               │
 *   │  Context Signal → Trigger Eval → Condition Match → Auto Plan   │
 *   │                                                                 │
 *   │  Signals:                                                       │
 *   │    - Sensor streams (camera, IR, IMU, GPS, microphone)          │
 *   │    - Vehicle CAN bus (speed, RPM, fuel, driver state)           │
 *   │    - Environment (time-of-day, weather, geofence)               │
 *   │    - System events (battery low, NPU idle, connectivity)        │
 *   │                                                                 │
 *   │  Trigger lifecycle:                                             │
 *   │    Register → Poll/Subscribe → Evaluate → Activate → Cooldown  │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * Resource management for proactive polling:
 *   - Budget-aware: each trigger has a compute cost estimate
 *   - Priority-based: safety triggers always fire; comfort triggers
 *     only when NPU idle capacity > threshold
 *   - Cooldown: prevent trigger spam (configurable per-trigger)
 *   - Batch evaluation: group low-priority triggers into single
 *     inference pass to amortize NPU wake cost
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sparx::proactive {

// ─── Signal Types ───────────────────────────────────────────────────────────

/// Origin of a context signal.
enum class SignalSource : uint8_t {
    Camera,         // Visual stream (RGB, IR, depth)
    Microphone,     // Ambient audio / keyword detection
    IMU,            // Acceleration, gyroscope
    GPS,            // Position, speed, heading
    CAN_Bus,        // Vehicle data (OBD-II / proprietary)
    Environment,    // Temperature, humidity, light
    System,         // Battery, NPU load, connectivity
    Timer,          // Periodic / cron-style triggers
    Geofence,       // Entering/leaving a spatial zone
    UserBehavior,   // Inferred state (drowsy, distracted, idle)
};

/// Priority class for triggers (maps to scheduler SchedClass).
enum class TriggerPriority : uint8_t {
    Safety    = 0,  // Always fires immediately (collision, fire, medical)
    Urgent    = 1,  // Fires if NPU load < 80%
    Comfort   = 2,  // Fires if NPU idle > 40%
    Ambient   = 3,  // Fires only during idle windows
};

/// A raw context signal from a sensor or system event.
struct ContextSignal {
    SignalSource source;
    std::string channel;            // e.g. "camera.front.ir", "can.driver_state"
    int64_t timestamp_ms = 0;
    double confidence = 1.0;        // Signal quality [0, 1]

    /// Signal payload — variant for type-safe access.
    struct NumericPayload { double value; std::string unit; };
    struct VectorPayload  { std::vector<double> values; };
    struct LabelPayload   { std::string label; double score; };
    struct FramePayload   { uint32_t width; uint32_t height; std::string encoding; };

    using Payload = std::variant<NumericPayload, VectorPayload,
                                 LabelPayload, FramePayload>;
    Payload data;
};

// ─── Trigger Definition ─────────────────────────────────────────────────────

using TriggerId = uint64_t;

/// Condition expression for trigger evaluation.
/// Supports: threshold, change-rate, pattern-match, temporal (duration).
struct TriggerCondition {
    enum class Op : uint8_t {
        GreaterThan,
        LessThan,
        Equals,
        ChangeRate,     // |current - prev| / dt > threshold
        Pattern,        // ML-inferred pattern match (e.g. "drowsy_face")
        Duration,       // Condition held for N seconds
        Compound,       // AND/OR of sub-conditions
    };

    Op op;
    std::string signal_channel;     // Which signal to watch
    double threshold = 0.0;
    std::string pattern_name;       // For Op::Pattern
    std::chrono::seconds duration{0};

    /// For compound conditions
    enum class Logic : uint8_t { And, Or };
    Logic logic = Logic::And;
    std::vector<TriggerCondition> sub_conditions;
};

/// Resource budget for a trigger evaluation cycle.
struct TriggerBudget {
    uint32_t max_tokens = 512;      // Max tokens for condition inference
    uint32_t max_latency_ms = 50;   // Must evaluate within this time
    float npu_share = 0.1f;         // Max fraction of NPU per eval cycle
};

/// Configuration for a single proactive trigger.
struct TriggerConfig {
    TriggerId id = 0;
    std::string name;               // Human-readable, e.g. "driver_fatigue"
    std::string description;

    TriggerPriority priority = TriggerPriority::Comfort;
    TriggerCondition condition;
    TriggerBudget budget;

    /// Polling interval (for non-event-driven signals).
    std::chrono::milliseconds poll_interval{1000};

    /// Cooldown after firing (prevents re-triggering).
    std::chrono::seconds cooldown{30};

    /// Action template: what DAG to generate when triggered.
    std::string action_intent;      // Intent string passed to Planner
    std::map<std::string, std::string> action_params;

    bool enabled = true;
};

// ─── Proactive Engine ───────────────────────────────────────────────────────

/// Statistics for monitoring engine health.
struct EngineStats {
    uint64_t signals_processed = 0;
    uint64_t triggers_evaluated = 0;
    uint64_t triggers_fired = 0;
    uint64_t triggers_suppressed = 0;  // Suppressed by budget/cooldown
    double avg_eval_latency_ms = 0.0;
    float current_npu_usage = 0.0f;
};

/// Result of a trigger evaluation.
struct TriggerResult {
    TriggerId trigger_id;
    bool fired = false;
    double confidence = 0.0;
    std::string intent;             // Intent to pass to Planner
    std::map<std::string, std::string> params;
    int64_t timestamp_ms = 0;
};

/**
 * @class ProactiveEngine
 * @brief Manages context-aware autonomous task triggering.
 *
 * The engine runs a background evaluation loop:
 *   1. Collect signals from registered sources (poll or subscribe)
 *   2. Evaluate trigger conditions against signal buffer
 *   3. For matched triggers, check budget & cooldown
 *   4. If allowed, emit a TriggerResult → Planner generates DAG
 *
 * Resource management strategy:
 *   - Safety triggers: ALWAYS evaluated, pre-empt everything
 *   - Urgent triggers: evaluated every poll_interval if NPU < 80%
 *   - Comfort triggers: batched evaluation when NPU idle > 40%
 *   - Ambient triggers: only during sustained idle windows (>5s)
 *
 * Polling vs Event-driven:
 *   - Camera/IR: frame callback (event-driven, no polling overhead)
 *   - CAN bus: message-driven (hardware interrupt → signal)
 *   - GPS/IMU: polled at configurable intervals
 *   - Timers: OS-level timer wheel (zero polling cost)
 */
class ProactiveEngine {
public:
    /// Callback when a trigger fires → sends intent to Planner.
    using TriggerCallback = std::function<void(const TriggerResult&)>;

    ProactiveEngine() = default;
    ~ProactiveEngine() = default;

    // ── Trigger Management ──────────────────────────────────────────────

    /// Register a new proactive trigger.
    TriggerId register_trigger(TriggerConfig config);

    /// Update an existing trigger's configuration.
    bool update_trigger(TriggerId id, TriggerConfig config);

    /// Remove a trigger.
    bool remove_trigger(TriggerId id);

    /// Enable/disable a trigger without removing it.
    void set_enabled(TriggerId id, bool enabled);

    /// List all registered triggers.
    std::vector<TriggerConfig> list_triggers() const;

    // ── Signal Ingestion ────────────────────────────────────────────────

    /// Push a signal into the engine (called by sensor adapters).
    void ingest_signal(ContextSignal signal);

    /// Register a signal source for polling (engine will poll it).
    void register_poll_source(SignalSource source, std::string channel,
                              std::chrono::milliseconds interval,
                              std::function<ContextSignal()> poll_fn);

    // ── Engine Lifecycle ────────────────────────────────────────────────

    /// Start the evaluation loop (background thread).
    void start(TriggerCallback on_trigger);

    /// Stop the engine gracefully.
    void stop();

    /// Check if engine is running.
    bool is_running() const;

    // ── Resource Control ────────────────────────────────────────────────

    /// Set global NPU budget ceiling for proactive evaluation.
    void set_npu_budget(float max_fraction);

    /// Report current NPU load (called by scheduler).
    void report_npu_load(float current_load);

    /// Get engine statistics.
    EngineStats stats() const;

    // ── Batch Evaluation ────────────────────────────────────────────────

    /// Force evaluation of all pending triggers (for testing).
    std::vector<TriggerResult> evaluate_now();

private:
    mutable std::mutex mu_;
    std::vector<TriggerConfig> triggers_;
    std::vector<ContextSignal> signal_buffer_;
    TriggerCallback callback_;
    EngineStats stats_;

    bool running_ = false;
    float npu_budget_ = 0.3f;       // Default: proactive gets 30% of NPU
    float current_npu_load_ = 0.0f;

    /// Evaluate a single trigger against buffered signals.
    std::optional<TriggerResult> evaluate_trigger(const TriggerConfig& trigger);

    /// Check if a trigger is in cooldown.
    bool in_cooldown(TriggerId id) const;

    /// Check if budget allows evaluation at given priority.
    bool budget_allows(TriggerPriority priority) const;

    std::map<TriggerId, int64_t> last_fired_;  // Cooldown tracking
};

} // namespace sparx::proactive

