#pragma once
/**
 * @file sparx_agent_scheduler.h
 * @brief Agent Scheduler — OS-level process management for concurrent Agents.
 *
 * Research basis:
 *   - AIOS: LLM Agent Operating System (arXiv:2403.16971)
 *   - "Agent-OS: A Blueprint for Real-Time, Secure, Scalable AI Agents" (Preprints 202509.0077)
 *   - "Architecting AgentOS: Token-Level Context to System-Level Intelligence" (arXiv:2602.20934)
 *   - CFS (Completely Fair Scheduler) — Linux kernel process scheduling
 *   - Priority Ceiling Protocol — real-time systems (Sha et al., 1990)
 *
 * This module provides OS-level Agent lifecycle management:
 *   1. Agent Process Model: spawn / suspend / resume / kill
 *   2. Priority-Based Scheduling: RT > Interactive > Background
 *   3. Time-Slice Round Robin: fair sharing of inference resources
 *   4. Preemption: higher-priority agent interrupts lower-priority
 *   5. Fair Queuing: no starvation via aging mechanism
 *   6. Resource Accounting: per-agent token budget tracking
 *
 * Scheduling hierarchy (analogous to Linux CFS + RT classes):
 *   ┌────────────────────────────────────────────────────┐
 *   │  RT Class (SCHED_FIFO)                             │
 *   │  - Safety monitors, formal verification            │
 *   │  - Always preempts lower classes                   │
 *   ├────────────────────────────────────────────────────┤
 *   │  Interactive Class (SCHED_RR)                      │
 *   │  - User-facing agents (current REPL session)       │
 *   │  - Round-robin within class, time-slice = 1 turn   │
 *   ├────────────────────────────────────────────────────┤
 *   │  Batch Class (SCHED_NORMAL / CFS)                  │
 *   │  - Background tasks (learning, sync, speculation)  │
 *   │  - Fair share, can be preempted                    │
 *   ├────────────────────────────────────────────────────┤
 *   │  Idle Class (SCHED_IDLE)                           │
 *   │  - Speculative execution, cache warming            │
 *   │  - Only runs when nothing else needs the NPU      │
 *   └────────────────────────────────────────────────────┘
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sparx::os {

// ─── Agent Identity ─────────────────────────────────────────────────────────

using AgentId = uint64_t;

/// Scheduling class (priority tier).
enum class SchedClass : uint8_t {
    RealTime    = 0,   // Highest: safety monitors, verification
    Interactive = 1,   // User-facing: REPL agents
    Batch       = 2,   // Background: learning, sync
    Idle        = 3,   // Lowest: speculation, cache warming
};

/// Agent execution state (mirrors OS process states).
enum class AgentState : uint8_t {
    Created,    // Spawned but not yet scheduled
    Ready,      // In run queue, waiting for CPU/NPU time
    Running,    // Currently executing (has the inference resource)
    Suspended,  // Voluntarily yielded or externally suspended
    Blocked,    // Waiting on I/O, tool call, or dependency
    Terminated, // Finished execution (exit code available)
};

/// Reason for blocking.
enum class BlockReason : uint8_t {
    None,
    WaitingOnTool,      // Tool call in progress
    WaitingOnCloud,     // Cloud inference pending
    WaitingOnPeer,      // Mesh peer response pending
    WaitingOnUser,      // Needs user input
    WaitingOnAgent,     // Depends on another agent's output
    ResourceLimit,      // Hit token/API quota
};

// ─── Agent Process Control Block (PCB) ──────────────────────────────────────

/// Per-agent resource accounting.
struct ResourceAccounting {
    uint64_t tokens_consumed = 0;     // Total tokens used (input + output)
    uint64_t tokens_budget = 0;       // Max tokens allowed (0 = unlimited)
    uint32_t api_calls_made = 0;      // External API calls
    uint32_t api_calls_limit = 0;     // Max API calls (0 = unlimited)
    uint32_t tool_calls_made = 0;     // Tool invocations
    uint32_t turns_completed = 0;     // Inference turns executed
    std::chrono::milliseconds cpu_time{0};   // Wall-clock time in Running state
    std::chrono::milliseconds wait_time{0};  // Time in Ready queue

    bool withinBudget() const {
        if (tokens_budget > 0 && tokens_consumed >= tokens_budget) return false;
        if (api_calls_limit > 0 && api_calls_made >= api_calls_limit) return false;
        return true;
    }
};

/// Agent Process Control Block — all scheduler state for one agent.
struct AgentPCB {
    AgentId id = 0;
    std::string name;                  // Human-readable name
    std::string agent_type;            // "repl", "background", "monitor", etc.

    // ── Scheduling state ──
    SchedClass sched_class = SchedClass::Batch;
    AgentState state = AgentState::Created;
    BlockReason block_reason = BlockReason::None;
    int priority = 0;                  // Within-class priority (higher = more important)
    uint64_t vruntime = 0;             // Virtual runtime (CFS fairness metric)
    uint32_t time_slice_remaining = 1; // Turns remaining in current slice
    uint32_t nice = 0;                 // Nice value (0-39, 20 = default, higher = nicer)

    // ── Lifecycle timestamps ──
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_scheduled;
    std::chrono::steady_clock::time_point last_blocked;

    // ── Resource accounting ──
    ResourceAccounting resources;

    // ── Aging (anti-starvation) ──
    uint32_t wait_ticks = 0;           // Ticks spent in Ready without scheduling
    static constexpr uint32_t kAgingThreshold = 10; // Boost priority after N ticks

    // ── Dependencies ──
    std::set<AgentId> waiting_on;      // Agents this one depends on
    std::set<AgentId> blocked_by_me;   // Agents waiting on this one

    // ── Execution context (opaque handle for Context Manager) ──
    uint64_t context_handle = 0;

    /// Effective priority after aging boost.
    int effectivePriority() const {
        int boost = static_cast<int>(wait_ticks / kAgingThreshold);
        return priority + boost;
    }
};

// ─── Scheduler Configuration ────────────────────────────────────────────────

struct SchedulerConfig {
    /// Time slice for Interactive class (in turns).
    uint32_t interactive_time_slice = 1;
    /// Time slice for Batch class (in turns).
    uint32_t batch_time_slice = 3;
    /// Maximum concurrent running agents.
    uint32_t max_concurrent = 4;
    /// Enable preemption (higher priority interrupts lower).
    bool enable_preemption = true;
    /// Aging interval (scheduler ticks before priority boost).
    uint32_t aging_interval = 10;
    /// Default token budget per agent (0 = unlimited).
    uint64_t default_token_budget = 0;
    /// Tick interval for scheduler maintenance (ms).
    uint32_t tick_interval_ms = 100;
};

// ─── Scheduler Events ───────────────────────────────────────────────────────

struct SchedulerEvent {
    enum class Type : uint8_t {
        AgentSpawned,
        AgentScheduled,
        AgentPreempted,
        AgentSuspended,
        AgentResumed,
        AgentBlocked,
        AgentUnblocked,
        AgentTerminated,
        BudgetExhausted,
        AgingBoost,
    };
    Type type;
    AgentId agent_id;
    std::string detail;
    std::chrono::steady_clock::time_point timestamp;
};

using SchedulerCallback = std::function<void(const SchedulerEvent&)>;

// ─── Agent Scheduler ────────────────────────────────────────────────────────

/**
 * @brief OS-level scheduler for concurrent Agent execution.
 *
 * Manages the lifecycle of multiple Agents competing for shared inference
 * resources (NPU/LLM). Provides:
 *   - Fair scheduling via virtual runtime (CFS-inspired)
 *   - Priority classes with preemption
 *   - Anti-starvation via aging
 *   - Resource accounting and budget enforcement
 *   - Dependency tracking between agents
 *
 * Thread-safe. Runs a background tick thread for aging and accounting.
 */
class AgentScheduler {
public:
    explicit AgentScheduler(SchedulerConfig config = {});
    ~AgentScheduler();

    /// Start the scheduler (begins tick thread).
    void start();
    /// Stop the scheduler.
    void stop();

    // ── Agent Lifecycle ──

    /// Spawn a new agent. Returns its ID.
    AgentId spawn(const std::string& name, SchedClass sched_class,
                  int priority = 0, const std::string& agent_type = "generic");

    /// Suspend an agent (voluntarily or externally).
    bool suspend(AgentId id);

    /// Resume a suspended agent (puts it back in Ready queue).
    bool resume(AgentId id);

    /// Kill an agent (immediate termination).
    bool kill(AgentId id);

    /// Block an agent (waiting on external event).
    bool block(AgentId id, BlockReason reason);

    /// Unblock an agent (external event arrived).
    bool unblock(AgentId id);

    // ── Scheduling ──

    /// Select the next agent to run. Returns nullopt if nothing is ready.
    /// This is called by the inference loop to get the next agent's turn.
    std::optional<AgentId> schedule();

    /// Called when an agent completes one turn of inference.
    /// Updates accounting and decides if it keeps the resource or yields.
    bool yield(AgentId id, uint32_t tokens_used = 0);

    /// Notify scheduler that an agent finished its work entirely.
    void terminate(AgentId id, int exit_code = 0);

    // ── Queries ──

    /// Get the PCB for an agent (read-only snapshot).
    std::optional<AgentPCB> getAgent(AgentId id) const;

    /// Get all agents in a given state.
    std::vector<AgentPCB> agentsByState(AgentState state) const;

    /// Get all agents.
    std::vector<AgentPCB> allAgents() const;

    /// Number of agents in each state.
    struct Stats {
        uint32_t total = 0;
        uint32_t running = 0;
        uint32_t ready = 0;
        uint32_t suspended = 0;
        uint32_t blocked = 0;
        uint32_t terminated = 0;
        uint64_t total_tokens = 0;
        uint64_t schedule_count = 0;   // Total schedule() calls
        uint64_t preemption_count = 0;
        uint64_t aging_boosts = 0;
    };
    Stats stats() const;

    // ── Dependencies ──

    /// Declare that `waiter` depends on `dependency`.
    void addDependency(AgentId waiter, AgentId dependency);

    /// Remove a dependency (when dependency completes).
    void removeDependency(AgentId waiter, AgentId dependency);

    // ── Events ──

    /// Register a callback for scheduler events.
    void onEvent(SchedulerCallback cb);

    // ── Resource Management ──

    /// Set token budget for an agent.
    void setTokenBudget(AgentId id, uint64_t budget);

    /// Get remaining budget.
    uint64_t remainingBudget(AgentId id) const;

private:
    SchedulerConfig config_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread tick_thread_;

    // Agent storage
    AgentId next_id_ = 1;
    std::map<AgentId, AgentPCB> agents_;

    // Run queues (per scheduling class)
    std::deque<AgentId> rt_queue_;
    std::deque<AgentId> interactive_queue_;
    std::deque<AgentId> batch_queue_;
    std::deque<AgentId> idle_queue_;

    // Currently running agents
    std::set<AgentId> running_set_;

    // Event callbacks
    std::vector<SchedulerCallback> callbacks_;

    // Stats
    mutable Stats stats_;

    // ── Internal methods ──

    /// Pick from highest non-empty queue (RT > Interactive > Batch > Idle).
    std::optional<AgentId> pickNext();

    /// Check if preemption is needed (higher priority agent waiting).
    std::optional<AgentId> checkPreemption();

    /// Background tick: aging, timeout detection, budget enforcement.
    void tickLoop();

    /// Enqueue an agent into its class's run queue.
    void enqueue(AgentId id);

    /// Remove from run queue.
    void dequeue(AgentId id);

    /// Emit an event.
    void emit(SchedulerEvent::Type type, AgentId id,
              const std::string& detail = "");

    /// Update vruntime after a turn.
    void updateVRuntime(AgentPCB& pcb, uint32_t tokens);
};

}  // namespace sparx::os
