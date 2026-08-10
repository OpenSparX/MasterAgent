#pragma once

/**
 * @file types.h
 * @brief Defines shared identity, priority, deadline, error, and side-effect contracts.
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace master_agent {

enum class CallerModuleId : std::uint8_t {
    Invalid,
    InteractionIngress,
    AgentService,
    IntentRecognitionEngine,
    PreprocessingEngine,
    MemoryService,
    SkillEngine,
    PromptEngine,
    InferenceFramework,
    TaskOrchestrationEngine,
    AgentDispatch,
    SubAgent,
    AtomicServiceManager,
    KvCacheManager,
    DataLogManager,
    ExceptionManager
};

enum class TaskPriority : std::uint8_t {
    P0 = 0,
    P1 = 1,
    P2 = 2
};

enum class SideEffectState : std::uint8_t {
    NotApplicable = 0,
    NotStarted = 1,
    Committed = 2,
    ConfirmedNotExecuted = 3,
    Unknown = 4,
    Compensated = 5
};

struct ObservationContext {
    std::string request_id;
    std::string trace_id;
    std::string span_id;
    std::optional<std::string> causal_parent_event_id;
    std::optional<std::string> session_id;
    std::optional<std::string> plan_id;
    std::optional<std::string> pid;
    std::optional<std::string> activation_id;
    std::optional<std::string> execution_id;
    std::string producer_endpoint_id;
    std::uint64_t producer_epoch = 1;
    std::uint64_t producer_sequence = 0;
    std::uint64_t boot_id = 1;
    TaskPriority task_priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
};

enum class ErrorSeverityHint : std::uint8_t {
    Info,
    Warning,
    Error,
    Critical
};

enum class TerminalStatus : std::uint8_t {
    Pending,
    Succeeded,
    Failed,
    Cancelled,
    Unknown
};

struct StructuredError {
    StructuredError() = default;

    /// Convenience constructor for the core error fields. Safe-detail fields
    /// are populated explicitly instead of remaining implicit aggregate tails.
    StructuredError(
        std::string error_domain, std::string error_code,
        std::string error_message, bool is_retryable = false,
        SideEffectState effect_state = SideEffectState::NotApplicable)
        : domain(std::move(error_domain)),
          code(std::move(error_code)),
          message(std::move(error_message)),
          retryable(is_retryable),
          side_effect_state(effect_state),
          source_module(domain),
          recoverable_hint(is_retryable),
          safe_detail_code(code),
          safe_detail_summary(message) {}

    std::string domain;
    std::string code;
    std::string message;
    bool retryable = false;
    SideEffectState side_effect_state =
        SideEffectState::NotApplicable;

    // Frozen error-envelope fields.
    ErrorSeverityHint severity_hint = ErrorSeverityHint::Error;
    std::string source_module;
    std::string source_interface;
    std::string operation;
    ObservationContext context;
    bool recoverable_hint = false;
    std::string retry_scope_hint;
    std::string safe_detail_code;
    std::string safe_detail_summary;
    std::vector<std::string> evidence_event_ids;
    std::vector<std::string> evidence_object_refs;
    std::vector<std::string> privacy_labels;
};

struct Status {
    bool ok = true;
    StructuredError error;

    static Status Ok();

    static Status Error(std::string domain, std::string code,
                         std::string message, bool retryable = false,
                         SideEffectState side_effect_state =
                             SideEffectState::NotApplicable);
};

template <typename T>
struct Result {
    Status status;
    std::optional<T> value;

    static Result<T> Success(T result) {
        return {Status::Ok(), std::move(result)};
    }

    static Result<T> Failure(Status failure) {
        return {std::move(failure), std::nullopt};
    }
};

struct CallContext {
    CallContext(
        CallerModuleId caller_module = CallerModuleId::Invalid,
        std::string request = {}, std::string trace = {},
        std::string principal_hash = {},
        TaskPriority task_priority = TaskPriority::P1,
        std::int64_t absolute_deadline_mono_ns = 0,
        std::string endpoint = {}, std::uint64_t process_epoch = 0,
        std::string authorization = {});

    // Fail closed: every cross-module call must name its real caller.
    CallerModuleId caller = CallerModuleId::Invalid;
    std::string request_id;
    std::string trace_id;
    std::string principal_id_hash;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string caller_endpoint_id;
    std::uint64_t caller_process_epoch = 0;
    std::string authorization_ref;
};

/// Returns the canonical endpoint for a module hosted by this process.
std::string hostModuleEndpoint(CallerModuleId module);

/// Returns a boot-unique epoch for an in-process module identity.
std::uint64_t hostModuleProcessEpoch(CallerModuleId module);

/// Fail-closed caller check used by in-process trust boundaries.
bool hasHostModuleIdentity(const CallContext& call,
                           CallerModuleId expected_module);

/// Re-issues a child context under the real downstream-calling module
/// instead of mutating only the enum and accidentally retaining an
/// upstream endpoint/epoch.
CallContext makeChildCallContext(const CallContext& parent,
                                 CallerModuleId child_module);

class IRuntimeClock {
public:
    virtual ~IRuntimeClock() = default;

    virtual std::int64_t utcNowMs() const = 0;

    virtual std::int64_t monotonicNowNs() const = 0;
};

class SystemRuntimeClock final : public IRuntimeClock {
public:
    std::int64_t utcNowMs() const override;

    std::int64_t monotonicNowNs() const override;
};

class ManualRuntimeClock final : public IRuntimeClock {
public:
    explicit ManualRuntimeClock(
        std::int64_t utc_ms = 1785200000000LL,
        std::int64_t mono_ns = 1000000000LL);

    std::int64_t utcNowMs() const override;

    std::int64_t monotonicNowNs() const override;

    void advanceMs(std::int64_t delta_ms);

private:
    std::atomic<std::int64_t> utc_ms_;
    std::atomic<std::int64_t> mono_ns_;
};

class IdGenerator {
public:
    explicit IdGenerator(std::string boot_prefix = "boot");

    std::string next(const std::string& kind);

private:
    std::string boot_prefix_;
    std::atomic<std::uint64_t> sequence_{1};
};

bool isHigherPriority(TaskPriority left, TaskPriority right);

/// Closed-enum guard required at every trust boundary.
bool isValidTaskPriority(TaskPriority priority);

/// Closed-enum guard for side-effect truth received across trust boundaries.
bool isValidSideEffectState(SideEffectState state);

bool deadlineExpired(std::int64_t deadline_mono_ns,
                     const IRuntimeClock& clock);

std::string toString(CallerModuleId module);

std::string toString(TaskPriority priority);

std::string toString(SideEffectState state);

std::string stableDigest(const std::string& value);

std::string secureDigest(const std::string& value);

/// Builds a collision-resistant, principal-scoped ledger key. Length
/// framing is mandatory: concatenating untrusted fields with a delimiter
/// makes ("a|b", "c") collide with ("a", "b|c").
std::string scopedIdempotencyLedgerKey(
    const std::string& principal_id_hash,
    const std::string& idempotency_key);

}  // namespace master_agent
