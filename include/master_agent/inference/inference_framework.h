#pragma once

/**
 * @file inference_framework.h
 * @brief Defines inference jobs, replicas, admission, cancellation, and preemption contracts.
 */

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "master_agent/common/types.h"
#include "master_agent/kv_cache/kv_cache_manager.h"

namespace master_agent::inference {

enum class InferenceJobState : std::uint8_t {
    Accepted,
    Ready,
    Running,
    Suspended,
    Completed,
    Failed,
    Cancelled
};

struct InferenceAdmissionContext {
    std::string principal_id;
    CallerModuleId caller_module_id =
        CallerModuleId::IntentRecognitionEngine;
    std::string source_request_id;
    TaskPriority granted_priority = TaskPriority::P1;
    bool p0_authorization = false;
    std::string policy_snapshot_id;
    std::vector<std::string> allowed_model_profiles;
    std::uint32_t max_input_tokens = 4096;
    std::uint32_t max_output_tokens = 1024;
    std::int64_t deadline_mono_ns = 0;
    std::string signature_ref;
};

struct InferenceRequest {
    std::string job_id;
    std::string request_id;
    std::string parent_operation_id;
    std::string session_id;
    std::string prompt;
    std::string prompt_digest;
    std::vector<kv_cache::PromptSegment> prompt_segments;
    std::string prompt_protocol_version = "prompt-v2";
    // FIRST_INFERENCE selects QUERY_BATCH or NO_QUERY+FINAL.
    // SECOND_INFERENCE consumes a frozen EvidenceBundle and may only return
    // a final decision. The phase is part of the idempotency digest.
    std::string inference_phase = "FIRST_INFERENCE";
    std::string model = "mock-master-agent";
    std::string adapter;
    std::string kv_reuse_policy = "PREFIX";
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string idempotency_key;
    InferenceAdmissionContext admission;
    // Required only when caller_module_id == SubAgent. These fields bind the
    // model job to the live parent Dispatch/AgentLease.
    std::string parent_dispatch_id;
    std::string parent_agent_id;
    std::uint64_t parent_agent_epoch = 0;
    std::string parent_lease_id;
    std::uint64_t parent_fencing_token = 0;
    std::string reality = "SIMULATED";
    std::string trace_id;
};

/// Stable SubAgent child-inference idempotency identity.
std::string inferenceChildIdempotencyKey(
    const std::string& parent_dispatch_id,
    const std::string& child_job_id);

/// Read-only/reservation seam for validating a SubAgent model job against
/// the parent AgentLease.
class IInferenceParentLineageValidator {
public:
    virtual ~IInferenceParentLineageValidator() = default;

    virtual Status validateInferenceParentLineage(
        const InferenceRequest& request,
        const CallContext& call) const = 0;

    virtual Result<std::string>
    acquireInferenceParentInvocationLease(
        const InferenceRequest& request,
        const CallContext& call) {
        const auto validated =
            validateInferenceParentLineage(request, call);
        if (!validated.ok) {
            return Result<std::string>::Failure(validated);
        }
        return Result<std::string>::Success(
            "validated-inference-parent-lease|" +
            request.job_id);
    }

    virtual Status releaseInferenceParentInvocationLease(
        const std::string&) {
        return Status::Ok();
    }
};

/// Immutable cross-process identity for one Runtime invocation. A callback
/// that does not echo this exact seal is stale and cannot commit.
struct RuntimeInvocationSeal {
    std::string job_id;
    std::string attempt_id;
    std::string operation_id;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::string lease_id;
    std::uint64_t fencing_token = 0;
    std::uint64_t control_epoch = 0;
    std::string prompt_digest;
    std::string model_digest;
    std::int64_t deadline_mono_ns = 0;
    std::string invocation_id;
};

std::string runtimeInvocationDigest(
    const RuntimeInvocationSeal& seal);

/**
 * @brief Outcome of a streamed invocation, as observed by the framework.
 *
 * The framework accumulates chunk deltas itself and compares them against the
 * committed raw_output, so a runtime cannot misreport what it streamed.
 */
enum class StreamIntegrity : std::uint8_t {
    /// Not a streamed invocation.
    NotStreamed,
    /// Concatenated deltas matched the committed raw_output exactly.
    Verified,
    /// Deltas diverged from raw_output. The output is rejected: presentation
    /// sinks already emitted text that the decision path would not agree with.
    Diverged,
    /// The framework asked the runtime to stop and it complied.
    Aborted
};

struct InferenceOutput {
    std::string raw_output;
    std::string finish_reason;
    std::string model_id;
    std::string model_digest;
    std::string job_id;
    std::string operation_id;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::string lease_id;
    std::uint64_t fencing_token = 0;
    std::uint64_t control_epoch = 0;
    std::string attempt_id;
    std::string prompt_digest;
    std::string invocation_id;
    std::uint32_t prompt_token_count = 0;
    std::uint32_t generated_token_count = 0;
    std::uint64_t total_latency_ms = 0;
    std::string runtime_backend = "mock";
    std::string reality = "SIMULATED";
    std::string output_digest;
    /// Framework-observed streaming outcome. Deliberately excluded from
    /// inferenceOutputDigest: these are framework observations about the
    /// runtime, not claims made by the runtime, and including them would
    /// change every previously computed digest.
    StreamIntegrity stream_integrity = StreamIntegrity::NotStreamed;
    std::uint32_t streamed_chunk_count = 0;
    /// Monotonic time of the first accepted chunk. Enables first-token latency
    /// measurement, which total_latency_ms cannot express.
    std::int64_t first_chunk_mono_ns = 0;
};

/// Digest of the complete result envelope, excluding output_digest itself.
std::string inferenceOutputDigest(const InferenceOutput& output);

/**
 * @brief One incremental slice of model output.
 *
 * A chunk is SPECULATIVE. The framework commit fence runs only after the
 * runtime call returns, so any chunk may still be discarded when the job is
 * cancelled, preempted, fenced by a rebuilt replica, or past its deadline.
 * Chunks may therefore be routed to presentation sinks (TTS, UI) but must
 * never reach the decision path: only a committed InferenceOutput may produce
 * a PLAN or authorize a tool invocation.
 */
struct InferenceChunk {
    /// Must echo RuntimeInvocationSeal::invocation_id verbatim. A chunk that
    /// does not is stale or misrouted and is rejected.
    std::string invocation_id;
    /// Monotonic from 0 within one invocation. Gaps and repeats are rejected.
    std::uint32_t chunk_index = 0;
    std::string delta;
    /// True exactly once, on the last chunk of the stream.
    bool final = false;
    /// Populated only when final is true.
    std::string finish_reason;
};

/// Framework verdict handed back to the runtime for each delivered chunk.
/// Abort is a cooperative preemption request; runtimes must stop generating
/// and return promptly. It is never a thread termination.
enum class StreamControl : std::uint8_t {
    Continue,
    Abort
};

/// Sink invoked by the runtime on the generating thread, outside framework
/// state locks. Implementations must not block for long.
using InferenceStreamSink =
    std::function<StreamControl(const InferenceChunk&)>;

struct InferenceJobSnapshot {
    std::string job_id;
    std::string attempt_id;
    std::string operation_id;
    InferenceJobState state = InferenceJobState::Accepted;
    TaskPriority base_priority = TaskPriority::P1;
    TaskPriority effective_priority = TaskPriority::P1;
    std::uint64_t enqueue_sequence = 0;
    std::int64_t queued_at_mono_ns = 0;
    std::int64_t started_at_mono_ns = 0;
    std::int64_t deadline_mono_ns = 0;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::string lease_id;
    std::uint64_t fencing_token = 0;
    std::string stage;
    std::string checkpoint_ref;
    std::optional<InferenceOutput> result;
    std::optional<StructuredError> last_error;
    std::uint64_t control_epoch = 0;
    std::uint64_t version = 1;
};

struct InferenceAcceptance {
    bool accepted = false;
    bool existing = false;
    std::string job_id;
    std::string reject_code;
};

struct InferenceEvent {
    std::string event_id;
    std::string event_type;
    std::string job_id;
    std::string attempt_id;
    std::string operation_id;
    std::string request_id;
    std::string parent_operation_id;
    InferenceJobState state = InferenceJobState::Accepted;
    std::string stage;
    TaskPriority priority = TaskPriority::P1;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::string lease_id;
    std::uint64_t fencing_token = 0;
    std::string checkpoint_ref;
    std::optional<InferenceOutput> result;
    std::optional<StructuredError> last_error;
    bool resource_released = false;
    std::int64_t occurred_at_utc_ms = 0;
    std::string trace_id;
};

/**
 * @brief Runtime adapter that performs model work outside framework state locks.
 *
 * Outputs are committed only when the job attempt, replica epoch, control epoch,
 * KV lease, and fencing token still match the frozen invocation seal.
 */
class IModelRuntime {
public:
    virtual ~IModelRuntime() = default;

    /// Short identifier used in the model_digest field of RuntimeInvocationSeal.
    /// The framework computes: secureDigest(request.model + "|" + runtimeTag()).
    /// A runtime that returns a different tag than what the framework used will
    /// fail seal validation, which is the correct fail-closed behaviour.
    virtual std::string runtimeTag() const { return "mock-runtime"; }

    /// Estimates cooperative scheduling quanta; it must not invoke the model.
    virtual std::uint32_t requiredWorkUnits(
        const InferenceRequest& request) const = 0;

    /**
     * Executes model work without holding framework state locks.
     *
     * Implementations must copy every identity and fencing field from seal into
     * the output so the framework can reject stale or misrouted callbacks.
     */
    virtual Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) = 0;

    /// True when inferStream delivers incremental chunks. A false value is not
    /// an error: the framework falls back to whole-response delivery.
    virtual bool supportsStreaming() const { return false; }

    /**
     * Executes model work, delivering incremental output through sink.
     *
     * Contract:
     *  - sink is called on the calling thread, outside framework state locks.
     *  - Every chunk must echo seal.invocation_id and carry a monotonic
     *    chunk_index starting at 0.
     *  - Exactly one chunk has final == true, delivered last.
     *  - When sink returns StreamControl::Abort, generation must stop and the
     *    call must return promptly. Returning a partial output is correct;
     *    the framework decides whether to commit it.
     *  - The returned InferenceOutput carries the SAME obligations as infer():
     *    raw_output must equal the ordered concatenation of every delivered
     *    delta, and all seal identity fields must be echoed. The framework
     *    verifies the concatenation independently and rejects a divergence.
     *
     * The default implementation calls infer() and delivers the whole response
     * as one final chunk, so a non-streaming runtime remains correct.
     */
    virtual Result<InferenceOutput> inferStream(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal,
        const InferenceStreamSink& sink) {
        auto output = infer(request, seal);
        if (!output.status.ok || !output.value) {
            return output;
        }
        if (sink) {
            InferenceChunk chunk;
            chunk.invocation_id = seal.invocation_id;
            chunk.chunk_index = 0;
            chunk.delta = output.value->raw_output;
            chunk.final = true;
            chunk.finish_reason = output.value->finish_reason;
            (void)sink(chunk);
        }
        return output;
    }
};

/// Simulation-only record of the exact mock model boundary. Raw content is
/// retained only in this in-process test double; integrated audit
/// events retain digests/redacted metadata instead.
struct MockModelInvocation {
    std::uint64_t sequence = 0;
    std::string request_id;
    std::string job_id;
    std::string inference_phase;
    std::string prompt_protocol_version;
    std::string prompt;
    std::string prompt_digest;
    std::string raw_output;
    std::string output_digest;
    std::string reality = "SIMULATED";
};

class MockModelRuntime final : public IModelRuntime {
public:
    explicit MockModelRuntime(std::uint32_t work_units = 1);

    std::uint32_t requiredWorkUnits(
        const InferenceRequest& request) const override;

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override;

    void setWorkUnits(std::uint32_t units);

    void failNext(std::string error_code);

    /// Returns a stable copy in invocation order for diagnostics and tests.
    std::vector<MockModelInvocation> invocations() const;

private:
    mutable std::mutex mutex_;
    std::uint32_t work_units_;
    std::string next_error_;
    std::uint64_t invocation_sequence_ = 0;
    std::vector<MockModelInvocation> invocations_;
};

/**
 * @brief Governs inference admission, scheduling, cancellation, and preemption.
 *
 * submitInference acknowledges queue admission. Completion is observed through
 * queryInference. Preemption occurs only at a safe point and never terminates a
 * worker thread directly.
 */
class IInferenceFramework {
public:
    virtual ~IInferenceFramework() = default;

    /**
     * Validates and admits a job. Acceptance confirms queue ownership only;
     * callers obtain terminal output through queryInference().
     */
    virtual InferenceAcceptance submitInference(
        const InferenceRequest& request,
        const CallContext& call) = 0;

    /// Returns the authoritative snapshot for the stable job identity.
    virtual Result<InferenceJobSnapshot> queryInference(
        const std::string& job_id, const CallContext& call) const = 0;

    /// Requests cooperative cancellation; control_epoch fences stale commands.
    virtual Status cancelInference(const std::string& job_id,
                                   std::uint64_t control_epoch,
                                   const CallContext& call) = 0;

    /**
     * Registers a presentation sink for a job's streamed output.
     *
     * The sink is deliberately not a field on InferenceRequest: the request is
     * a serializable value that crosses the IPC boundary and is persisted for
     * idempotency, and a callback is neither.
     *
     * Chunks delivered to the sink are SPECULATIVE. They are correct for
     * presentation (TTS, UI) and must not drive decisions: only a committed
     * InferenceOutput may produce a PLAN or authorize a tool call. A sink that
     * returns StreamControl::Abort requests cooperative stop, which is how
     * barge-in is expressed; it never terminates a thread.
     *
     * Returning Abort does not by itself fail the job. The framework records
     * StreamIntegrity::Aborted and applies the normal commit fence to whatever
     * partial output the runtime returned.
     *
     * Defaulted to fail closed so an implementation that does not stream
     * refuses the sink outright rather than accepting it and silently never
     * calling it. A caller that got Ok knows chunks will actually arrive.
     */
    virtual Status attachStreamSink(const std::string& job_id,
                                    std::uint64_t control_epoch,
                                    InferenceStreamSink sink,
                                    const CallContext& call) {
        (void)job_id;
        (void)control_epoch;
        (void)sink;
        (void)call;
        return Status::Error(
            "inference", "INFERENCE_STREAM_SINK_UNSUPPORTED",
            "this inference framework does not deliver streamed chunks");
    }

    /**
     * Requests priority preemption at the next checkpoint-safe boundary.
     * The scheduler never force-terminates a runtime thread.
     */
    virtual Status requestPreempt(const std::string& job_id,
                                  TaskPriority arriving_priority,
                                  std::uint64_t control_epoch,
                                  const CallContext& call) = 0;

    /// Fence all old leases/callbacks for a rebuilt physical Replica.
    virtual Status rebuildReplica(
        const std::string& replica_id,
        const CallContext& call) = 0;

    /// Advances at most one owner-loop scheduling transition.
    virtual bool pumpOne() = 0;

    /// Drives the deterministic scheduler until quiescent or max_steps is reached.
    virtual Status runUntilIdle(std::size_t max_steps = 10000) = 0;

    virtual std::vector<InferenceEvent> events() const = 0;
};

class InferenceFramework final : public IInferenceFramework {
public:
    InferenceFramework(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<IModelRuntime> runtime,
        std::shared_ptr<kv_cache::IKvCacheManager> kv_cache,
        std::size_t replica_count = 2,
        std::shared_ptr<IInferenceParentLineageValidator>
            lineage_validator = nullptr);

    InferenceAcceptance submitInference(
        const InferenceRequest& request,
        const CallContext& call) override;

    Result<InferenceJobSnapshot> queryInference(
        const std::string& job_id,
        const CallContext& call) const override;

    Status cancelInference(const std::string& job_id,
                           std::uint64_t control_epoch,
                           const CallContext& call) override;

    Status attachStreamSink(const std::string& job_id,
                            std::uint64_t control_epoch,
                            InferenceStreamSink sink,
                            const CallContext& call) override;

    Status requestPreempt(const std::string& job_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;

    Status rebuildReplica(
        const std::string& replica_id,
        const CallContext& call) override;

    bool pumpOne() override;

    Status runUntilIdle(std::size_t max_steps = 10000) override;

    std::vector<InferenceEvent> events() const override;

private:
    /// External components are never entered while mutex_ is held. The
    /// operation kind/token is a two-phase-commit fence, so a re-entrant
    /// callback cannot overwrite a newer cancellation or preemption.
    enum class ExternalOperation : std::uint8_t {
        None,
        RuntimeSizing,
        KvAcquire,
        KvCompleteUse,
        RuntimeInfer,
        KvPublish
    };

    struct Job {
        InferenceRequest request;
        // Frozen authenticated caller is reused at the physical model
        // boundary; trust fields are never reconstructed from payload data.
        CallContext submit_call;
        InferenceJobSnapshot snapshot;
        std::uint32_t remaining_work_units = 0;
        std::optional<kv_cache::KvCacheLease> kv_lease;
        // Allocated once per acquired lease and retained for timeout replay.
        std::string kv_complete_id;
        bool kv_restore_succeeded = true;
        std::string request_digest;
        ExternalOperation external_operation = ExternalOperation::None;
        std::uint64_t external_token = 0;
    };

    static Status validateSubmitCaller(const InferenceRequest& request,
                                       const CallContext& call);

    std::optional<std::string> selectReadyJob() const;

    std::optional<std::string> selectPreemptionVictim(
        TaskPriority arriving) const;

    bool startJob(const std::string& job_id,
                  const std::string& replica_id);

    bool completeJob(const std::string& job_id);

    Status releaseKvLease(const std::string& job_id);

    /**
     * Copies out the registered sink for a job, guarded by invocation identity.
     *
     * The returned wrapper holds the sink by value so the runtime never invokes
     * caller code while mutex_ is held, consistent with every other external
     * boundary in this framework.
     */
    InferenceStreamSink streamSinkFor(
        const std::string& job_id,
        const std::string& invocation_id) const;

    void emit(Job& job, const std::string& event_type,
              bool resource_released = false);

    std::optional<std::string> freeReplica() const;

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<IModelRuntime> runtime_;
    std::shared_ptr<kv_cache::IKvCacheManager> kv_cache_;
    std::shared_ptr<IInferenceParentLineageValidator>
        lineage_validator_;
    mutable std::mutex mutex_;
    std::map<std::string, Job> jobs_;
    /// Presentation sinks by job_id. Erased when the job reaches a terminal
    /// state so a completed job cannot retain caller-owned state.
    std::map<std::string, InferenceStreamSink> stream_sinks_;
    std::map<std::string, std::string> idempotency_to_job_;
    std::vector<std::string> replicas_;
    std::map<std::string, std::uint64_t> replica_epochs_;
    std::map<std::string, std::uint64_t>
        replica_fencing_sequences_;
    std::map<std::string, std::string> replica_to_job_;
    // Round-robin cursor for the isolated KV completeUse cleanup lane. A
    // permanently failing lease must not starve later cleanup work.
    std::string kv_cleanup_cursor_;
    std::vector<InferenceEvent> events_;
    std::uint64_t enqueue_sequence_ = 0;
};

}  // namespace master_agent::inference
