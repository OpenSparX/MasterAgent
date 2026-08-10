/**
 * @file test_contracts.cpp
 * @brief Verifies cross-module identity, schema, deadline, and policy contracts.
 */

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/intent/intent_engine.h"
#include "master_agent/kv_cache/kv_cache_manager.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/preprocess/preprocess_engine.h"
#include "master_agent/prompt/prompt_engine.h"
#include "master_agent/skill/skill_engine.h"
#include "test_support.h"

using namespace master_agent;
using namespace master_agent::atomic_service;
using namespace master_agent::kv_cache;
using master_agent::test_support::expect;
namespace dispatch = master_agent::agent_dispatch;
namespace inference = master_agent::inference;
namespace intent = master_agent::intent;
namespace orchestrator = master_agent::orchestrator;
namespace prompt = master_agent::prompt;
namespace skill = master_agent::skill;

namespace {

class CountingMemoryClient final
    : public master_agent::memory::IMemoryClient {
public:
    master_agent::memory::WriteMemoryResult writeMemory(
        const master_agent::memory::WriteMemoryRequest&) override {
        ++write_calls;
        master_agent::memory::WriteMemoryResult result;
        result.success = true;
        return result;
    }

    master_agent::memory::GetContextResult getContext(
        const master_agent::memory::GetContextRequest&) override {
        ++read_calls;
        master_agent::memory::GetContextResult result;
        result.success = true;
        return result;
    }

    std::size_t read_calls = 0;
    std::size_t write_calls = 0;
};

/// Deterministic Skill boundary used to make a result cross the original
/// intent deadline without sleeping or changing wall-clock time.
class ContractSkillEngine final
    : public intent_support::IIntentSkillResolver {
public:
    enum class Mode {
        ResolveAfterDeadline,
        NoMatch
    };

    ContractSkillEngine(
        std::shared_ptr<ManualRuntimeClock> clock, Mode mode)
        : clock_(std::move(clock)), mode_(mode) {}

    Result<std::vector<McpToolDefinition>> listAvailableTools(
        const CallContext&) const override {
        return Result<std::vector<McpToolDefinition>>::Success({});
    }

    intent_support::DeterministicResolution resolveDeterministic(
        const std::string&, const CallContext&) const override {
        ++resolve_calls_;
        intent_support::DeterministicResolution result;
        if (mode_ == Mode::NoMatch) {
            result.state =
                intent_support::DeterministicResolutionState::NoMatch;
            return result;
        }
        clock_->advanceMs(2);
        result.state =
            intent_support::DeterministicResolutionState::Resolved;
        result.tool_name =
            "com_sgm_service_climate_setAirCirculationMode";
        result.arguments =
            nlohmann::json{{"mode", "INTERNAL"}};
        result.user_reply = "must-not-commit";
        result.reason_code = "LATE_SKILL_MATCH";
        return result;
    }

    std::size_t resolveCalls() const {
        return resolve_calls_;
    }

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
    Mode mode_;
    mutable std::size_t resolve_calls_ = 0;
};

class ContractPromptEngine final
    : public intent_support::IIntentPromptAssembler {
public:
    Result<intent_support::PromptPackage> assembleIntentPrompt(
        const interaction::StandardRequest& request,
        const memory::MemoryContext&,
        const CallContext&) const override {
        intent_support::PromptPackage package;
        package.prompt = "contract prompt: " + request.text;
        package.prompt_digest = secureDigest(package.prompt);
        package.segments = {
            {"intent-contract", package.prompt_digest, 8}};
        package.protocol_version = "prompt-v2-contract";
        return Result<intent_support::PromptPackage>::Success(
            std::move(package));
    }
};

/// Completed inference seam that advances ManualRuntimeClock either while
/// runUntilIdle returns or while the completed snapshot is queried.
class DeadlineAdvancingInference final
    : public inference::IInferenceFramework {
public:
    enum class AdvancePoint {
        Completion,
        Query,
        Never
    };

    DeadlineAdvancingInference(
        std::shared_ptr<ManualRuntimeClock> clock,
        AdvancePoint advance_point,
        std::string raw_output =
            R"json({"outcome":"REPLY","reply":"late direct reply"})json")
        : clock_(std::move(clock)),
          advance_point_(advance_point),
          raw_output_(std::move(raw_output)) {}

    inference::InferenceAcceptance submitInference(
        const inference::InferenceRequest& request,
        const CallContext&) override {
        ++submit_calls_;
        job_id_ = request.job_id;
        return {true, false, request.job_id, {}};
    }

    Result<inference::InferenceJobSnapshot> queryInference(
        const std::string& job_id,
        const CallContext&) const override {
        ++query_calls_;
        if (advance_point_ == AdvancePoint::Query) {
            clock_->advanceMs(2);
        }
        if (job_id != job_id_) {
            return Result<
                inference::InferenceJobSnapshot>::Failure(
                Status::Error(
                    "test_inference",
                    "TEST_INFERENCE_JOB_ID_MISMATCH",
                    "query did not bind the submitted job"));
        }
        inference::InferenceJobSnapshot snapshot;
        snapshot.job_id = job_id;
        snapshot.state =
            inference::InferenceJobState::Completed;
        inference::InferenceOutput output;
        output.raw_output = raw_output_;
        snapshot.result = std::move(output);
        return Result<inference::InferenceJobSnapshot>::Success(
            std::move(snapshot));
    }

    Status cancelInference(
        const std::string&, std::uint64_t,
        const CallContext&) override {
        return Status::Ok();
    }

    Status requestPreempt(
        const std::string&, TaskPriority, std::uint64_t,
        const CallContext&) override {
        return Status::Ok();
    }

    Status rebuildReplica(
        const std::string&, const CallContext&) override {
        return Status::Ok();
    }

    bool pumpOne() override {
        return false;
    }

    Status runUntilIdle(std::size_t) override {
        if (advance_point_ == AdvancePoint::Completion) {
            clock_->advanceMs(2);
        }
        return Status::Ok();
    }

    std::vector<inference::InferenceEvent> events()
        const override {
        return {};
    }

    std::size_t submitCalls() const {
        return submit_calls_;
    }

    std::size_t queryCalls() const {
        return query_calls_;
    }

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
    AdvancePoint advance_point_;
    std::string raw_output_;
    std::string job_id_;
    std::size_t submit_calls_ = 0;
    mutable std::size_t query_calls_ = 0;
};

struct IntentContractInput {
    interaction::StandardRequest request;
    intent::IntentContext context;
    CallContext call;
};

IntentContractInput intentContractInput(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const std::string& suffix) {
    IntentContractInput input;
    const auto deadline =
        clock->monotonicNowNs() + 1'000'000LL;
    input.request.request_id = "intent-contract-" + suffix;
    input.request.trace_id = "trace-intent-" + suffix;
    input.request.text = "plan a non-greeting request";
    input.request.user_id = "driver";
    input.request.session_id = "intent-session";
    input.request.turn_id = 1;
    input.request.priority = TaskPriority::P1;
    input.request.deadline_mono_ns = deadline;
    input.context.preprocess_result.normalized_request =
        input.request;
    input.context.preprocess_result.valid = true;
    input.context.session_id = input.request.session_id;
    input.context.turn_id = input.request.turn_id;
    input.context.context_version = 1;
    input.context.priority = input.request.priority;
    input.context.deadline_mono_ns = deadline;
    input.context.expected_capability_digest =
        "intent-contract-capability";
    input.call = {
        CallerModuleId::AgentService, input.request.request_id,
        input.request.trace_id, "intent-principal",
        input.request.priority, deadline, {}, 0,
        "intent-authorization"};
    return input;
}

void testMemoryRejectsExpiredCallsBeforeExternalIo() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto client = std::make_shared<CountingMemoryClient>();
    memory::MemoryService service(client, clock);
    interaction::StandardRequest request;
    request.request_id = "memory-deadline-request";
    request.trace_id = "memory-deadline-trace";
    request.user_id = "driver";
    request.session_id = "session";
    request.turn_id = 1;
    request.priority = TaskPriority::P1;
    request.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000LL;
    CallContext call{
        CallerModuleId::AgentService, request.request_id,
        request.trace_id, "principal", request.priority,
        request.deadline_mono_ns};
    clock->advanceMs(2);
    const auto context =
        service.getContext(request, "query", call);
    memory::CompletedTurn turn;
    turn.request = request;
    turn.normalized_user_input = "query";
    turn.assistant_output = "reply";
    const auto write = service.writeTurn(turn, call);
    expect(!context.status.ok &&
               context.status.error.code ==
                   "MEMORY_CALL_EXPIRED" &&
               !write.ok &&
               write.error.code == "MEMORY_CALL_EXPIRED" &&
               client->read_calls == 0 &&
               client->write_calls == 0,
           "expired memory calls must be fenced before SDK I/O");
}

void testPreprocessStateContracts() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    preprocess::PreprocessEngine engine(clock);
    const auto deadline =
        clock->monotonicNowNs() + 1'000'000'000LL;
    CallContext call{CallerModuleId::AgentService, "state-request",
                     "state-trace", "principal", TaskPriority::P1,
                     deadline};
    const auto capabilities = engine.getCapabilities(call);
    expect(capabilities.status.ok && capabilities.value &&
               capabilities.value->size() == 2,
           "preprocess must publish its bounded state capabilities");

    preprocess::StateQuery query;
    query.request_id = call.request_id;
    query.session_id = "session";
    query.turn_id = 1;
    query.state_type =
        preprocess::StateDomain::Vehicle;
    query.fields = {"speed_kmh", "is_parked"};
    const auto state = engine.queryRuntimeState(query, call);
    expect(state.status.ok && state.value && state.value->success &&
               state.value->values.at("speed_kmh") == "0" &&
               state.value->values.at("is_parked") == "true",
           "preprocess state query must return only declared fields");

    auto denied_call = call;
    denied_call.caller = CallerModuleId::IntentRecognitionEngine;
    const auto denied = engine.getCapabilities(denied_call);
    expect(!denied.status.ok &&
               denied.status.error.code ==
                   "PREPROCESS_CALLER_NOT_ALLOWED",
           "preprocess state contracts must reject non-AgentService callers");
}

void testKvContextInvalidateAndLeaseProtection() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("kv-contract");
    KvCacheManager cache(clock, ids, 1024 * 1024);
    const auto deadline = clock->monotonicNowNs() + 5'000'000'000LL;
    CallContext call{CallerModuleId::InferenceFramework, "request-kv",
                     "trace-kv", "principal-kv", TaskPriority::P1,
                     deadline};

    KvCachePublishRequest publish;
    publish.publish_id = "publish-1";
    publish.request_id = call.request_id;
    publish.job_id = "job-producer";
    publish.namespace_id = "namespace-1";
    publish.runtime_fingerprint_digest = "runtime-fingerprint";
    publish.segments = {{"system", "segment-digest", 64}};
    publish.replica_id = "replica-1";
    publish.replica_epoch = 7;
    publish.runtime_cache_handle = "sealed-handle";
    publish.size_bytes = 4096;
    publish.priority = call.priority;
    publish.deadline_mono_ns = call.deadline_mono_ns;
    const auto published = cache.publish(publish, call);
    expect(published.status.ok && published.value &&
               published.value->admitted,
           "context-bound KV publish must be admitted");

    KvCacheAcquireRequest acquire;
    acquire.request_id = call.request_id;
    acquire.job_id = "job-consumer";
    acquire.namespace_id = publish.namespace_id;
    acquire.runtime_fingerprint_digest =
        publish.runtime_fingerprint_digest;
    acquire.segments = publish.segments;
    acquire.replica_candidates = {publish.replica_id};
    acquire.replica_candidate_epochs[publish.replica_id] =
        publish.replica_epoch;
    acquire.priority = call.priority;
    acquire.deadline_mono_ns = call.deadline_mono_ns;
    const auto hit = cache.acquire(acquire, call);
    expect(hit.status.ok && hit.value && hit.value->lease,
           "published prefix must produce an owned lease");

    KvCacheInvalidateRequest invalidate;
    invalidate.invalidate_id = "invalidate-1";
    invalidate.request_id = call.request_id;
    invalidate.cache_id = published.value->cache_id;
    invalidate.reason_code = "MODEL_RETIRED";
    invalidate.priority = call.priority;
    invalidate.deadline_mono_ns = call.deadline_mono_ns;
    const auto invalidated = cache.invalidate(invalidate, call);
    expect(invalidated.status.ok && invalidated.value &&
               invalidated.value->invalidated_count == 1 &&
               invalidated.value->protected_by_lease_count == 1,
           "invalidate must fence hits while preserving an active lease");

    acquire.job_id = "job-after-invalidate";
    const auto miss = cache.acquire(acquire, call);
    expect(miss.status.ok && miss.value &&
               miss.value->outcome == AcquireOutcome::Miss,
           "invalidated entry must stop serving new leases");

    const auto completed = cache.completeUse(
        {"complete-1", hit.value->lease->lease_id, "job-consumer", true},
        call);
    expect(completed.ok, "active lease must close idempotently");
    const auto status = cache.queryStatus(call);
    expect(status.active_leases == 0 && status.ready_entries == 0,
           "unleased invalidated entry must be reclaimed");

    const auto replay = cache.invalidate(invalidate, call);
    expect(replay.status.ok && replay.value && replay.value->existing,
           "same invalidate command must replay its original result");
    invalidate.reason_code = "DIFFERENT_REASON";
    const auto conflict = cache.invalidate(invalidate, call);
    expect(!conflict.status.ok &&
               conflict.status.error.code ==
                   "KV_INVALIDATE_IDEMPOTENCY_CONFLICT",
           "invalidate idempotency key must bind selector and reason");

    auto wrong_context = call;
    wrong_context.request_id = "different-request";
    publish.publish_id = "publish-context-mismatch";
    const auto rejected = cache.publish(publish, wrong_context);
    expect(!rejected.status.ok &&
               rejected.status.error.code ==
                   "KV_PUBLISH_CONTEXT_MISMATCH",
           "publish must reject an unbound CallContext");
}

/// A fenced entry remains resident until its authenticated lease completion;
/// lease expiry is not liveness proof and cannot silently release capacity.
void testKvInvalidatedLeasePinsCapacityPastAdvisoryExpiry() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("kv-pin-contract");
    KvCacheManager cache(clock, ids, 100);
    const auto business_deadline =
        clock->monotonicNowNs() + 30'000'000'000LL;
    CallContext call{CallerModuleId::InferenceFramework,
                     "kv-pin-request", "kv-pin-trace",
                     "kv-pin-principal", TaskPriority::P1,
                     business_deadline};

    KvCachePublishRequest first;
    first.publish_id = "kv-pin-publish-1";
    first.request_id = call.request_id;
    first.job_id = "kv-pin-producer";
    first.namespace_id = "kv-pin-namespace";
    first.runtime_fingerprint_digest = "kv-pin-runtime";
    first.segments = {{"system", "kv-pin-digest", 10}};
    first.replica_id = "replica-pin";
    first.replica_epoch = 9;
    first.runtime_cache_handle = "kv-pin-handle";
    first.size_bytes = 100;
    first.priority = call.priority;
    first.deadline_mono_ns = call.deadline_mono_ns;
    const auto published = cache.publish(first, call);
    expect(published.status.ok && published.value &&
               published.value->admitted,
           "initial cache entry must fill the test capacity");

    KvCacheAcquireRequest acquire;
    acquire.request_id = call.request_id;
    acquire.job_id = "kv-pin-consumer";
    acquire.namespace_id = first.namespace_id;
    acquire.runtime_fingerprint_digest =
        first.runtime_fingerprint_digest;
    acquire.segments = first.segments;
    acquire.replica_candidates = {first.replica_id};
    acquire.replica_candidate_epochs[first.replica_id] =
        first.replica_epoch;
    acquire.priority = call.priority;
    acquire.deadline_mono_ns = call.deadline_mono_ns;
    const auto hit = cache.acquire(acquire, call);
    expect(hit.status.ok && hit.value && hit.value->lease,
           "capacity test requires an active KV lease");

    KvCacheInvalidateRequest invalidate;
    invalidate.invalidate_id = "kv-pin-invalidate";
    invalidate.request_id = call.request_id;
    invalidate.cache_id = published.value->cache_id;
    invalidate.reason_code = "REPLICA_REBUILT";
    invalidate.priority = call.priority;
    invalidate.deadline_mono_ns = call.deadline_mono_ns;
    const auto invalidated = cache.invalidate(invalidate, call);
    expect(invalidated.status.ok && invalidated.value &&
               invalidated.value->protected_by_lease_count == 1,
           "invalidate must retain the entry protected by its lease");

    auto second = first;
    second.publish_id = "kv-pin-publish-2";
    second.job_id = "kv-pin-producer-2";
    second.segments = {{"system", "kv-pin-digest-2", 10}};
    second.runtime_cache_handle = "kv-pin-handle-2";
    const auto rejected = cache.publish(second, call);
    expect(rejected.status.ok && rejected.value &&
               !rejected.value->admitted &&
               rejected.value->reason_code ==
                   "CAPACITY_PINNED_BY_ACTIVE_LEASES",
           "invalidated leased bytes must still count against capacity");

    clock->advanceMs(11'000);
    CallContext control{
        CallerModuleId::InferenceFramework, call.request_id,
        call.trace_id, call.principal_id_hash, call.priority,
        clock->monotonicNowNs() + 1'000'000'000LL};
    const auto pinned = cache.queryStatus(control);
    expect(pinned.active_leases == 1 && pinned.used_bytes == 100 &&
               pinned.ready_entries == 0,
           "advisory lease expiry must not silently free cache ownership");

    const auto completed = cache.completeUse(
        {"kv-pin-complete", hit.value->lease->lease_id,
         acquire.job_id, true},
        control);
    expect(completed.ok,
           "delayed authenticated completion must remain valid");
    const auto released = cache.queryStatus(control);
    expect(released.active_leases == 0 &&
               released.used_bytes == 0,
           "completion must reclaim the invalidated physical entry");
}

class InvalidOutputProvider final : public IAtomicProvider {
public:
    ProviderInvocationResult call(
        const AtomicMcpCallEnvelope&,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        ProviderInvocationResult result;
        result.state = ProviderInvocationState::Succeeded;
        result.side_effect_state = SideEffectState::Committed;
        result.completion_evidence =
            CompletionEvidence::StateVerified;
        result.result.structured_content =
            nlohmann::json{{"unexpected", true}};
        result.invocation_seal = invocation_seal;
        return result;
    }

    AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        AtomicReconcileResult result;
        result.invocation_seal = invocation_seal;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.fencing_token = envelope.runtime.fencing_token;
        result.status = ReconcileStatus::StillUnknown;
        return result;
    }
};

/// Returns a schema-valid payload but only a durable accept receipt.  The
/// climate tools require STATE_VERIFIED, so this must never become success.
class WrongCompletionEvidenceProvider final : public IAtomicProvider {
public:
    ProviderInvocationResult call(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        ProviderInvocationResult result;
        result.state = ProviderInvocationState::Succeeded;
        result.side_effect_state = SideEffectState::Committed;
        result.completion_evidence =
            CompletionEvidence::ProviderAccepted;
        result.result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedLocation",
                            envelope.mcp_request.arguments.at("location")},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
        result.result.text_content.push_back(
            result.result.structured_content.dump());
        result.invocation_seal = invocation_seal;
        return result;
    }

    AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        AtomicReconcileResult result;
        result.invocation_seal = invocation_seal;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.fencing_token = envelope.runtime.fencing_token;
        result.status = ReconcileStatus::StillUnknown;
        return result;
    }
};

AtomicMcpCallEnvelope atomicRequest(
    const McpToolCatalogSnapshot& catalog, const std::string& suffix,
    const std::string& location, std::int64_t deadline) {
    AtomicMcpCallEnvelope request;
    request.mcp_request.id = "operation-" + suffix;
    request.mcp_request.name =
        "com_sgm_service_climate_setAutoFanSpeed";
    request.mcp_request.arguments =
        nlohmann::json{{"location", location}, {"mode", "HIGH"}};
    auto& runtime = request.runtime;
    runtime.caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    runtime.request_id = "request-" + suffix;
    runtime.trace_id = "trace-" + suffix;
    runtime.plan_id = "plan-" + suffix;
    runtime.pid = "pid-" + suffix;
    runtime.activation_id = "activation-" + suffix;
    runtime.execution_id = "execution-" + suffix;
    runtime.operation_id = request.mcp_request.id;
    runtime.priority = TaskPriority::P1;
    runtime.deadline_mono_ns = deadline;
    runtime.idempotency_key = "idempotency-" + suffix;
    runtime.fencing_token = 1;
    runtime.tool_catalog_snapshot_id = catalog.snapshot_id;
    runtime.tool_digest =
        catalog.tool_digests.at(request.mcp_request.name);
    runtime.policy_digest =
        catalog.policy_digests.at(request.mcp_request.name);
    runtime.granted_permissions = {
        "vehicle.climate.write"};
    runtime.principal_id_hash = "principal";
    runtime.authorization_ref = "interactive";
    return request;
}

CallContext atomicCall(const AtomicMcpCallEnvelope& request) {
    return {CallerModuleId::TaskOrchestrationEngine,
            request.runtime.request_id, request.runtime.trace_id,
            request.runtime.principal_id_hash, request.runtime.priority,
            request.runtime.deadline_mono_ns, {}, 0,
            request.runtime.authorization_ref};
}

void testAtomicPerToolConcurrencyLimit() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("atomic-capacity");
    auto provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager manager(clock, ids, 2);
    auto policies = defaultClimateRuntimePolicies(4);
    for (auto& policy : policies) policy.max_concurrency = 1;
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace", "system", TaskPriority::P1,
                          clock->monotonicNowNs() + 10'000'000'000LL};
    expect(manager
               .registerTools(defaultClimateMcpTools(), policies, provider,
                              bootstrap)
               .ok,
           "atomic catalog registration must succeed");
    const auto catalog = manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog.status.ok && catalog.value,
           "atomic catalog must be queryable");

    const auto deadline =
        clock->monotonicNowNs() + 5'000'000'000LL;
    const auto front =
        atomicRequest(*catalog.value, "front", "FRONT", deadline);
    const auto rear =
        atomicRequest(*catalog.value, "rear", "REAR", deadline);
    expect(manager.callTool(front, atomicCall(front)).accepted &&
               manager.callTool(rear, atomicCall(rear)).accepted,
           "both independent resource calls must be admitted");
    expect(manager.pumpOne(), "one atomic call must start");

    const auto first =
        manager.queryExecution(front.runtime.execution_id,
                               atomicCall(front));
    const auto second =
        manager.queryExecution(rear.runtime.execution_id,
                               atomicCall(rear));
    expect(first.value && second.value,
           "both executions must remain queryable");
    const auto running_count =
        (first.value->state == AtomicExecutionState::Running ? 1 : 0) +
        (second.value->state == AtomicExecutionState::Running ? 1 : 0);
    expect(running_count == 1,
           "max_concurrency=1 must serialize the same MCP Tool");
    expect(manager.runUntilIdle().ok,
           "serialized atomic calls must eventually settle");
    const auto front_terminal =
        manager.queryExecution(front.runtime.execution_id,
                               atomicCall(front));
    const auto rear_terminal =
        manager.queryExecution(rear.runtime.execution_id,
                               atomicCall(rear));
    expect(front_terminal.value && rear_terminal.value &&
               front_terminal.value->state ==
                   AtomicExecutionState::Succeeded &&
               rear_terminal.value->state ==
                   AtomicExecutionState::Succeeded,
           "serialization must execute both calls, not discard the waiter");
}

void testAtomicRejectsMalformedProviderOutput() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("atomic-output");
    auto provider = std::make_shared<InvalidOutputProvider>();
    AtomicServiceManager manager(clock, ids, 1);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace", "system", TaskPriority::P1,
                          clock->monotonicNowNs() + 10'000'000'000LL};
    expect(manager
               .registerTools(defaultClimateMcpTools(),
                              defaultClimateRuntimePolicies(1), provider,
                              bootstrap)
               .ok,
           "invalid-output test catalog must register");
    const auto catalog = manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog.status.ok && catalog.value,
           "invalid-output test catalog must be queryable");
    const auto request = atomicRequest(
        *catalog.value, "invalid-output", "FRONT",
        clock->monotonicNowNs() + 5'000'000'000LL);
    const auto call = atomicCall(request);
    expect(manager.callTool(request, call).accepted,
           "schema test execution must be admitted");
    expect(manager.runUntilIdle().ok,
           "malformed output must still leave the worker idle");
    const auto snapshot =
        manager.queryExecution(request.runtime.execution_id, call);
    expect(snapshot.value &&
               snapshot.value->state == AtomicExecutionState::Unknown &&
               snapshot.value->error_code ==
                   "ATOMIC_PROVIDER_OUTPUT_SCHEMA_INVALID",
           "malformed Provider output must fail closed as UNKNOWN");
}

void testAtomicRequiresFrozenCompletionEvidence() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("atomic-completion");
    auto provider =
        std::make_shared<WrongCompletionEvidenceProvider>();
    AtomicServiceManager manager(clock, ids, 1);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace", "system", TaskPriority::P1,
                          clock->monotonicNowNs() + 10'000'000'000LL};
    expect(manager
               .registerTools(defaultClimateMcpTools(),
                              defaultClimateRuntimePolicies(1), provider,
                              bootstrap)
               .ok,
           "completion-policy test catalog must register");
    const auto catalog = manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog.status.ok && catalog.value,
           "completion-policy test catalog must be queryable");
    const auto request = atomicRequest(
        *catalog.value, "wrong-completion", "FRONT",
        clock->monotonicNowNs() + 5'000'000'000LL);
    const auto call = atomicCall(request);
    expect(manager.callTool(request, call).accepted,
           "completion-policy execution must be admitted");
    expect(manager.runUntilIdle().ok,
           "completion-policy rejection must leave worker idle");
    const auto snapshot =
        manager.queryExecution(request.runtime.execution_id, call);
    expect(snapshot.value &&
               snapshot.value->state == AtomicExecutionState::Unknown &&
               snapshot.value->error_code ==
                   "ATOMIC_COMPLETION_EVIDENCE_MISMATCH" &&
               snapshot.value->completion_evidence ==
                   CompletionEvidence::ProviderAccepted,
           "schema-valid Provider ACK must not satisfy STATE_VERIFIED");
}

void testAtomicEnforcesPermissionAndLeaseClaims() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("atomic-authz");
    auto provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager manager(clock, ids, 1);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace", "system", TaskPriority::P1,
                          clock->monotonicNowNs() + 10'000'000'000LL};
    expect(manager
               .registerTools(defaultClimateMcpTools(),
                              defaultClimateRuntimePolicies(1), provider,
                              bootstrap)
               .ok,
           "authorization test catalog must register");
    const auto catalog = manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog.status.ok && catalog.value,
           "authorization test catalog must be queryable");

    auto denied = atomicRequest(
        *catalog.value, "permission-denied", "FRONT",
        clock->monotonicNowNs() + 5'000'000'000LL);
    denied.runtime.granted_permissions.clear();
    const auto denied_result =
        manager.callTool(denied, atomicCall(denied));
    expect(!denied_result.accepted &&
               denied_result.reject_code ==
                   "ATOMIC_PERMISSION_DENIED",
           "tool admission must fail closed without every required permission");

    auto untrusted_lease = atomicRequest(
        *catalog.value, "lease-invalid", "REAR",
        clock->monotonicNowNs() + 5'000'000'000LL);
    untrusted_lease.runtime.resource_lease_refs = {
        "climate/rear"};
    const auto lease_result =
        manager.callTool(untrusted_lease,
                         atomicCall(untrusted_lease));
    expect(!lease_result.accepted &&
               lease_result.reject_code ==
                   "ATOMIC_RESOURCE_LEASE_INVALID",
           "raw resource names must not be accepted as trusted lease IDs");
}

void testOrchestratorRejectsUnfrozenDagSchemaShapes() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "orchestrator-schema-contract");
    auto atomic =
        std::make_shared<AtomicServiceManager>(clock, ids, 1);
    auto dispatch =
        std::make_shared<dispatch::AgentDispatch>(
            clock, ids);
    orchestrator::Orchestrator orchestrator(
        clock, ids, atomic, dispatch);
    const auto deadline =
        clock->monotonicNowNs() + 5'000'000'000LL;

    orchestrator::IntentDAG dag;
    dag.dag_id = "schema-contract-dag";
    dag.request_id = "schema-contract-request";
    dag.priority = TaskPriority::P1;
    dag.deadline_mono_ns = deadline;
    dag.schema_version = 2;
    dag.idempotency_key = "schema-contract-idempotency";
    orchestrator::DAGNode first;
    first.node_id = "first";
    first.executor = "agent_dispatch";
    first.action = "contract.action.first";
    first.params = nlohmann::json::object();
    first.base_priority = TaskPriority::P1;
    first.deadline_mono_ns = deadline;
    orchestrator::DAGNode second;
    second.node_id = "second";
    second.executor = "agent_dispatch";
    second.action = "contract.action.second";
    second.params = nlohmann::json::object();
    second.base_priority = TaskPriority::P1;
    second.deadline_mono_ns = deadline;
    dag.nodes = {first, second};
    dag.edges = {{"edge-first-second", "first", "second",
                  "SUCCESS", true}};

    orchestrator::AdmissionContext admission;
    admission.principal_id_hash = "orchestrator-principal";
    admission.granted_priority = TaskPriority::P1;
    admission.policy_snapshot_id =
        "orchestrator-contract-policy";
    admission.policy_digest =
        secureDigest(admission.policy_snapshot_id);
    admission.authorization_ref =
        "orchestrator-contract-authorization";
    admission.allowed_capabilities = {
        first.action, second.action};
    admission.deadline_mono_ns = deadline;
    CallContext call{
        CallerModuleId::AgentService, dag.request_id,
        "trace-orchestrator-schema",
        admission.principal_id_hash,
        admission.granted_priority, deadline, {}, 0,
        admission.authorization_ref};
    expect(orchestrator.validateDAG(dag, admission, call).valid,
           "baseline frozen-schema DAG must validate");

    const auto expect_rejected =
        [&](const orchestrator::IntentDAG& candidate,
            const std::string& expected_code,
            const std::string& message) {
            const auto validation = orchestrator.validateDAG(
                candidate, admission, call);
            orchestrator::OrchestratorSubmitRequest submit;
            submit.dag = candidate;
            submit.admission = admission;
            submit.idempotency_key =
                candidate.idempotency_key;
            submit.expected_capability_digest =
                "validation-must-fail-before-catalog";
            submit.trace_id = call.trace_id;
            submit.submitted_at_utc_ms = clock->utcNowMs();
            const auto committed =
                orchestrator.submit(submit, call);
            expect(!validation.valid &&
                       validation.reject_code ==
                           expected_code &&
                       !committed.accepted &&
                       committed.reject_code ==
                           expected_code,
                   message + "; validation=" +
                       validation.reject_code + " submit=" +
                       committed.reject_code);
        };

    auto wrong_dag_schema = dag;
    wrong_dag_schema.schema_version = 20;
    expect_rejected(
        wrong_dag_schema, "ORCHESTRATOR_DAG_INVALID",
        "DAG schema_version other than 2 must be rejected");

    auto zero_input_schema = dag;
    zero_input_schema.nodes.front().input_schema_version = 0;
    expect_rejected(
        zero_input_schema, "ORCHESTRATOR_NODE_INVALID",
        "zero node input_schema_version must be rejected");

    auto zero_output_schema = dag;
    zero_output_schema.nodes.back()
        .expected_output_schema_version = 0;
    expect_rejected(
        zero_output_schema, "ORCHESTRATOR_NODE_INVALID",
        "zero node expected_output_schema_version must be rejected");

    auto optional_edge = dag;
    optional_edge.edges.front().required = false;
    expect(
        orchestrator.validateDAG(
            optional_edge, admission, call).valid,
        "required=false is a documented optional branch edge and must "
        "remain valid");
}

void testIntentRejectsSkillResultAfterOriginalDeadline() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "intent-late-skill-contract");
    auto skill = std::make_shared<ContractSkillEngine>(
        clock, ContractSkillEngine::Mode::ResolveAfterDeadline);
    auto prompt = std::make_shared<ContractPromptEngine>();
    auto inference =
        std::make_shared<DeadlineAdvancingInference>(
            clock,
            DeadlineAdvancingInference::AdvancePoint::Query);
    intent::IntentEngine engine(
        clock, ids, skill, prompt, inference);
    const auto input =
        intentContractInput(clock, "late-skill");

    const auto result =
        engine.process(input.request, input.context, input.call);
    expect(!result.status.ok &&
               result.status.error.code ==
                   "INTENT_RESULT_AFTER_DEADLINE" &&
               !result.value &&
               skill->resolveCalls() == 1U &&
               inference->submitCalls() == 0U,
           "late resolved Skill must not produce a DAG or reach inference");
}

void testIntentRejectsInferenceCompletionAfterOriginalDeadline() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "intent-late-inference-completion");
    auto skill = std::make_shared<ContractSkillEngine>(
        clock, ContractSkillEngine::Mode::NoMatch);
    auto prompt = std::make_shared<ContractPromptEngine>();
    auto inference =
        std::make_shared<DeadlineAdvancingInference>(
            clock,
            DeadlineAdvancingInference::AdvancePoint::Completion);
    intent::IntentEngine engine(
        clock, ids, skill, prompt, inference);
    const auto input =
        intentContractInput(clock, "late-completion");

    const auto result =
        engine.process(input.request, input.context, input.call);
    expect(!result.status.ok &&
               result.status.error.code ==
                   "INTENT_RESULT_AFTER_DEADLINE" &&
               !result.value &&
               inference->submitCalls() == 1U &&
               inference->queryCalls() == 0U,
           "late inference completion must not query or produce an outcome");
}

void testIntentRejectsInferenceQueryResultAfterOriginalDeadline() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "intent-late-inference-query");
    auto skill = std::make_shared<ContractSkillEngine>(
        clock, ContractSkillEngine::Mode::NoMatch);
    auto prompt = std::make_shared<ContractPromptEngine>();
    auto inference =
        std::make_shared<DeadlineAdvancingInference>(
            clock,
            DeadlineAdvancingInference::AdvancePoint::Query);
    intent::IntentEngine engine(
        clock, ids, skill, prompt, inference);
    const auto input =
        intentContractInput(clock, "late-query");

    const auto result =
        engine.process(input.request, input.context, input.call);
    expect(!result.status.ok &&
               result.status.error.code ==
                   "INTENT_RESULT_AFTER_DEADLINE" &&
               !result.value &&
               inference->submitCalls() == 1U &&
               inference->queryCalls() == 1U,
           "late completed snapshot must not produce DirectReply or DAG");
}

void testIntentFinalAskFailProtocolIsClosedAndBounded() {
    const auto evaluate =
        [](const std::string& raw_output,
           const std::string& suffix) {
            auto clock =
                std::make_shared<ManualRuntimeClock>();
            auto ids = std::make_shared<IdGenerator>(
                "intent-terminal-contract-" + suffix);
            auto skill =
                std::make_shared<ContractSkillEngine>(
                    clock,
                    ContractSkillEngine::Mode::NoMatch);
            auto prompt =
                std::make_shared<ContractPromptEngine>();
            auto inference =
                std::make_shared<DeadlineAdvancingInference>(
                    clock,
                    DeadlineAdvancingInference::
                        AdvancePoint::Never,
                    raw_output);
            intent::IntentEngine engine(
                clock, ids, skill, prompt, inference);
            const auto input =
                intentContractInput(clock, suffix);
            return engine.process(
                input.request, input.context, input.call);
        };

    const auto valid_ask = evaluate(
        R"json({"branch":"NO_QUERY","final":{"outcome":"ASK","slot":"person","reply":"Which person?"}})json",
        "valid-ask");
    expect(valid_ask.status.ok && valid_ask.value &&
               valid_ask.value->outcome_type ==
                   intent::IntentOutcomeType::Clarify &&
               !valid_ask.value->task_dag,
           "allowlisted ASK must become a non-plan clarification");

    const auto valid_fail = evaluate(
        R"json({"branch":"NO_QUERY","final":{"outcome":"FAIL","reason_code":"contradiction","reply":"The constraints conflict."}})json",
        "valid-fail");
    expect(valid_fail.status.ok && valid_fail.value &&
               valid_fail.value->outcome_type ==
                   intent::IntentOutcomeType::Failed &&
               valid_fail.value->reason_code ==
                   "INTENT_MODEL_CONTRADICTION" &&
               !valid_fail.value->task_dag,
           "allowlisted FAIL must become a bounded non-plan terminal");

    const std::vector<std::pair<std::string, std::string>>
        invalid_outputs{
            {"unknown-ask-slot",
             R"json({"branch":"NO_QUERY","final":{"outcome":"ASK","slot":"password","reply":"Provide it."}})json"},
            {"ask-extra-field",
             R"json({"branch":"NO_QUERY","final":{"outcome":"ASK","slot":"person","reply":"Which?","query":"Q1"}})json"},
            {"unknown-fail-reason",
             R"json({"branch":"NO_QUERY","final":{"outcome":"FAIL","reason_code":"model_guess","reply":"No."}})json"},
            {"fail-missing-reply",
             R"json({"branch":"NO_QUERY","final":{"outcome":"FAIL","reason_code":"unsupported"}})json"}};
    for (const auto& [suffix, raw_output] :
         invalid_outputs) {
        const auto rejected =
            evaluate(raw_output, suffix);
        expect(!rejected.status.ok &&
                   rejected.status.error.code ==
                       "INTENT_MODEL_PROTOCOL_INVALID" &&
                   !rejected.value,
               "unknown or structurally open ASK/FAIL must fail closed");
    }
}

}  // namespace

int main() {
    try {
        testMemoryRejectsExpiredCallsBeforeExternalIo();
        testPreprocessStateContracts();
        testKvContextInvalidateAndLeaseProtection();
        testKvInvalidatedLeasePinsCapacityPastAdvisoryExpiry();
        testAtomicPerToolConcurrencyLimit();
        testAtomicRejectsMalformedProviderOutput();
        testAtomicRequiresFrozenCompletionEvidence();
        testAtomicEnforcesPermissionAndLeaseClaims();
        testOrchestratorRejectsUnfrozenDagSchemaShapes();
        testIntentRejectsSkillResultAfterOriginalDeadline();
        testIntentRejectsInferenceCompletionAfterOriginalDeadline();
        testIntentRejectsInferenceQueryResultAfterOriginalDeadline();
        testIntentFinalAskFailProtocolIsClosedAndBounded();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "contract tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
