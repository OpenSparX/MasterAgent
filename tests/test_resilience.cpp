/**
 * @file test_resilience.cpp
 * @brief Verifies fault containment, replay safety, and exception recovery.
 */

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/kv_cache/kv_cache_manager.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/sub_agents/sub_agent.h"
#include "test_support.h"

using namespace master_agent;
using master_agent::test_support::expect;

namespace {

namespace atomic = master_agent::atomic_service;
namespace dispatch = master_agent::agent_dispatch;
namespace infer = master_agent::inference;
namespace kv = master_agent::kv_cache;
namespace orch = master_agent::orchestrator;
namespace sub = master_agent::sub_agents;

constexpr std::int64_t kTestBudgetNs = 60LL * 1000LL * 1000LL * 1000LL;

/**
 * Public-API coverage gaps deliberately left outside this test:
 * - ResultBinding/output projection is not represented by the current
 *   Orchestrator public types.
 * - AtomicService exposes no resource-fence history; stale execution-time
 *   fencing is therefore asserted through provider call count and snapshots.
 *
 * These are contract gaps, not test doubles invented by this test.
 */

std::int64_t deadline(const std::shared_ptr<ManualRuntimeClock>& clock) {
    return clock->monotonicNowNs() + kTestBudgetNs;
}

atomic::AtomicMcpCallEnvelope atomicEnvelope(
    const atomic::McpToolCatalogSnapshot& catalog,
    const std::string& execution_id, const std::string& operation_id,
    const std::string& idempotency_key, TaskPriority priority,
    std::uint64_t fencing_token, const std::string& location,
    const std::string& mode, std::int64_t deadline_mono_ns) {
    atomic::AtomicMcpCallEnvelope request;
    request.mcp_request.id = operation_id;
    request.mcp_request.name =
        "com_sgm_service_climate_setAutoFanSpeed";
    request.mcp_request.arguments =
        nlohmann::json{{"location", location}, {"mode", mode}};
    request.runtime.caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    request.runtime.request_id = "request-" + operation_id;
    request.runtime.trace_id = "trace-" + operation_id;
    request.runtime.plan_id = "plan-resilience";
    request.runtime.pid = "pid-" + operation_id;
    request.runtime.activation_id = "activation-" + operation_id;
    request.runtime.execution_id = execution_id;
    request.runtime.operation_id = operation_id;
    request.runtime.priority = priority;
    request.runtime.deadline_mono_ns = deadline_mono_ns;
    request.runtime.idempotency_key = idempotency_key;
    request.runtime.fencing_token = fencing_token;
    request.runtime.tool_catalog_snapshot_id = catalog.snapshot_id;
    request.runtime.tool_digest =
        catalog.tool_digests.at(request.mcp_request.name);
    request.runtime.policy_digest =
        catalog.policy_digests.at(request.mcp_request.name);
    request.runtime.granted_permissions = {
        "vehicle.climate.write"};
    request.runtime.principal_id_hash = "principal-resilience";
    request.runtime.authorization_ref =
        priority == TaskPriority::P0
            ? "trusted-safety:test"
            : "authorization:test";
    return request;
}

class RecordingAtomicProvider final : public atomic::IAtomicProvider {
public:
    explicit RecordingAtomicProvider(bool return_unknown = false,
                                     bool mismatched_reconcile = false)
        : return_unknown_(return_unknown),
          mismatched_reconcile_(mismatched_reconcile) {}

    atomic::ProviderInvocationResult call(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        ++call_count_;
        called_operations_.push_back(envelope.runtime.operation_id);
        if (return_unknown_) {
            return {atomic::ProviderInvocationState::Unknown, {},
                    SideEffectState::Unknown, "TEST_RESPONSE_LOST",
                    atomic::CompletionEvidence::None,
                    invocation_seal};
        }
        atomic::CallToolResult result;
        result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedLocation",
                            envelope.mcp_request.arguments.at("location")},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
        result.text_content.push_back(result.structured_content.dump());
        return {atomic::ProviderInvocationState::Succeeded, result,
                SideEffectState::Committed, {},
                atomic::CompletionEvidence::StateVerified,
                invocation_seal};
    }

    atomic::AtomicReconcileResult reconcile(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        atomic::AtomicReconcileResult result;
        result.invocation_seal = invocation_seal;
        result.operation_id = mismatched_reconcile_
                                  ? "foreign-operation"
                                  : envelope.runtime.operation_id;
        result.execution_id = mismatched_reconcile_
                                  ? "foreign-execution"
                                  : envelope.runtime.execution_id;
        result.tool_name =
            mismatched_reconcile_
                ? "com_sgm_service_climate_setAirCirculationMode"
                : envelope.mcp_request.name;
        result.fencing_token = mismatched_reconcile_
                                   ? envelope.runtime.fencing_token + 1
                                   : envelope.runtime.fencing_token;
        result.status = atomic::ReconcileStatus::ConfirmedSuccess;
        result.side_effect_state = SideEffectState::Committed;
        atomic::CallToolResult call_result;
        call_result.structured_content =
            nlohmann::json{{"success", true}, {"reconciled", true}};
        call_result.text_content.push_back(
            call_result.structured_content.dump());
        result.call_tool_result = std::move(call_result);
        result.completion_evidence =
            atomic::CompletionEvidence::StateVerified;
        return result;
    }

    std::size_t callCount() const {
        return call_count_;
    }

    const std::vector<std::string>& calledOperations() const {
        return called_operations_;
    }

private:
    bool return_unknown_;
    bool mismatched_reconcile_;
    std::size_t call_count_ = 0;
    std::vector<std::string> called_operations_;
};

struct ScriptedAtomicOutcome {
    bool succeeds = false;
    std::string error_code;
    SideEffectState side_effect_state =
        SideEffectState::ConfirmedNotExecuted;
    bool retryable_hint = false;
};

/// Records the exact runtime identity of every physical call and returns a
/// caller-supplied sequence of authoritative Provider outcomes.
class ScriptedRetryAtomicProvider final
    : public atomic::IAtomicProvider {
public:
    explicit ScriptedRetryAtomicProvider(
        std::vector<ScriptedAtomicOutcome> outcomes)
        : outcomes_(std::move(outcomes)) {}

    atomic::ProviderInvocationResult call(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        calls_.push_back(envelope);
        const auto outcome_index = calls_.size() - 1U;
        const auto outcome =
            outcome_index < outcomes_.size()
                ? outcomes_.at(outcome_index)
                : ScriptedAtomicOutcome{
                      true, {}, SideEffectState::Committed, false};

        atomic::ProviderInvocationResult result;
        result.invocation_seal = invocation_seal;
        result.retryable_hint = outcome.retryable_hint;
        if (!outcome.succeeds) {
            result.state = atomic::ProviderInvocationState::Failed;
            result.side_effect_state = outcome.side_effect_state;
            result.error_code = outcome.error_code;
            return result;
        }

        result.state = atomic::ProviderInvocationState::Succeeded;
        result.side_effect_state = SideEffectState::Committed;
        result.completion_evidence =
            atomic::CompletionEvidence::StateVerified;
        result.result.structured_content =
            nlohmann::json{
                {"success", true},
                {"appliedLocation",
                 envelope.mcp_request.arguments.at("location")},
                {"appliedMode",
                 envelope.mcp_request.arguments.at("mode")},
                {"errorCode", ""}};
        result.result.text_content.push_back(
            result.result.structured_content.dump());
        return result;
    }

    atomic::AtomicReconcileResult reconcile(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        atomic::AtomicReconcileResult result;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.status = atomic::ReconcileStatus::StillUnknown;
        result.fencing_token = envelope.runtime.fencing_token;
        result.side_effect_state = SideEffectState::Unknown;
        result.invocation_seal = invocation_seal;
        return result;
    }

    const std::vector<atomic::AtomicMcpCallEnvelope>& calls() const {
        return calls_;
    }

private:
    std::vector<ScriptedAtomicOutcome> outcomes_;
    std::vector<atomic::AtomicMcpCallEnvelope> calls_;
};

class DeadlineCrossingAtomicProvider final
    : public atomic::IAtomicProvider {
public:
    explicit DeadlineCrossingAtomicProvider(
        std::shared_ptr<ManualRuntimeClock> clock)
        : clock_(std::move(clock)) {}

    atomic::ProviderInvocationResult call(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        clock_->advanceMs(100);
        atomic::CallToolResult result;
        result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedLocation",
                            envelope.mcp_request.arguments.at("location")},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
        result.text_content.push_back(
            result.structured_content.dump());
        return {atomic::ProviderInvocationState::Succeeded, result,
                SideEffectState::Committed, {},
                atomic::CompletionEvidence::StateVerified,
                invocation_seal};
    }

    atomic::AtomicReconcileResult reconcile(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        atomic::AtomicReconcileResult result;
        result.invocation_seal = invocation_seal;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.fencing_token = envelope.runtime.fencing_token;
        result.status = atomic::ReconcileStatus::StillUnknown;
        return result;
    }

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
};

/// Returns a schema-valid result under a deliberately stale Provider
/// invocation identity. The manager must quarantine it as UNKNOWN.
class SealTamperingAtomicProvider final
    : public atomic::IAtomicProvider {
public:
    enum class Field {
        Invocation,
        ProviderEpoch,
        Attempt,
        Catalog,
        Fence,
        RequestDigest
    };

    explicit SealTamperingAtomicProvider(Field field) : field_(field) {}

    atomic::ProviderInvocationResult call(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        auto echoed = invocation_seal;
        switch (field_) {
            case Field::Invocation:
                echoed.invocation_id += "-stale";
                break;
            case Field::ProviderEpoch:
                ++echoed.provider_epoch;
                break;
            case Field::Attempt:
                ++echoed.attempt_no;
                break;
            case Field::Catalog:
                echoed.tool_catalog_snapshot_id += "-stale";
                break;
            case Field::Fence:
                ++echoed.fencing_token;
                break;
            case Field::RequestDigest:
                echoed.request_digest = secureDigest("foreign-request");
                break;
        }
        atomic::CallToolResult result;
        result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedLocation",
                            envelope.mcp_request.arguments.at("location")},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
        result.text_content.push_back(result.structured_content.dump());
        return {atomic::ProviderInvocationState::Succeeded,
                std::move(result), SideEffectState::Committed, {},
                atomic::CompletionEvidence::StateVerified,
                std::move(echoed)};
    }

    atomic::AtomicReconcileResult reconcile(
        const atomic::AtomicMcpCallEnvelope& envelope,
        const atomic::AtomicProviderInvocationSeal&
            invocation_seal) override {
        atomic::AtomicReconcileResult result;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.fencing_token = envelope.runtime.fencing_token;
        result.status = atomic::ReconcileStatus::StillUnknown;
        result.side_effect_state = SideEffectState::Unknown;
        result.invocation_seal = invocation_seal;
        return result;
    }

private:
    Field field_;
};

struct AtomicFixture {
    std::shared_ptr<ManualRuntimeClock> clock =
        std::make_shared<ManualRuntimeClock>();
    std::shared_ptr<IdGenerator> ids =
        std::make_shared<IdGenerator>("resilience-atomic");
    std::shared_ptr<RecordingAtomicProvider> provider;
    atomic::AtomicServiceManager manager;
    CallContext bootstrap;
    CallContext orchestrator_call;

    explicit AtomicFixture(bool return_unknown = false,
                           bool mismatched_reconcile = false,
                           std::uint32_t work_units = 1)
        : provider(std::make_shared<RecordingAtomicProvider>(
              return_unknown, mismatched_reconcile)),
          manager(clock, ids, 1),
          bootstrap{CallerModuleId::AgentService, "bootstrap",
                    "trace-bootstrap", "principal-resilience",
                    TaskPriority::P1, deadline(clock)},
          orchestrator_call{CallerModuleId::TaskOrchestrationEngine,
                            "request-resilience", "trace-resilience",
                            "principal-resilience", TaskPriority::P1,
                            deadline(clock)} {
        expect(manager
                   .registerTools(atomic::defaultClimateMcpTools(),
                                  atomic::defaultClimateRuntimePolicies(
                                      work_units),
                                  provider, bootstrap)
                   .ok,
               "atomic fixture registration must succeed");
    }

    atomic::McpToolCatalogSnapshot catalog() const {
        const auto result = manager.getToolCatalogSnapshot(bootstrap);
        expect(result.status.ok && result.value,
               "atomic catalog must be available");
        return *result.value;
    }

    CallContext callFor(
        const atomic::AtomicMcpCallEnvelope& request) const {
        return {request.runtime.caller_module_id,
                request.runtime.request_id,
                request.runtime.trace_id,
                request.runtime.principal_id_hash,
                request.runtime.priority,
                request.runtime.deadline_mono_ns, {}, 0,
                request.runtime.authorization_ref};
    }

    atomic::DispatchAcceptance call(
        const atomic::AtomicMcpCallEnvelope& request) {
        return manager.callTool(request, callFor(request));
    }
};

void testAtomicRejectsDuplicateExecutionIdentity() {
    AtomicFixture fixture;
    const auto catalog = fixture.catalog();
    const auto first = atomicEnvelope(
        catalog, "execution-shared", "operation-first", "idem-first",
        TaskPriority::P1, 1, "FRONT", "LOW", deadline(fixture.clock));
    expect(fixture.call(first).accepted,
           "first execution identity must be accepted");

    const auto duplicate_execution = atomicEnvelope(
        catalog, "execution-shared", "operation-second", "idem-second",
        TaskPriority::P1, 1, "REAR", "HIGH", deadline(fixture.clock));
    const auto result = fixture.call(duplicate_execution);
    expect(!result.accepted,
           "a new idempotency key must not overwrite an execution_id");
}

void testAtomicRejectsDuplicateOperationIdentity() {
    AtomicFixture fixture;
    const auto catalog = fixture.catalog();
    const auto first = atomicEnvelope(
        catalog, "execution-first", "operation-shared", "idem-first",
        TaskPriority::P1, 1, "FRONT", "LOW", deadline(fixture.clock));
    expect(fixture.call(first).accepted,
           "first operation identity must be accepted");

    const auto duplicate_operation = atomicEnvelope(
        catalog, "execution-second", "operation-shared", "idem-second",
        TaskPriority::P1, 1, "REAR", "HIGH", deadline(fixture.clock));
    const auto result = fixture.call(duplicate_operation);
    expect(!result.accepted,
           "a new idempotency key must not overwrite an operation_id");
}

void testAtomicIdempotencyBindsExecutionContext() {
    AtomicFixture fixture;
    const auto catalog = fixture.catalog();
    const auto first = atomicEnvelope(
        catalog, "execution-idem-first", "operation-idem-first",
        "idem-context", TaskPriority::P1, 3, "FRONT", "NORMAL",
        deadline(fixture.clock));
    const auto accepted = fixture.call(first);
    expect(accepted.accepted && !accepted.existing,
           "first atomic idempotency payload must be accepted");
    const auto replay = fixture.call(first);
    expect(replay.accepted && replay.existing &&
               replay.execution_id == first.runtime.execution_id,
           "exact atomic idempotency replay must return original identity");

    auto forged_context = first;
    forged_context.runtime.execution_id = "execution-idem-forged";
    forged_context.runtime.operation_id = "operation-idem-forged";
    forged_context.mcp_request.id = forged_context.runtime.operation_id;
    forged_context.runtime.fencing_token = 4;
    const auto conflict = fixture.call(forged_context);
    expect(!conflict.accepted &&
               conflict.reject_code == "ATOMIC_IDEMPOTENCY_CONFLICT",
           "atomic idempotency digest must bind execution, operation and "
           "fencing context");
}

void testAtomicRechecksFenceImmediatelyBeforeSideEffect() {
    AtomicFixture fixture;
    const auto catalog = fixture.catalog();
    const auto superseded = atomicEnvelope(
        catalog, "execution-fence-10", "operation-fence-10", "idem-fence-10",
        TaskPriority::P2, 10, "FRONT", "LOW", deadline(fixture.clock));
    const auto newest = atomicEnvelope(
        catalog, "execution-fence-11", "operation-fence-11", "idem-fence-11",
        TaskPriority::P0, 11, "FRONT", "HIGH", deadline(fixture.clock));
    expect(fixture.call(superseded).accepted,
           "initial fencing token must be accepted");
    expect(fixture.call(newest).accepted,
           "newer fencing token must be accepted");
    const auto stale = atomicEnvelope(
        catalog, "execution-fence-9", "operation-fence-9", "idem-fence-9",
        TaskPriority::P1, 9, "FRONT", "NORMAL", deadline(fixture.clock));
    const auto stale_result = fixture.call(stale);
    expect(!stale_result.accepted &&
               stale_result.reject_code == "ATOMIC_STALE_FENCING_TOKEN",
           "already-stale fencing token must be rejected at admission");
    expect(fixture.manager.runUntilIdle().ok,
           "atomic fence test must drain");

    const auto old_snapshot = fixture.manager.queryExecution(
        superseded.runtime.execution_id, fixture.callFor(superseded));
    const auto new_snapshot = fixture.manager.queryExecution(
        newest.runtime.execution_id, fixture.callFor(newest));
    expect(old_snapshot.status.ok && old_snapshot.value &&
               old_snapshot.value->state !=
                   atomic::AtomicExecutionState::Succeeded &&
               old_snapshot.value->side_effect_state ==
                   SideEffectState::NotStarted,
           "superseded fencing token must terminalize without side effect");
    expect(new_snapshot.status.ok && new_snapshot.value &&
               new_snapshot.value->state ==
                   atomic::AtomicExecutionState::Succeeded,
           "highest fencing token must be the execution that succeeds");
    expect(fixture.provider->callCount() == 1 &&
               fixture.provider->calledOperations().front() ==
                   newest.runtime.operation_id,
           "provider must be invoked exactly once for the highest fence");
}

void testAtomicQuarantinesProviderResultAfterDeadline() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("atomic-deadline");
    auto provider =
        std::make_shared<DeadlineCrossingAtomicProvider>(clock);
    atomic::AtomicServiceManager manager(clock, ids, 1);
    CallContext bootstrap{
        CallerModuleId::AgentService, "bootstrap-deadline",
        "trace-deadline", "principal-resilience", TaskPriority::P1,
        deadline(clock)};
    expect(manager
               .registerTools(
                   atomic::defaultClimateMcpTools(),
                   atomic::defaultClimateRuntimePolicies(1),
                   provider, bootstrap)
               .ok,
           "deadline test catalog must register");
    const auto catalog_result =
        manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog_result.value.has_value(),
           "deadline test catalog must be available");
    const auto request_deadline =
        clock->monotonicNowNs() + 10'000'000LL;
    const auto request = atomicEnvelope(
        *catalog_result.value, "execution-deadline",
        "operation-deadline", "idem-deadline", TaskPriority::P1,
        1, "FRONT", "HIGH", request_deadline);
    CallContext call{
        request.runtime.caller_module_id,
        request.runtime.request_id, request.runtime.trace_id,
        request.runtime.principal_id_hash, request.runtime.priority,
        request.runtime.deadline_mono_ns, {}, 0,
        request.runtime.authorization_ref};
    expect(manager.callTool(request, call).accepted,
           "deadline crossing request must be admitted before expiry");
    expect(manager.runUntilIdle().ok,
           "late provider result must settle into an isolated state");
    auto query_call = call;
    query_call.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000'000LL;
    const auto snapshot = manager.queryExecution(
        request.runtime.execution_id, query_call);
    expect(snapshot.value &&
               snapshot.value->state ==
                   atomic::AtomicExecutionState::Unknown &&
               snapshot.value->side_effect_state ==
                   SideEffectState::Unknown &&
               snapshot.value->error_code ==
                   "ATOMIC_PROVIDER_RESULT_AFTER_DEADLINE",
           "provider success after deadline must require reconciliation");
}

void testAtomicP0PreemptsAndP2ResumesAtSafePoint() {
    AtomicFixture fixture(false, false, 5);
    const auto catalog = fixture.catalog();
    const auto low = atomicEnvelope(
        catalog, "execution-low", "operation-low", "idem-low",
        TaskPriority::P2, 1, "FRONT", "LOW", deadline(fixture.clock));
    expect(fixture.call(low).accepted,
           "P2 atomic execution must be accepted");
    expect(fixture.manager.pumpOne(), "P2 atomic execution must start");

    const auto high = atomicEnvelope(
        catalog, "execution-p0", "operation-p0", "idem-p0",
        TaskPriority::P0, 1, "REAR", "HIGH", deadline(fixture.clock));
    expect(fixture.call(high).accepted,
           "P0 atomic execution must be accepted");
    expect(fixture.manager.runUntilIdle().ok,
           "preemption and restoration must settle");

    const auto events = fixture.manager.events();
    const auto event_index = [&events](const std::string& execution,
                                       const std::string& type) {
        const auto found = std::find_if(
            events.begin(), events.end(), [&](const auto& event) {
                return event.execution_id == execution &&
                       event.event_type == type;
            });
        return found == events.end()
                   ? events.size()
                   : static_cast<std::size_t>(
                         std::distance(events.begin(), found));
    };
    expect(event_index("execution-low", "SUSPENDED") <
               event_index("execution-p0", "SUCCEEDED") &&
               event_index("execution-p0", "SUCCEEDED") <
                   event_index("execution-low", "SUCCEEDED"),
           "P2 must suspend, P0 finish, then P2 resume and finish");
    expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
               return event.execution_id == "execution-low" &&
                      event.event_type == "SUSPENDED" && event.safe_point &&
                      event.resource_released;
           }),
           "atomic suspension must be recorded at a released safe point");
}

void testAtomicRejectsMismatchedUnknownReconciliationIdentity() {
    AtomicFixture fixture(true, true);
    const auto catalog = fixture.catalog();
    const auto request = atomicEnvelope(
        catalog, "execution-unknown", "operation-unknown", "idem-unknown",
        TaskPriority::P1, 7, "FRONT", "NORMAL", deadline(fixture.clock));
    expect(fixture.call(request).accepted,
           "UNKNOWN test call must be accepted");
    expect(fixture.manager.runUntilIdle().ok,
           "UNKNOWN test execution must stop consuming worker capacity");
    const auto request_call = fixture.callFor(request);
    const auto before = fixture.manager.queryExecution(
        request.runtime.execution_id, request_call);
    expect(before.value &&
               before.value->state ==
                   atomic::AtomicExecutionState::Unknown,
           "provider response loss must remain UNKNOWN");

    const auto reconciled = fixture.manager.reconcileExecution(
        request.runtime.operation_id, request_call);
    expect(!reconciled.status.ok,
           "mismatched operation/execution/tool/fence reconciliation "
           "identity must be rejected");
    const auto after = fixture.manager.queryExecution(
        request.runtime.execution_id, request_call);
    expect(after.value &&
               after.value->state ==
                   atomic::AtomicExecutionState::Unknown &&
               after.value->side_effect_state == SideEffectState::Unknown,
           "identity mismatch must not settle or mutate UNKNOWN");
}

void testAtomicRejectsStaleProviderInvocationSeals() {
    const std::vector<SealTamperingAtomicProvider::Field> fields{
        SealTamperingAtomicProvider::Field::Invocation,
        SealTamperingAtomicProvider::Field::ProviderEpoch,
        SealTamperingAtomicProvider::Field::Attempt,
        SealTamperingAtomicProvider::Field::Catalog,
        SealTamperingAtomicProvider::Field::Fence,
        SealTamperingAtomicProvider::Field::RequestDigest};
    std::size_t suffix = 0;
    for (const auto field : fields) {
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids = std::make_shared<IdGenerator>(
            "atomic-seal-" + std::to_string(++suffix));
        auto provider =
            std::make_shared<SealTamperingAtomicProvider>(field);
        atomic::AtomicServiceManager manager(clock, ids, 1);
        CallContext bootstrap{
            CallerModuleId::AgentService,
            "bootstrap-seal-" + std::to_string(suffix),
            "trace-bootstrap-seal-" + std::to_string(suffix),
            "principal-resilience", TaskPriority::P1,
            deadline(clock)};
        expect(manager
                   .registerTools(
                       atomic::defaultClimateMcpTools(),
                       atomic::defaultClimateRuntimePolicies(1),
                       provider, bootstrap)
                   .ok,
               "seal test catalog must register");
        const auto catalog =
            manager.getToolCatalogSnapshot(bootstrap);
        expect(catalog.status.ok && catalog.value,
               "seal test catalog must be readable");
        const auto tag = std::to_string(suffix);
        const auto request = atomicEnvelope(
            *catalog.value, "execution-seal-" + tag,
            "operation-seal-" + tag, "idem-seal-" + tag,
            TaskPriority::P1, 1, "FRONT", "HIGH",
            deadline(clock));
        CallContext call{
            request.runtime.caller_module_id,
            request.runtime.request_id,
            request.runtime.trace_id,
            request.runtime.principal_id_hash,
            request.runtime.priority,
            request.runtime.deadline_mono_ns,
            {},
            0,
            request.runtime.authorization_ref};
        expect(manager.callTool(request, call).accepted &&
                   manager.runUntilIdle().ok,
               "tampered Provider callback must be isolated");
        const auto snapshot = manager.queryExecution(
            request.runtime.execution_id, call);
        expect(snapshot.value &&
                   snapshot.value->state ==
                       atomic::AtomicExecutionState::Unknown &&
                   snapshot.value->side_effect_state ==
                       SideEffectState::Unknown &&
                   snapshot.value->error_code ==
                       "ATOMIC_PROVIDER_INVOCATION_SEAL_MISMATCH" &&
                   snapshot.value->provider_invocation,
               "every stale Provider invocation field must be rejected");
        const auto events = manager.events();
        expect(std::any_of(
                   events.begin(), events.end(),
                   [&request](const auto& event) {
                       return event.execution_id ==
                                  request.runtime.execution_id &&
                              event.event_type == "UNKNOWN" &&
                              !event.resource_released;
                   }),
               "tampered Provider response must retain its physical lease");
    }
}

infer::InferenceRequest inferenceRequest(
    const std::shared_ptr<ManualRuntimeClock>& clock, const std::string& id,
    TaskPriority priority, bool p0_authorized,
    const std::string& prompt = "shared deterministic prompt") {
    infer::InferenceRequest request;
    request.job_id = id;
    request.request_id = "request-" + id;
    request.parent_operation_id = "parent-" + id;
    request.session_id = "session-resilience";
    request.prompt = prompt;
    request.prompt_digest = secureDigest(prompt);
    request.prompt_segments = {
        {"user", request.prompt_digest, 16}};
    request.priority = priority;
    request.deadline_mono_ns = deadline(clock);
    request.idempotency_key = "idem-" + id;
    request.trace_id = "trace-" + id;
    request.admission.principal_id = "principal-resilience";
    request.admission.caller_module_id =
        CallerModuleId::IntentRecognitionEngine;
    request.admission.source_request_id = request.request_id;
    request.admission.granted_priority = priority;
    request.admission.p0_authorization = p0_authorized;
    request.admission.policy_snapshot_id = "policy-resilience";
    request.admission.deadline_mono_ns = request.deadline_mono_ns;
    if (p0_authorized) {
        request.admission.signature_ref =
            "trusted-safety:inference-test";
    }
    return request;
}

/// Builds a fresh read-only context while retaining the job owner's
/// request/trace/principal seal.
CallContext inferenceQueryCall(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const infer::InferenceRequest& request) {
    return {CallerModuleId::AgentService,
            request.request_id,
            request.trace_id,
            request.admission.principal_id,
            request.priority,
            clock->monotonicNowNs() + 1'000'000'000LL,
            {},
            0,
            request.admission.signature_ref};
}

struct InferenceFixture {
    std::shared_ptr<ManualRuntimeClock> clock =
        std::make_shared<ManualRuntimeClock>();
    std::shared_ptr<IdGenerator> ids =
        std::make_shared<IdGenerator>("resilience-inference");
    std::shared_ptr<kv::KvCacheManager> cache =
        std::make_shared<kv::KvCacheManager>(clock, ids);
    std::shared_ptr<infer::MockModelRuntime> model =
        std::make_shared<infer::MockModelRuntime>(1);
    infer::InferenceFramework framework{clock, ids, model, cache, 1};
    CallContext intent_call{CallerModuleId::IntentRecognitionEngine,
                            "request-inference", "trace-inference",
                            "principal-resilience", TaskPriority::P1,
                            deadline(clock)};
    CallContext control_call{CallerModuleId::AgentService,
                             "request-control", "trace-control",
                             "principal-resilience", TaskPriority::P0,
                             deadline(clock), {}, 0,
                             "trusted-safety:control-test"};
    CallContext kv_query{CallerModuleId::InferenceFramework,
                         "request-kv", "trace-kv",
                         "principal-resilience", TaskPriority::P1,
                         deadline(clock)};

    void warmCache() {
        const auto warm = inferenceRequest(
            clock, "job-warm", TaskPriority::P1, false);
        intent_call.request_id = warm.request_id;
        intent_call.trace_id = warm.trace_id;
        expect(framework.submitInference(warm, intent_call).accepted,
               "warm-up inference must be accepted");
        expect(framework.runUntilIdle().ok,
               "warm-up inference must populate deterministic KV cache");
        expect(cache->queryStatus(kv_query).ready_entries == 1,
               "warm-up must publish one reusable KV entry");
    }
};

void testInferenceCancelAfterImmediateKvImportStaysLeaseFree() {
    InferenceFixture fixture;
    fixture.warmCache();
    fixture.model->setWorkUnits(6);
    const auto target = inferenceRequest(
        fixture.clock, "job-cancel", TaskPriority::P1, false);
    fixture.intent_call.request_id = target.request_id;
    fixture.intent_call.trace_id = target.trace_id;
    expect(fixture.framework.submitInference(target, fixture.intent_call)
               .accepted,
           "cancellable inference must be accepted");
    expect(fixture.framework.pumpOne(),
           "cancellable inference must start and import KV");
    expect(fixture.cache->queryStatus(fixture.kv_query).active_leases == 0,
           "simulated KV import must immediately complete the hit lease");
    fixture.control_call.request_id = target.request_id;
    fixture.control_call.trace_id = target.trace_id;
    expect(fixture.framework
               .cancelInference(target.job_id, 1, fixture.control_call)
               .ok,
           "running inference cancellation must succeed");
    expect(fixture.cache->queryStatus(fixture.kv_query).active_leases == 0,
           "cancellation must not reacquire or leak the completed KV lease");
    const auto snapshot = fixture.framework.queryInference(
        target.job_id, inferenceQueryCall(fixture.clock, target));
    expect(snapshot.value &&
               snapshot.value->state == infer::InferenceJobState::Cancelled,
           "cancelled inference must be terminal");
}

void testInferencePreemptionDoesNotLeakOrDuplicateKvLease() {
    InferenceFixture fixture;
    fixture.warmCache();
    fixture.model->setWorkUnits(6);

    const auto low = inferenceRequest(
        fixture.clock, "job-low", TaskPriority::P2, false);
    fixture.intent_call.priority = TaskPriority::P2;
    fixture.intent_call.request_id = low.request_id;
    fixture.intent_call.trace_id = low.trace_id;
    expect(fixture.framework.submitInference(low, fixture.intent_call)
               .accepted,
           "P2 inference must be accepted");
    expect(fixture.framework.pumpOne(),
           "P2 inference must start with a cache hit");
    expect(fixture.cache->queryStatus(fixture.kv_query).active_leases == 0,
           "P2 KV hit must be released immediately after simulated import");

    const auto high = inferenceRequest(
        fixture.clock, "job-p0", TaskPriority::P0, true);
    fixture.intent_call.priority = TaskPriority::P0;
    fixture.intent_call.request_id = high.request_id;
    fixture.intent_call.trace_id = high.trace_id;
    fixture.intent_call.authorization_ref =
        high.admission.signature_ref;
    expect(fixture.framework.submitInference(high, fixture.intent_call)
               .accepted,
           "authorized P0 inference must be accepted");
    expect(fixture.framework.runUntilIdle().ok,
           "P0 preemption and P2 restoration must drain");

    const auto low_state = fixture.framework.queryInference(
        low.job_id, inferenceQueryCall(fixture.clock, low));
    const auto high_state = fixture.framework.queryInference(
        high.job_id, inferenceQueryCall(fixture.clock, high));
    expect(low_state.value && high_state.value &&
               low_state.value->state ==
                   infer::InferenceJobState::Completed &&
               high_state.value->state ==
                   infer::InferenceJobState::Completed,
           "both preemptor and restored inference must complete");
    expect(fixture.cache->queryStatus(fixture.kv_query).active_leases == 0,
           "preemption/restore must leave no orphan or duplicate KV lease");
    const auto events = fixture.framework.events();
    expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
               return event.job_id == "job-low" &&
                      event.event_type == "SUSPENDED";
           }),
           "P2 inference must expose a suspension checkpoint");
}

void testInferenceBindsPromptToDigestAndIdempotency() {
    InferenceFixture fixture;
    const auto original = inferenceRequest(
        fixture.clock, "job-original", TaskPriority::P1, false,
        "trusted prompt");
    fixture.intent_call.request_id = original.request_id;
    fixture.intent_call.trace_id = original.trace_id;
    expect(fixture.framework.submitInference(original, fixture.intent_call)
               .accepted,
           "original inference must be accepted");

    auto transport_retry = original;
    transport_retry.job_id = "job-transport-retry";
    const auto replay = fixture.framework.submitInference(
        transport_retry, fixture.intent_call);
    expect(replay.accepted && replay.existing &&
               replay.job_id == original.job_id,
           "new transport job_id with the same logical request must replay "
           "the existing inference job");

    auto forged = original;
    forged.job_id = "job-forged";
    forged.prompt = "different prompt with forged old digest";
    const auto result =
        fixture.framework.submitInference(forged, fixture.intent_call);
    expect(!result.accepted,
           "prompt bytes must be bound to prompt_digest/idempotency digest");
}

sub::AgentManifest manifest(const std::string& agent_id,
                            std::uint64_t epoch) {
    sub::AgentManifest value;
    value.agent_id = agent_id;
    value.agent_epoch = epoch;
    value.manifest_digest =
        secureDigest(agent_id + "|" + std::to_string(epoch) + "|plan_trip");
    value.capability_version = "contract-v2";
    value.capabilities = {"plan_trip"};
    value.max_concurrency = 1;
    value.supports_safe_point_preemption = true;
    return value;
}

/**
 * Fault-injection wrapper for the external SubAgent contract.
 * It keeps normal behavior in the deterministic provider and injects only one
 * pump exception at the provider/process boundary.
 *
 */
/// Counts all Provider lifecycle/control boundaries so scheduler capacity and
/// preemption decisions can be checked without inspecting private ledgers.
class ControlCountingSubAgent final : public sub::ISubAgent {
public:
    ControlCountingSubAgent(
        sub::AgentManifest manifest,
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::uint32_t work_units)
        : delegate_(
              std::make_shared<sub::DeterministicSubAgent>(
                  std::move(manifest), std::move(clock),
                  std::move(ids), work_units)) {}

    sub::AgentManifest getManifest() const override {
        return delegate_->getManifest();
    }

    sub::SubAgentAcceptance submit(
        const sub::SubAgentExecutionRequest& request,
        const CallContext& call) override {
        submit_requests_.push_back(request);
        return delegate_->submit(request, call);
    }

    Result<sub::SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const override {
        return delegate_->query(dispatch_id, call);
    }

    Status requestPreempt(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        ++preempt_count_;
        return delegate_->requestPreempt(
            dispatch_id, control_epoch, call);
    }

    Status restore(
        const std::string& dispatch_id,
        const std::string& checkpoint_ref,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        ++restore_count_;
        return delegate_->restore(
            dispatch_id, checkpoint_ref, control_epoch, call);
    }

    Status cancel(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        ++cancel_count_;
        return delegate_->cancel(
            dispatch_id, control_epoch, call);
    }

    bool pumpOne() override {
        return delegate_->pumpOne();
    }

    bool heartbeat(const CallContext& call) const override {
        return delegate_->heartbeat(call);
    }

    const std::vector<sub::SubAgentExecutionRequest>&
    submitRequests() const {
        return submit_requests_;
    }

    std::size_t preemptCount() const {
        return preempt_count_;
    }

    std::size_t restoreCount() const {
        return restore_count_;
    }

    std::size_t cancelCount() const {
        return cancel_count_;
    }

private:
    std::shared_ptr<sub::DeterministicSubAgent> delegate_;
    std::vector<sub::SubAgentExecutionRequest> submit_requests_;
    std::size_t preempt_count_ = 0;
    std::size_t restore_count_ = 0;
    std::size_t cancel_count_ = 0;
};

class ThrowingPumpSubAgent final : public sub::ISubAgent {
public:
    ThrowingPumpSubAgent(sub::AgentManifest manifest,
                         std::shared_ptr<IRuntimeClock> clock,
                         std::shared_ptr<IdGenerator> ids,
                         std::uint32_t work_units)
        : delegate_(std::make_shared<sub::DeterministicSubAgent>(
              std::move(manifest), std::move(clock), std::move(ids),
              work_units)) {}

    sub::AgentManifest getManifest() const override {
        return delegate_->getManifest();
    }

    sub::SubAgentAcceptance submit(
        const sub::SubAgentExecutionRequest& request,
        const CallContext& call) override {
        return delegate_->submit(request, call);
    }

    Result<sub::SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const override {
        return delegate_->query(dispatch_id, call);
    }

    Status requestPreempt(const std::string& dispatch_id,
                          std::uint64_t control_epoch,
                          const CallContext& call) override {
        return delegate_->requestPreempt(dispatch_id, control_epoch, call);
    }

    Status restore(const std::string& dispatch_id,
                   const std::string& checkpoint_ref,
                   std::uint64_t control_epoch,
                   const CallContext& call) override {
        return delegate_->restore(
            dispatch_id, checkpoint_ref, control_epoch, call);
    }

    Status cancel(const std::string& dispatch_id,
                  std::uint64_t control_epoch,
                  const CallContext& call) override {
        return delegate_->cancel(dispatch_id, control_epoch, call);
    }

    bool pumpOne() override {
        if (throw_next_pump_) {
            throw_next_pump_ = false;
            throw std::runtime_error("injected SubAgent pump failure");
        }
        return delegate_->pumpOne();
    }

    bool heartbeat(const CallContext& call) const override {
        return delegate_->heartbeat(call);
    }

    void throwNextPump() {
        throw_next_pump_ = true;
    }

private:
    std::shared_ptr<sub::DeterministicSubAgent> delegate_;
    bool throw_next_pump_ = false;
};

class DeadlineCrossingSubAgent final
    : public sub::ISubAgent {
public:
    DeadlineCrossingSubAgent(
        sub::AgentManifest manifest,
        std::shared_ptr<ManualRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids)
        : clock_(clock),
          delegate_(
              std::make_shared<sub::DeterministicSubAgent>(
                  std::move(manifest), std::move(clock),
                  std::move(ids), 1)) {}

    sub::AgentManifest getManifest() const override {
        return delegate_->getManifest();
    }
    sub::SubAgentAcceptance submit(
        const sub::SubAgentExecutionRequest& request,
        const CallContext& call) override {
        return delegate_->submit(request, call);
    }
    Result<sub::SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const override {
        return delegate_->query(dispatch_id, call);
    }
    Status requestPreempt(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->requestPreempt(
            dispatch_id, control_epoch, call);
    }
    Status restore(
        const std::string& dispatch_id,
        const std::string& checkpoint_ref,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->restore(
            dispatch_id, checkpoint_ref, control_epoch, call);
    }
    Status cancel(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->cancel(
            dispatch_id, control_epoch, call);
    }
    bool pumpOne() override {
        const bool progressed = delegate_->pumpOne();
        if (progressed) clock_->advanceMs(100);
        return progressed;
    }
    bool heartbeat(const CallContext& call) const override {
        return delegate_->heartbeat(call);
    }

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
    std::shared_ptr<sub::DeterministicSubAgent> delegate_;
};

/// Mutates one echoed provider identity after the deterministic Agent has
/// accepted the real request. This proves Dispatch validates the complete
/// cross-process seal rather than trusting a provider's terminal state.
class SealTamperingSubAgent final : public sub::ISubAgent {
public:
    enum class Field {
        AcceptanceDispatch,
        Attempt,
        AgentEpoch,
        Lease,
        Fence,
        OutputSchema
    };

    SealTamperingSubAgent(
        sub::AgentManifest manifest,
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        Field field)
        : delegate_(
              std::make_shared<sub::DeterministicSubAgent>(
                  std::move(manifest), std::move(clock),
                  std::move(ids), 1)),
          field_(field) {}

    sub::AgentManifest getManifest() const override {
        return delegate_->getManifest();
    }

    sub::SubAgentAcceptance submit(
        const sub::SubAgentExecutionRequest& request,
        const CallContext& call) override {
        auto accepted = delegate_->submit(request, call);
        if (accepted.accepted &&
            field_ == Field::AcceptanceDispatch) {
            accepted.dispatch_id += "-stale";
        }
        return accepted;
    }

    Result<sub::SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const override {
        auto observed = delegate_->query(dispatch_id, call);
        if (!observed.status.ok || !observed.value) return observed;
        switch (field_) {
            case Field::AcceptanceDispatch:
                break;
            case Field::Attempt:
                ++observed.value->request.attempt_no;
                break;
            case Field::AgentEpoch:
                ++observed.value->request.agent_epoch;
                break;
            case Field::Lease:
                observed.value->request.lease_id += "-stale";
                break;
            case Field::Fence:
                ++observed.value->request.fencing_token;
                break;
            case Field::OutputSchema:
                ++observed.value->output_schema_version;
                break;
        }
        return observed;
    }

    Status requestPreempt(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->requestPreempt(
            dispatch_id, control_epoch, call);
    }

    Status restore(
        const std::string& dispatch_id,
        const std::string& checkpoint_ref,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->restore(
            dispatch_id, checkpoint_ref, control_epoch, call);
    }

    Status cancel(
        const std::string& dispatch_id,
        std::uint64_t control_epoch,
        const CallContext& call) override {
        return delegate_->cancel(
            dispatch_id, control_epoch, call);
    }

    bool pumpOne() override {
        return delegate_->pumpOne();
    }

    bool heartbeat(const CallContext& call) const override {
        return delegate_->heartbeat(call);
    }

private:
    std::shared_ptr<sub::DeterministicSubAgent> delegate_;
    Field field_;
};

dispatch::DispatchTask dispatchTask(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const sub::AgentManifest& target_manifest, const std::string& suffix,
    TaskPriority priority = TaskPriority::P1) {
    dispatch::DispatchTask task;
    task.request_id = "request-" + suffix;
    task.plan_id = "plan-" + suffix;
    task.pid = "pid-" + suffix;
    task.activation_id = "activation-" + suffix;
    task.execution_id = "execution-" + suffix;
    task.operation_id = "operation-" + suffix;
    task.task_id = "task-" + suffix;
    task.action = "plan_trip";
    task.target_agent = target_manifest.agent_id;
    task.params = nlohmann::json{{"destination", suffix}};
    task.priority = priority;
    task.deadline_mono_ns = deadline(clock);
    task.idempotency_key = "idem-" + suffix;
    task.fencing_token = 1;
    task.capability_digest = dispatch::dispatchCapabilityDigest(
        task.action, task.input_schema_version,
        task.expected_output_schema_version);
    // Capacity generation is a live AgentLease/topology snapshot and must be
    // sealed immediately before the first submit, never inferred from an
    // Agent provider epoch or an old fixed generation.
    task.capacity_epoch = 0;
    task.principal_id_hash = "principal-resilience";
    task.authorization_ref =
        priority == TaskPriority::P0
            ? "trusted-safety:test"
            : "authorization:test";
    task.trace_id = "trace-" + suffix;
    return task;
}

CallContext dispatchCall(const dispatch::DispatchTask& task) {
    return {CallerModuleId::TaskOrchestrationEngine,
            task.request_id,
            task.trace_id,
            task.principal_id_hash,
            task.priority,
            task.deadline_mono_ns, {}, 0,
            task.authorization_ref};
}

/// Binds a new logical dispatch to the scheduler's current capacity
/// generation. Exact idempotent replays retain the originally sealed task.
dispatch::AgentDispatchCapacity bindCurrentDispatchCapacity(
    dispatch::AgentDispatch& scheduler,
    dispatch::DispatchTask& task) {
    const auto capacity =
        scheduler.getCapacity(dispatchCall(task));
    expect(capacity.health_state != "CALLER_NOT_ALLOWED" &&
               capacity.capacity_epoch != 0,
           "dispatch capacity must be read through an authorized live "
           "scheduler snapshot");
    task.capacity_epoch = capacity.capacity_epoch;
    return capacity;
}

/// Query control has its own live deadline but remains sealed to the
/// dispatch owner's request/trace/principal identity.
CallContext dispatchQueryCall(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const dispatch::DispatchTask& task) {
    auto call = dispatchCall(task);
    call.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000'000LL;
    return call;
}

void testDispatchRejectsAgentEpochReplacementWhileLeased() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-epoch");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto epoch1 = manifest("trip-agent", 1);
    auto original = std::make_shared<sub::DeterministicSubAgent>(
        epoch1, clock, ids, 8);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(original, bootstrap).ok,
           "epoch 1 agent must register");
    auto same_epoch = std::make_shared<sub::DeterministicSubAgent>(
        epoch1, clock, ids, 1);
    expect(!scheduler.registerAgent(same_epoch, bootstrap).ok,
           "same agent epoch registration must be rejected as stale");
    auto task =
        dispatchTask(clock, epoch1, "active-epoch", TaskPriority::P2);
    bindCurrentDispatchCapacity(scheduler, task);
    const auto accepted = scheduler.submitDispatch(task, dispatchCall(task));
    expect(accepted.accepted, "dispatch must be accepted on epoch 1");
    expect(scheduler.pumpOne(), "dispatch must acquire epoch 1 agent lease");
    const auto running = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(running.value &&
               running.value->state == dispatch::DispatchState::Running,
           "dispatch must be running before replacement is attempted");

    const auto epoch2 = manifest("trip-agent", 2);
    auto replacement = std::make_shared<sub::DeterministicSubAgent>(
        epoch2, clock, ids, 1);
    const auto replaced = scheduler.registerAgent(replacement, bootstrap);
    expect(!replaced.ok,
           "higher agent epoch must not replace a provider with an active "
           "dispatch lease");
    expect(scheduler.runUntilIdle().ok,
           "original provider must remain bound until dispatch terminal");
    const auto terminal = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(terminal.value &&
               terminal.value->state == dispatch::DispatchState::Succeeded &&
               terminal.value->route.agent_epoch == 1,
           "in-flight dispatch must finish on its leased provider epoch");
    expect(scheduler.registerAgent(replacement, bootstrap).ok,
           "higher epoch replacement may register after the old lease drains");
}

void testDispatchQuarantinesProviderResultAfterDeadline() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("dispatch-deadline");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target =
        manifest("deadline-trip-agent", 1);
    auto agent = std::make_shared<DeadlineCrossingSubAgent>(
        target, clock, ids);
    CallContext bootstrap{
        CallerModuleId::AgentService, "bootstrap-deadline",
        "trace-deadline", "principal-resilience", TaskPriority::P1,
        deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "deadline crossing SubAgent must register");
    auto task = dispatchTask(
        clock, target, "deadline", TaskPriority::P1);
    task.deadline_mono_ns =
        clock->monotonicNowNs() + 10'000'000LL;
    bindCurrentDispatchCapacity(scheduler, task);
    const auto accepted =
        scheduler.submitDispatch(task, dispatchCall(task));
    expect(accepted.accepted,
           "dispatch must be admitted before its deadline");
    expect(scheduler.pumpOne(),
           "first dispatch pump must submit to the provider");
    expect(scheduler.pumpOne(),
           "second dispatch pump must observe the late terminal");
    const auto snapshot = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(snapshot.value &&
               snapshot.value->state ==
                   dispatch::DispatchState::Unknown &&
               snapshot.value->side_effect_state ==
                   SideEffectState::Unknown &&
               snapshot.value->error_code ==
                   "DISPATCH_RESULT_AFTER_DEADLINE" &&
               !snapshot.value->route.lease_id.empty(),
           "late SubAgent success must be UNKNOWN and retain its lease");
}

void testDispatchIdempotencyConflicts() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-idem");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("trip-agent", 3);
    auto agent = std::make_shared<sub::DeterministicSubAgent>(
        target, clock, ids, 2);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "dispatch fixture agent must register");

    auto task = dispatchTask(clock, target, "idem");
    bindCurrentDispatchCapacity(scheduler, task);
    const auto first = scheduler.submitDispatch(task, dispatchCall(task));
    expect(first.accepted && !first.existing,
           "first dispatch must be newly accepted");
    const auto replay = scheduler.submitDispatch(task, dispatchCall(task));
    expect(replay.accepted && replay.existing &&
               replay.dispatch_id == first.dispatch_id,
           "same idempotency key and digest must replay");

    auto changed = task;
    changed.params["destination"] = "changed";
    const auto conflict =
        scheduler.submitDispatch(changed, dispatchCall(changed));
    expect(!conflict.accepted &&
               conflict.reject_code == "DISPATCH_IDEMPOTENCY_CONFLICT",
           "same idempotency key with changed payload must conflict");

    auto changed_authority = task;
    changed_authority.authorization_ref = "authorization:attacker";
    const auto authority_conflict = scheduler.submitDispatch(
        changed_authority, dispatchCall(changed_authority));
    expect(!authority_conflict.accepted &&
               authority_conflict.reject_code ==
                   "DISPATCH_IDEMPOTENCY_CONFLICT",
           "dispatch idempotency digest must bind authorization context");
}

void testDispatchRejectsDuplicateExecutionIdentity() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-identity");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("trip-agent", 3);
    auto agent = std::make_shared<sub::DeterministicSubAgent>(
        target, clock, ids, 2);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "dispatch identity fixture agent must register");

    auto task = dispatchTask(clock, target, "identity-original");
    bindCurrentDispatchCapacity(scheduler, task);
    expect(scheduler.submitDispatch(task, dispatchCall(task)).accepted,
           "original dispatch execution identity must be accepted");
    auto duplicate_execution = dispatchTask(
        clock, target, "different-operation");
    duplicate_execution.execution_id = task.execution_id;
    bindCurrentDispatchCapacity(scheduler, duplicate_execution);
    const auto duplicate = scheduler.submitDispatch(
        duplicate_execution, dispatchCall(duplicate_execution));
    expect(!duplicate.accepted,
           "different operation must not reuse an active execution_id");
}

void testDispatchControlEpochReplayIsIdempotent() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-control");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("trip-agent", 1);
    auto agent = std::make_shared<sub::DeterministicSubAgent>(
        target, clock, ids, 8);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "control epoch fixture agent must register");
    auto task =
        dispatchTask(clock, target, "control", TaskPriority::P2);
    bindCurrentDispatchCapacity(scheduler, task);
    const auto accepted =
        scheduler.submitDispatch(task, dispatchCall(task));
    expect(accepted.accepted && scheduler.pumpOne(),
           "P2 dispatch must enter running state");

    auto preempt_call = dispatchCall(task);
    preempt_call.priority = TaskPriority::P0;
    preempt_call.authorization_ref =
        "trusted-safety:dispatch-control";
    expect(scheduler
               .requestPreempt(accepted.dispatch_id, TaskPriority::P0, 9,
                               preempt_call)
               .ok,
           "first control epoch must suspend the dispatch");
    expect(scheduler
               .requestPreempt(accepted.dispatch_id, TaskPriority::P0, 9,
                               preempt_call)
               .ok,
           "exact replay of an already-applied control epoch must be "
           "idempotent");
    const auto state = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(state.value &&
               state.value->state == dispatch::DispatchState::Suspended &&
               state.value->control_epoch == 9,
           "control replay must preserve the suspended snapshot");
}

void testDispatchAutomaticallyPreemptsAndRestoresLowerPriorityWork() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-preempt");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("trip-agent", 1);
    auto agent = std::make_shared<sub::DeterministicSubAgent>(
        target, clock, ids, 6);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "preemption fixture agent must register");

    auto low_task =
        dispatchTask(clock, target, "auto-preempt-low", TaskPriority::P2);
    bindCurrentDispatchCapacity(scheduler, low_task);
    const auto low =
        scheduler.submitDispatch(low_task, dispatchCall(low_task));
    expect(low.accepted && scheduler.pumpOne(),
           "P2 dispatch must start and hold the only Agent credit");
    const auto low_running = scheduler.queryDispatch(
        low.dispatch_id, dispatchQueryCall(clock, low_task));
    expect(low_running.value &&
               low_running.value->state == dispatch::DispatchState::Running,
           "P2 dispatch must be running before P0 arrives");

    auto high_task =
        dispatchTask(clock, target, "auto-preempt-high", TaskPriority::P0);
    bindCurrentDispatchCapacity(scheduler, high_task);
    const auto high =
        scheduler.submitDispatch(high_task, dispatchCall(high_task));
    expect(high.accepted,
           "authorized P0 dispatch must enter the priority queue");
    expect(scheduler.runUntilIdle().ok,
           "scheduler must preempt, run P0, restore P2 and become idle");

    const auto low_terminal = scheduler.queryDispatch(
        low.dispatch_id, dispatchQueryCall(clock, low_task));
    const auto high_terminal = scheduler.queryDispatch(
        high.dispatch_id, dispatchQueryCall(clock, high_task));
    expect(low_terminal.value && high_terminal.value &&
               low_terminal.value->state ==
                   dispatch::DispatchState::Succeeded &&
               high_terminal.value->state ==
                   dispatch::DispatchState::Succeeded,
           "preemptor and restored victim must both succeed");

    const auto events = scheduler.events();
    const auto eventPosition =
        [&events](const std::string& dispatch_id,
                  const std::string& event_type) {
            const auto found = std::find_if(
                events.begin(), events.end(),
                [&dispatch_id, &event_type](const auto& event) {
                    return event.dispatch_id == dispatch_id &&
                           event.event_type == event_type;
                });
            return static_cast<std::size_t>(
                std::distance(events.begin(), found));
        };
    const auto suspended = eventPosition(low.dispatch_id, "SUSPENDED");
    const auto high_succeeded =
        eventPosition(high.dispatch_id, "SUCCEEDED");
    const auto low_succeeded =
        eventPosition(low.dispatch_id, "SUCCEEDED");
    expect(suspended < events.size() && high_succeeded < events.size() &&
               low_succeeded < events.size() &&
               suspended < high_succeeded &&
               high_succeeded < low_succeeded,
           "event order must prove safe-point suspend, P0 completion and "
           "P2 restoration");
}

/// A non-preemptible manifest must not manufacture scheduler credit. P0 is
/// rejected while the sole lease is occupied, and neither automatic nor
/// explicit control reaches the Provider.
void testDispatchDoesNotOvercommitNonPreemptibleAgent() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "resilience-dispatch-non-preemptible");
    dispatch::AgentDispatch scheduler(clock, ids);
    auto target = manifest("non-preemptible-agent", 1);
    target.max_concurrency = 1;
    target.supports_safe_point_preemption = false;
    auto agent = std::make_shared<ControlCountingSubAgent>(
        target, clock, ids, 20);
    CallContext bootstrap{
        CallerModuleId::AgentService, "bootstrap-non-preemptible",
        "trace-bootstrap-non-preemptible", "principal-resilience",
        TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "non-preemptible Agent must register");

    auto low_task = dispatchTask(
        clock, target, "non-preemptible-low", TaskPriority::P2);
    bindCurrentDispatchCapacity(scheduler, low_task);
    const auto low = scheduler.submitDispatch(
        low_task, dispatchCall(low_task));
    expect(low.accepted && scheduler.pumpOne(),
           "P2 must start and consume the sole non-preemptible lease");
    const auto low_running = scheduler.queryDispatch(
        low.dispatch_id, dispatchQueryCall(clock, low_task));
    expect(low_running.value &&
               low_running.value->state ==
                   dispatch::DispatchState::Running &&
               agent->submitRequests().size() == 1U &&
               agent->submitRequests().front().lease_id ==
                   low_running.value->route.lease_id,
           "P2 Provider submission must own one stable AgentLease");
    const auto original_lease = low_running.value->route.lease_id;

    auto high_task = dispatchTask(
        clock, target, "non-preemptible-high-denied",
        TaskPriority::P0);
    const auto p0_capacity =
        bindCurrentDispatchCapacity(scheduler, high_task);
    const auto high_denied = scheduler.submitDispatch(
        high_task, dispatchCall(high_task));
    expect(
        p0_capacity.available_credits == 0U &&
            !high_denied.accepted &&
            high_denied.reject_code == "NO_AGENT_CAPACITY" &&
            agent->submitRequests().size() == 1U,
        "running non-preemptible P2 must not be advertised as P0 "
        "preemption credit");

    auto preempt_call = dispatchCall(low_task);
    preempt_call.priority = TaskPriority::P0;
    preempt_call.authorization_ref =
        "trusted-safety:non-preemptible-control";
    const auto explicit_preempt = scheduler.requestPreempt(
        low.dispatch_id, TaskPriority::P0, 1, preempt_call);
    expect(!explicit_preempt.ok &&
               explicit_preempt.error.code ==
                   "DISPATCH_PREEMPT_NOT_SUPPORTED" &&
               agent->preemptCount() == 0U,
           "explicit preemption must fail before the non-preemptible "
           "Provider control boundary");

    for (std::size_t step = 0; step < 5U; ++step) {
        expect(scheduler.pumpOne(),
               "live P2 work must make bounded Provider progress");
    }
    const auto still_running = scheduler.queryDispatch(
        low.dispatch_id, dispatchQueryCall(clock, low_task));
    expect(
        still_running.value &&
            still_running.value->state ==
                dispatch::DispatchState::Running &&
            still_running.value->route.lease_id == original_lease &&
            agent->submitRequests().size() == 1U &&
            agent->preemptCount() == 0U &&
            agent->restoreCount() == 0U &&
            agent->cancelCount() == 0U,
        "repeated pumps must not create a control storm, duplicate "
        "submission or replace the live lease");

    expect(scheduler.runUntilIdle().ok,
           "non-preemptible P2 must eventually release its lease");
    auto post_release_high = dispatchTask(
        clock, target, "non-preemptible-high-after-release",
        TaskPriority::P0);
    const auto released_capacity =
        bindCurrentDispatchCapacity(scheduler, post_release_high);
    const auto admitted_high = scheduler.submitDispatch(
        post_release_high, dispatchCall(post_release_high));
    expect(released_capacity.available_credits == 1U &&
               admitted_high.accepted && scheduler.pumpOne() &&
               agent->submitRequests().size() == 2U &&
               agent->submitRequests().at(1).lease_id !=
                   original_lease,
           "after real release P0 may start only with a newly issued "
           "AgentLease");
    expect(scheduler.runUntilIdle().ok,
           "post-release P0 must drain normally");
}

void testDispatchProviderExceptionBecomesUnknownWithoutLeaseRelease() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("resilience-dispatch-fault");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("faulting-trip-agent", 1);
    auto agent = std::make_shared<ThrowingPumpSubAgent>(
        target, clock, ids, 4);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "fault-injection Agent must register");
    auto task =
        dispatchTask(clock, target, "provider-fault", TaskPriority::P1);
    bindCurrentDispatchCapacity(scheduler, task);
    const auto accepted =
        scheduler.submitDispatch(task, dispatchCall(task));
    expect(accepted.accepted && scheduler.pumpOne(),
           "dispatch must be running before the provider fails");

    agent->throwNextPump();
    expect(scheduler.pumpOne(),
           "provider exception must be contained as scheduler progress");
    const auto unknown = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(unknown.value &&
               unknown.value->state == dispatch::DispatchState::Unknown &&
               unknown.value->side_effect_state ==
                   SideEffectState::Unknown &&
               unknown.value->error_code ==
                   "DISPATCH_PROVIDER_PUMP_EXCEPTION",
           "ambiguous external execution must become UNKNOWN");

    const auto events = scheduler.events();
    const auto unknown_event = std::find_if(
        events.begin(), events.end(),
        [&accepted](const auto& event) {
            return event.dispatch_id == accepted.dispatch_id &&
                   event.event_type == "UNKNOWN";
        });
    expect(unknown_event != events.end() &&
               !unknown_event->resource_released,
           "UNKNOWN must not claim that the external lease was released");

    const auto capacity = scheduler.getCapacity(dispatchCall(task));
    expect(capacity.health_state == "DEGRADED" &&
               capacity.available_credits == 0,
           "failed provider must be degraded and its UNKNOWN lease must "
           "remain charged against capacity");
    expect(scheduler.runUntilIdle().ok,
           "UNKNOWN is terminal for automatic scheduling pending explicit "
           "external reconciliation");
}

void testDispatchRejectsStaleProviderSeals() {
    const auto run =
        [](SealTamperingSubAgent::Field field,
           const std::string& suffix,
           const std::string& expected_error) {
            auto clock = std::make_shared<ManualRuntimeClock>();
            auto ids = std::make_shared<IdGenerator>(
                "dispatch-seal-" + suffix);
            dispatch::AgentDispatch scheduler(clock, ids);
            const auto target =
                manifest("seal-agent-" + suffix, 1);
            auto agent =
                std::make_shared<SealTamperingSubAgent>(
                    target, clock, ids, field);
            CallContext bootstrap{
                CallerModuleId::AgentService, "bootstrap-" + suffix,
                "trace-bootstrap-" + suffix,
                "principal-resilience", TaskPriority::P1,
                deadline(clock)};
            expect(scheduler.registerAgent(agent, bootstrap).ok,
                   "seal-tampering Agent must register");
            auto task =
                dispatchTask(clock, target, "seal-" + suffix);
            bindCurrentDispatchCapacity(scheduler, task);
            const auto accepted = scheduler.submitDispatch(
                task, dispatchCall(task));
            expect(accepted.accepted,
                   "seal test dispatch must be accepted");
            expect(scheduler.runUntilIdle().ok,
                   "mismatched provider seal must be isolated");
            const auto snapshot = scheduler.queryDispatch(
                accepted.dispatch_id,
                dispatchQueryCall(clock, task));
            expect(snapshot.value &&
                       snapshot.value->state ==
                           dispatch::DispatchState::Unknown &&
                       snapshot.value->side_effect_state ==
                           SideEffectState::Unknown &&
                       snapshot.value->error_code ==
                           expected_error,
                   "stale provider identity must never advance terminal");
            const auto events = scheduler.events();
            expect(std::none_of(
                       events.begin(), events.end(),
                       [&accepted](const auto& event) {
                           return event.dispatch_id ==
                                      accepted.dispatch_id &&
                                  event.event_type == "SUCCEEDED";
                       }),
                   "invalid provider seal must not emit success");
            if (field !=
                SealTamperingSubAgent::Field::AcceptanceDispatch) {
                const auto reconciled = scheduler.reconcileDispatch(
                    task.operation_id, target.agent_epoch,
                    dispatchCall(task));
                expect(!reconciled.status.ok &&
                           reconciled.status.error.code ==
                               "DISPATCH_PROVIDER_IDENTITY_MISMATCH",
                       "reconcile must enforce the same complete seal");
                const auto retained = scheduler.queryDispatch(
                    accepted.dispatch_id,
                    dispatchQueryCall(clock, task));
                expect(retained.value &&
                           retained.value->state ==
                               dispatch::DispatchState::Unknown,
                       "failed reconciliation must retain UNKNOWN lease");
            }
        };

    run(SealTamperingSubAgent::Field::AcceptanceDispatch,
        "acceptance",
        "DISPATCH_PROVIDER_ACCEPTANCE_IDENTITY_MISMATCH");
    run(SealTamperingSubAgent::Field::Attempt, "attempt",
        "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
    run(SealTamperingSubAgent::Field::AgentEpoch, "epoch",
        "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
    run(SealTamperingSubAgent::Field::Lease, "lease",
        "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
    run(SealTamperingSubAgent::Field::Fence, "fence",
        "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
    run(SealTamperingSubAgent::Field::OutputSchema, "schema",
        "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
}

void testDispatchReconcilesUnknownFromSealedTerminal() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("dispatch-reconcile");
    dispatch::AgentDispatch scheduler(clock, ids);
    const auto target = manifest("reconcile-agent", 1);
    auto agent = std::make_shared<ThrowingPumpSubAgent>(
        target, clock, ids, 1);
    CallContext bootstrap{
        CallerModuleId::AgentService, "bootstrap-reconcile",
        "trace-bootstrap-reconcile", "principal-resilience",
        TaskPriority::P1, deadline(clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "reconcile Agent must register");
    auto task =
        dispatchTask(clock, target, "reconcile");
    bindCurrentDispatchCapacity(scheduler, task);
    const auto accepted =
        scheduler.submitDispatch(task, dispatchCall(task));
    expect(accepted.accepted && scheduler.pumpOne(),
           "dispatch must enter provider execution");
    agent->throwNextPump();
    expect(scheduler.pumpOne(),
           "ambiguous provider failure must create UNKNOWN");
    expect(scheduler.pumpOne(),
           "provider may reach a sealed terminal while isolated");
    const auto unknown = scheduler.queryDispatch(
        accepted.dispatch_id, dispatchQueryCall(clock, task));
    expect(unknown.value &&
               unknown.value->state ==
                   dispatch::DispatchState::Unknown,
           "automatic observation must not guess after ambiguity");

    const auto reconciled = scheduler.reconcileDispatch(
        task.operation_id, target.agent_epoch,
        dispatchCall(task));
    expect(reconciled.status.ok && reconciled.value &&
               reconciled.value->state ==
                   dispatch::DispatchState::Succeeded &&
               reconciled.value->route.lease_id ==
                   unknown.value->route.lease_id,
           "exact sealed terminal evidence must resolve UNKNOWN");
    const auto events = scheduler.events();
    expect(std::any_of(
               events.begin(), events.end(),
               [&accepted](const auto& event) {
                   return event.dispatch_id ==
                              accepted.dispatch_id &&
                          event.event_type ==
                              "RECONCILED_SUCCEEDED" &&
                          event.resource_released;
               }),
           "reconcile must publish a terminal release fact");
}

/// AgentLease ownership is a capacity-generation fact. Reserved slots are
/// excluded from P1/P2 admission and remain directly usable by authorized P0.
void testDispatchCapacityEpochAndReservedP0Credits() {
    {
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids = std::make_shared<IdGenerator>(
            "dispatch-capacity-generation");
        dispatch::AgentDispatch scheduler(clock, ids);
        const auto target =
            manifest("capacity-generation-agent", 1);
        auto agent =
            std::make_shared<sub::DeterministicSubAgent>(
                target, clock, ids, 2);
        CallContext bootstrap{
            CallerModuleId::AgentService,
            "bootstrap-capacity-generation",
            "trace-capacity-generation",
            "principal-resilience", TaskPriority::P1,
            deadline(clock)};
        expect(scheduler.registerAgent(agent, bootstrap).ok,
               "capacity-generation Agent must register");

        auto task = dispatchTask(
            clock, target, "capacity-generation",
            TaskPriority::P1);
        const auto idle_capacity =
            bindCurrentDispatchCapacity(scheduler, task);
        const auto accepted =
            scheduler.submitDispatch(task, dispatchCall(task));
        const auto leased_capacity =
            scheduler.getCapacity(dispatchCall(task));
        expect(accepted.accepted &&
                   leased_capacity.capacity_epoch !=
                       idle_capacity.capacity_epoch &&
                   leased_capacity.available_credits == 0,
               "AgentLease acquisition must advance capacity_epoch and "
               "consume the only ordinary credit");

        expect(scheduler.runUntilIdle().ok,
               "capacity-generation dispatch must complete");
        const auto released_capacity =
            scheduler.getCapacity(dispatchCall(task));
        const auto terminal = scheduler.queryDispatch(
            accepted.dispatch_id,
            dispatchQueryCall(clock, task));
        expect(terminal.value &&
                   terminal.value->state ==
                       dispatch::DispatchState::Succeeded &&
                   released_capacity.capacity_epoch !=
                       leased_capacity.capacity_epoch &&
                   released_capacity.available_credits == 1,
               "terminal AgentLease release must advance capacity_epoch "
               "and restore the ordinary credit");
    }

    {
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids = std::make_shared<IdGenerator>(
            "dispatch-p0-reservation");
        dispatch::AgentDispatch scheduler(clock, ids);
        auto target =
            manifest("reserved-capacity-agent", 1);
        target.max_concurrency = 2;
        target.reserved_p0_slots = 1;
        target.manifest_digest = secureDigest(
            target.agent_id + "|1|plan_trip|max=2|reserved-p0=1");
        auto agent =
            std::make_shared<sub::DeterministicSubAgent>(
                target, clock, ids, 3);
        CallContext bootstrap{
            CallerModuleId::AgentService,
            "bootstrap-p0-reservation",
            "trace-p0-reservation",
            "principal-resilience", TaskPriority::P1,
            deadline(clock)};
        expect(scheduler.registerAgent(agent, bootstrap).ok,
               "P0 reservation Agent must register");

        auto ordinary = dispatchTask(
            clock, target, "reservation-ordinary",
            TaskPriority::P1);
        const auto idle_p1 =
            bindCurrentDispatchCapacity(scheduler, ordinary);
        auto idle_p2_probe = dispatchTask(
            clock, target, "reservation-idle-p2",
            TaskPriority::P2);
        const auto idle_p2 =
            bindCurrentDispatchCapacity(
                scheduler, idle_p2_probe);
        auto idle_p0_probe = dispatchTask(
            clock, target, "reservation-idle-p0",
            TaskPriority::P0);
        const auto idle_p0 =
            bindCurrentDispatchCapacity(
                scheduler, idle_p0_probe);
        expect(idle_p1.max_inflight == 2 &&
                   idle_p1.reserved_p0_credits == 1 &&
                   idle_p1.available_credits == 1 &&
                   idle_p2.available_credits == 1 &&
                   idle_p0.available_credits == 2,
               "idle capacity must expose one ordinary and one P0-reserved "
               "credit");

        const auto ordinary_accept =
            scheduler.submitDispatch(
                ordinary, dispatchCall(ordinary));
        expect(ordinary_accept.accepted,
               "P1 must consume the one ordinary credit");

        auto blocked_p1 = dispatchTask(
            clock, target, "reservation-blocked-p1",
            TaskPriority::P1);
        const auto blocked_p1_capacity =
            bindCurrentDispatchCapacity(
                scheduler, blocked_p1);
        const auto p1_rejected =
            scheduler.submitDispatch(
                blocked_p1, dispatchCall(blocked_p1));

        auto blocked_p2 = dispatchTask(
            clock, target, "reservation-blocked-p2",
            TaskPriority::P2);
        const auto blocked_p2_capacity =
            bindCurrentDispatchCapacity(
                scheduler, blocked_p2);
        const auto p2_rejected =
            scheduler.submitDispatch(
                blocked_p2, dispatchCall(blocked_p2));
        expect(blocked_p1_capacity.available_credits == 0 &&
                   blocked_p2_capacity.available_credits == 0 &&
                   !p1_rejected.accepted &&
                   p1_rejected.reject_code ==
                       "NO_AGENT_CAPACITY" &&
                   !p2_rejected.accepted &&
                   p2_rejected.reject_code ==
                       "NO_AGENT_CAPACITY",
               "P1 and P2 must not consume the reserved P0 slot");

        auto urgent = dispatchTask(
            clock, target, "reservation-urgent",
            TaskPriority::P0);
        const auto before_urgent =
            bindCurrentDispatchCapacity(scheduler, urgent);
        const auto urgent_accept =
            scheduler.submitDispatch(
                urgent, dispatchCall(urgent));
        const auto after_urgent =
            scheduler.getCapacity(dispatchCall(urgent));
        expect(before_urgent.available_credits == 1 &&
                   urgent_accept.accepted &&
                   after_urgent.available_credits == 0 &&
                   after_urgent.capacity_epoch !=
                       before_urgent.capacity_epoch,
               "authorized P0 must acquire the reserved credit and "
               "advance the capacity generation");

        expect(scheduler.runUntilIdle().ok,
               "ordinary and reserved P0 dispatches must drain");
        const auto ordinary_terminal =
            scheduler.queryDispatch(
                ordinary_accept.dispatch_id,
                dispatchQueryCall(clock, ordinary));
        const auto urgent_terminal =
            scheduler.queryDispatch(
                urgent_accept.dispatch_id,
                dispatchQueryCall(clock, urgent));
        const auto final_p1 =
            scheduler.getCapacity(dispatchCall(ordinary));
        const auto final_p0 =
            scheduler.getCapacity(dispatchCall(urgent));
        expect(ordinary_terminal.value &&
                   urgent_terminal.value &&
                   ordinary_terminal.value->state ==
                       dispatch::DispatchState::Succeeded &&
                   urgent_terminal.value->state ==
                       dispatch::DispatchState::Succeeded &&
                   final_p1.available_credits == 1 &&
                   final_p0.available_credits == 2 &&
                   final_p0.capacity_epoch !=
                       after_urgent.capacity_epoch,
               "lease release must restore ordinary/P0 views without "
               "exposing the reservation to P1");
    }
}

struct OrchestratorFixture {
    std::shared_ptr<ManualRuntimeClock> clock =
        std::make_shared<ManualRuntimeClock>();
    std::shared_ptr<IdGenerator> ids =
        std::make_shared<IdGenerator>("resilience-orchestrator");
    std::shared_ptr<atomic::DeterministicClimateProvider> provider =
        std::make_shared<atomic::DeterministicClimateProvider>();
    std::shared_ptr<atomic::AtomicServiceManager> atomic_manager =
        std::make_shared<atomic::AtomicServiceManager>(clock, ids, 1);
    std::shared_ptr<dispatch::AgentDispatch> agent_dispatch =
        std::make_shared<dispatch::AgentDispatch>(clock, ids);
    orch::Orchestrator orchestrator{
        clock, ids, atomic_manager, agent_dispatch};
    CallContext agent_service_call{CallerModuleId::AgentService,
                                   "request-orchestrator",
                                   "trace-orchestrator",
                                   "principal-resilience",
                                   TaskPriority::P1,
                                   deadline(clock)};

    OrchestratorFixture() {
        expect(atomic_manager
                   ->registerTools(
                       atomic::defaultClimateMcpTools(),
                       atomic::defaultClimateRuntimePolicies(2), provider,
                       agent_service_call)
                   .ok,
               "orchestrator atomic catalog must register");
        const auto trip_manifest = manifest("trip-agent", 1);
        auto trip_agent =
            std::make_shared<sub::DeterministicSubAgent>(
                trip_manifest, clock, ids, 2);
        expect(agent_dispatch
                   ->registerAgent(trip_agent, agent_service_call)
                   .ok,
               "orchestrator dispatch agent must register");
    }

    orch::AdmissionContext admission(TaskPriority priority) const {
        orch::AdmissionContext value;
        value.principal_id_hash = "principal-resilience";
        value.granted_priority = priority;
        value.p0_authorization = priority == TaskPriority::P0;
        value.policy_snapshot_id = "policy-resilience";
        value.policy_digest = secureDigest("policy-resilience");
        value.authorization_ref =
            priority == TaskPriority::P0
                ? "trusted-safety:resilience-grant"
                : "policy:" + value.policy_digest;
        value.deadline_mono_ns = deadline(clock);
        value.allowed_capabilities = {
            "com_sgm_service_climate_setAirCirculationMode",
            "com_sgm_service_climate_setAutoFanSpeed", "plan_trip"};
        value.granted_permissions = {
            "vehicle.climate.write"};
        value.p0_allowed_capabilities = value.allowed_capabilities;
        return value;
    }

    orch::OrchestratorSubmitRequest request(
        std::string dag_id, std::vector<orch::DAGNode> nodes,
        TaskPriority priority) {
        orch::OrchestratorSubmitRequest value;
        value.dag.dag_id = std::move(dag_id);
        value.dag.request_id = "request-" + value.dag.dag_id;
        value.dag.nodes = std::move(nodes);
        value.dag.priority = priority;
        value.dag.deadline_mono_ns = deadline(clock);
        value.dag.idempotency_key = "idem-" + value.dag.dag_id;
        value.admission = admission(priority);
        value.idempotency_key = value.dag.idempotency_key;
        value.trace_id = "trace-" + value.dag.dag_id;
        const auto catalog =
            atomic_manager->getToolCatalogSnapshot(agent_service_call);
        expect(catalog.status.ok && catalog.value,
               "orchestrator fixture catalog must be readable");
        value.expected_capability_digest =
            catalog.value->catalog_digest;
        agent_service_call.request_id = value.dag.request_id;
        agent_service_call.trace_id = value.trace_id;
        agent_service_call.priority =
            value.admission.granted_priority;
        agent_service_call.deadline_mono_ns =
            value.admission.deadline_mono_ns;
        agent_service_call.authorization_ref =
            value.admission.authorization_ref;
        return value;
    }
};

const std::string kRetryAtomicTool =
    "com_sgm_service_climate_setAutoFanSpeed";

orch::DAGNode atomicRetryNode(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const std::string& node_id) {
    orch::DAGNode node;
    node.node_id = node_id;
    node.executor = "atomic_service";
    node.action = kRetryAtomicTool;
    node.params =
        nlohmann::json{{"location", "FRONT"}, {"mode", "HIGH"}};
    node.base_priority = TaskPriority::P1;
    node.deadline_mono_ns = deadline(clock);
    node.max_attempts = 2;
    return node;
}

atomic::McpToolCatalogSnapshot orchestratorCatalog(
    const OrchestratorFixture& fixture) {
    const auto catalog =
        fixture.atomic_manager->getToolCatalogSnapshot(
            fixture.agent_service_call);
    expect(catalog.status.ok && catalog.value,
           "retry test catalog must be readable");
    return *catalog.value;
}

orch::CapabilityRetryPolicy retryPolicyFromCatalog(
    const atomic::McpToolCatalogSnapshot& catalog,
    const std::string& action, std::int64_t base_backoff_ns,
    std::int64_t max_backoff_ns) {
    orch::CapabilityRetryPolicy policy;
    policy.idempotency_policy =
        catalog.idempotency_policies.at(action);
    const auto& errors = catalog.retryable_errors.at(action);
    policy.retryable_errors =
        std::set<std::string>(errors.begin(), errors.end());
    policy.base_backoff_ns = base_backoff_ns;
    policy.max_backoff_ns = max_backoff_ns;
    return policy;
}

void issueRetryPolicy(
    orch::OrchestratorSubmitRequest& request,
    const std::string& action,
    orch::CapabilityRetryPolicy policy) {
    request.admission.retry_policies[action] =
        std::move(policy);
    request.admission.retry_policy_digest =
        orch::retryPoliciesDigest(
            request.admission.retry_policies);
}

void installRetryProvider(
    OrchestratorFixture& fixture,
    const std::shared_ptr<ScriptedRetryAtomicProvider>& provider,
    std::uint32_t work_units = 1) {
    expect(
        fixture.atomic_manager
            ->registerTools(
                atomic::defaultClimateMcpTools(),
                atomic::defaultClimateRuntimePolicies(work_units),
                provider, fixture.agent_service_call)
            .ok,
        "scripted retry Provider catalog must register");
}

void testOrchestratorHonorsDependenciesAndRejectsCycles() {
    OrchestratorFixture fixture;
    orch::DAGNode first;
    first.node_id = "prepare";
    first.executor = "atomic_service";
    first.action =
        "com_sgm_service_climate_setAirCirculationMode";
    first.params = nlohmann::json{{"mode", "INTERNAL"}};
    first.base_priority = TaskPriority::P1;
    first.deadline_mono_ns = deadline(fixture.clock);

    orch::DAGNode second;
    second.node_id = "finish";
    second.executor = "atomic_service";
    second.action = "com_sgm_service_climate_setAutoFanSpeed";
    second.params =
        nlohmann::json{{"location", "FRONT"}, {"mode", "HIGH"}};
    second.dependencies = {"prepare"};
    second.base_priority = TaskPriority::P1;
    second.deadline_mono_ns = deadline(fixture.clock);

    const auto request = fixture.request(
        "ordered-dependencies", {first, second}, TaskPriority::P1);
    fixture.agent_service_call.request_id = request.dag.request_id;
    fixture.agent_service_call.trace_id = request.trace_id;
    const auto committed =
        fixture.orchestrator.submit(request, fixture.agent_service_call);
    expect(committed.accepted,
           "valid dependency DAG must commit");
    const auto initial = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(initial.value &&
               initial.value->nodes.at("prepare").state ==
                   orch::ActivationState::Ready &&
               initial.value->nodes.at("finish").state ==
                   orch::ActivationState::Blocked,
           "dependent node must remain blocked before prerequisite");
    expect(fixture.orchestrator
               .runUntilPlanTerminal(committed.plan_id)
               .ok,
           "dependency DAG must reach terminal state");
    const auto terminal = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(terminal.value &&
               terminal.value->state == orch::PlanState::Succeeded,
           "ordered dependency DAG must succeed");

    const auto events = fixture.orchestrator.events();
    const auto prepare_pid = committed.node_id_to_pid.at("prepare");
    const auto finish_pid = committed.node_id_to_pid.at("finish");
    const auto find_event =
        [&events](const std::string& pid, const std::string& type) {
            const auto found = std::find_if(
                events.begin(), events.end(), [&](const auto& event) {
                    return event.pid == pid && event.event_type == type;
                });
            return found == events.end()
                       ? events.size()
                       : static_cast<std::size_t>(
                             std::distance(events.begin(), found));
        };
    expect(find_event(prepare_pid, "NODE_SUCCEEDED") <
               find_event(finish_pid, "NODE_READY") &&
               find_event(finish_pid, "NODE_READY") <
                   find_event(finish_pid, "DISPATCH_PENDING"),
           "dependent dispatch must occur only after prerequisite success");

    auto cyclic_first = first;
    auto cyclic_second = second;
    cyclic_first.node_id = "cycle-a";
    cyclic_first.dependencies = {"cycle-b"};
    cyclic_second.node_id = "cycle-b";
    cyclic_second.dependencies = {"cycle-a"};
    const auto cyclic = fixture.request(
        "cyclic", {cyclic_first, cyclic_second}, TaskPriority::P1);
    fixture.agent_service_call.request_id = cyclic.dag.request_id;
    fixture.agent_service_call.trace_id = cyclic.trace_id;
    const auto validation = fixture.orchestrator.validateDAG(
        cyclic.dag, cyclic.admission, fixture.agent_service_call);
    expect(!validation.valid &&
               validation.reject_code == "ORCHESTRATOR_DAG_CYCLE",
           "cyclic dependencies must be rejected before commit; actual=" +
               validation.reject_code);
}

void testOrchestratorSelectsHighestPriorityReadyNode() {
    OrchestratorFixture fixture;
    orch::DAGNode low;
    low.node_id = "low";
    low.executor = "atomic_service";
    low.action =
        "com_sgm_service_climate_setAirCirculationMode";
    low.params = nlohmann::json{{"mode", "AUTO"}};
    low.base_priority = TaskPriority::P2;
    low.deadline_mono_ns = deadline(fixture.clock);

    orch::DAGNode high;
    high.node_id = "high";
    high.executor = "atomic_service";
    high.action = "com_sgm_service_climate_setAutoFanSpeed";
    high.params =
        nlohmann::json{{"location", "REAR"}, {"mode", "NORMAL"}};
    high.base_priority = TaskPriority::P1;
    high.deadline_mono_ns = deadline(fixture.clock);

    const auto request =
        fixture.request("priority-order", {low, high}, TaskPriority::P1);
    fixture.agent_service_call.request_id = request.dag.request_id;
    fixture.agent_service_call.trace_id = request.trace_id;
    const auto committed =
        fixture.orchestrator.submit(request, fixture.agent_service_call);
    expect(committed.accepted, "priority DAG must commit");
    expect(fixture.orchestrator.pumpOne(),
           "orchestrator must dispatch one ready node");
    const auto snapshot = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(snapshot.value &&
               snapshot.value->nodes.at("high").state ==
                   orch::ActivationState::Queued &&
               snapshot.value->nodes.at("low").state ==
                   orch::ActivationState::Ready,
           "P1 ready node must be accepted before the P2 ready node");
    expect(fixture.orchestrator.pumpOne(),
           "accepted execution must advance from downstream lifecycle");
    const auto started = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(started.value &&
               started.value->nodes.at("high").state ==
                   orch::ActivationState::Running,
           "DispatchAcceptance must remain QUEUED until the executor "
           "reports that execution started");
}

/// Capacity races are scheduler re-evaluation facts, not business Attempts.
/// Two independent Ready nodes sharing one Agent credit must serialize.
void testOrchestratorReevaluatesAgentCapacityWithoutAttempt() {
    OrchestratorFixture fixture;
    orch::DAGNode first;
    first.node_id = "capacity-first";
    first.executor = "agent_dispatch";
    first.action = "plan_trip";
    first.target_agent = "trip-agent";
    first.params =
        nlohmann::json{{"destination", "first"}};
    first.base_priority = TaskPriority::P1;
    first.deadline_mono_ns = deadline(fixture.clock);
    first.max_attempts = 1;

    auto second = first;
    second.node_id = "capacity-second";
    second.params =
        nlohmann::json{{"destination", "second"}};

    const auto request = fixture.request(
        "dispatch-capacity-reevaluate",
        {first, second}, TaskPriority::P1);
    const auto committed = fixture.orchestrator.submit(
        request, fixture.agent_service_call);
    expect(committed.accepted,
           "two independent Agent nodes must commit");
    const auto initial = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(initial.value &&
               initial.value->nodes.at(first.node_id).state ==
                   orch::ActivationState::Ready &&
               initial.value->nodes.at(second.node_id).state ==
                   orch::ActivationState::Ready,
           "both dependency-free Agent nodes must begin Ready");

    expect(fixture.orchestrator.pumpOne(),
           "the first Ready node must acquire the AgentLease");
    bool saw_capacity_reevaluation = false;
    std::string waiting_node_id;
    for (std::size_t step = 0;
         step < 4 && !saw_capacity_reevaluation; ++step) {
        expect(fixture.orchestrator.pumpOne(),
               "capacity contention must remain bounded scheduler "
               "progress");
        const auto snapshot = fixture.orchestrator.getPlan(
            committed.plan_id, fixture.agent_service_call);
        expect(snapshot.value.has_value(),
               "capacity re-evaluation snapshot must be queryable");
        for (const auto& [node_id, node] :
             snapshot.value->nodes) {
            if (node.state == orch::ActivationState::Ready &&
                node.error_code == "NO_AGENT_CAPACITY") {
                waiting_node_id = node_id;
                saw_capacity_reevaluation = true;
                expect(node.attempt_count == 0 &&
                           node.execution_id.empty() &&
                           node.operation_id.empty() &&
                           node.fencing_token == 0,
                       "NO_AGENT_CAPACITY must release the prepared "
                       "identity and consume no Attempt");
                break;
            }
        }
    }
    expect(saw_capacity_reevaluation,
           "the second Ready node must emit a capacity re-evaluation");

    const auto events_at_contention =
        fixture.orchestrator.events();
    const auto waiting_pid =
        committed.node_id_to_pid.at(waiting_node_id);
    expect(std::any_of(
               events_at_contention.begin(),
               events_at_contention.end(),
               [&](const auto& event) {
                   return event.plan_id == committed.plan_id &&
                          event.pid == waiting_pid &&
                          event.event_type ==
                              "DISPATCH_REEVALUATE";
               }) &&
               std::none_of(
                   events_at_contention.begin(),
                   events_at_contention.end(),
                   [&](const auto& event) {
                       return event.plan_id == committed.plan_id &&
                              event.pid == waiting_pid &&
                              event.event_type ==
                                  "DISPATCH_REJECTED";
                   }),
           "NO_AGENT_CAPACITY must be observable as "
           "DISPATCH_REEVALUATE, never as terminal rejection");

    const auto drained =
        fixture.orchestrator.runUntilPlanTerminal(
            committed.plan_id);
    const auto terminal = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(drained.ok && terminal.value &&
               terminal.value->state ==
                   orch::PlanState::Succeeded &&
               terminal.value->nodes.at(first.node_id).state ==
                   orch::ActivationState::Succeeded &&
               terminal.value->nodes.at(second.node_id).state ==
                   orch::ActivationState::Succeeded &&
               terminal.value->nodes.at(first.node_id).attempt_count ==
                   1 &&
               terminal.value->nodes.at(second.node_id).attempt_count ==
                   1,
           "after lease release both nodes must succeed serially with "
           "exactly one business Attempt each");

    const auto completed_events =
        fixture.orchestrator.events();
    const auto running_node_id =
        waiting_node_id == first.node_id
            ? second.node_id
            : first.node_id;
    const auto running_pid =
        committed.node_id_to_pid.at(running_node_id);
    const auto event_position =
        [&](const std::string& pid,
            const std::string& event_type) {
            const auto found = std::find_if(
                completed_events.begin(),
                completed_events.end(),
                [&](const auto& event) {
                    return event.plan_id ==
                               committed.plan_id &&
                           event.pid == pid &&
                           event.event_type ==
                               event_type;
                });
            return found == completed_events.end()
                       ? completed_events.size()
                       : static_cast<std::size_t>(
                             std::distance(
                                 completed_events.begin(),
                                 found));
        };
    expect(event_position(running_pid, "NODE_SUCCEEDED") <
               event_position(
                   waiting_pid, "DISPATCH_ACCEPTED"),
           "the waiting node may acquire its AgentLease only after the "
           "first node publishes terminal release");
}

void testOrchestratorIdempotencyBindsDependencyGraph() {
    OrchestratorFixture fixture;
    orch::DAGNode first;
    first.node_id = "graph-first";
    first.executor = "atomic_service";
    first.action =
        "com_sgm_service_climate_setAirCirculationMode";
    first.params = nlohmann::json{{"mode", "INTERNAL"}};
    first.base_priority = TaskPriority::P1;
    first.deadline_mono_ns = deadline(fixture.clock);

    orch::DAGNode second;
    second.node_id = "graph-second";
    second.executor = "atomic_service";
    second.action = "com_sgm_service_climate_setAutoFanSpeed";
    second.params =
        nlohmann::json{{"location", "FRONT"}, {"mode", "NORMAL"}};
    second.dependencies = {"graph-first"};
    second.base_priority = TaskPriority::P1;
    second.deadline_mono_ns = deadline(fixture.clock);

    const auto original = fixture.request(
        "graph-idempotency", {first, second}, TaskPriority::P1);
    fixture.agent_service_call.request_id = original.dag.request_id;
    fixture.agent_service_call.trace_id = original.trace_id;
    const auto accepted =
        fixture.orchestrator.submit(original, fixture.agent_service_call);
    expect(accepted.accepted && !accepted.existing,
           "original dependency graph must commit");

    auto transport_retry = original;
    transport_retry.dag.dag_id = "new-transport-dag-id";
    transport_retry.submitted_at_utc_ms += 1000;
    const auto replay = fixture.orchestrator.submit(
        transport_retry, fixture.agent_service_call);
    expect(replay.accepted && replay.existing &&
               replay.plan_id == accepted.plan_id,
           "new DAG transport identity/time must replay the logical plan");

    auto changed_graph = original;
    changed_graph.dag.nodes.at(1).dependencies.clear();
    const auto conflict = fixture.orchestrator.submit(
        changed_graph, fixture.agent_service_call);
    expect(!conflict.accepted &&
               conflict.reject_code == "ORCHESTRATOR_IDEMPOTENCY_CONFLICT",
           "plan idempotency digest must bind dependency edges");
}

/// Admission must issue and sign retry authority; DAG/LLM claims alone never
/// create it, and the live Atomic catalog remains the capability authority.
void testOrchestratorRejectsUntrustedRetryContracts() {
    {
        OrchestratorFixture fixture;
        const auto node =
            atomicRetryNode(fixture.clock, "retry-policy-missing");
        const auto request = fixture.request(
            "retry-policy-missing", {node}, TaskPriority::P1);
        const auto rejected = fixture.orchestrator.submit(
            request, fixture.agent_service_call);
        expect(
            !rejected.accepted &&
                rejected.reject_code ==
                    "ORCHESTRATOR_RETRY_POLICY_REQUIRED" &&
                fixture.provider->invocationCount() == 0U,
            "max_attempts > 1 must fail closed without an "
            "Admission-issued retry policy");
    }

    {
        OrchestratorFixture fixture;
        const auto node =
            atomicRetryNode(fixture.clock, "retry-non-retryable");
        auto request = fixture.request(
            "retry-non-retryable", {node}, TaskPriority::P1);
        orch::CapabilityRetryPolicy policy;
        policy.idempotency_policy = "NON_RETRYABLE";
        policy.retryable_errors = {"PROVIDER_EXECUTION_FAILED"};
        policy.base_backoff_ns = 10'000'000LL;
        policy.max_backoff_ns = 10'000'000LL;
        issueRetryPolicy(request, node.action, std::move(policy));
        const auto rejected = fixture.orchestrator.submit(
            request, fixture.agent_service_call);
        expect(
            !rejected.accepted &&
                rejected.reject_code ==
                    "ORCHESTRATOR_RETRY_POLICY_INVALID" &&
                fixture.provider->invocationCount() == 0U,
            "NON_RETRYABLE Admission policy must never authorize retry");
    }

    {
        OrchestratorFixture fixture;
        const auto node =
            atomicRetryNode(fixture.clock, "retry-digest-forged");
        auto request = fixture.request(
            "retry-digest-forged", {node}, TaskPriority::P1);
        issueRetryPolicy(
            request, node.action,
            retryPolicyFromCatalog(
                orchestratorCatalog(fixture), node.action,
                10'000'000LL, 40'000'000LL));
        request.admission.retry_policy_digest =
            secureDigest("forged-retry-policy");
        const auto rejected = fixture.orchestrator.submit(
            request, fixture.agent_service_call);
        expect(
            !rejected.accepted &&
                rejected.reject_code ==
                    "ORCHESTRATOR_RETRY_POLICY_DIGEST_INVALID" &&
                fixture.provider->invocationCount() == 0U,
            "forged Admission retry-policy digest must be rejected "
            "before plan commit");
    }

    {
        OrchestratorFixture fixture;
        const auto node =
            atomicRetryNode(fixture.clock, "retry-catalog-mismatch");
        auto request = fixture.request(
            "retry-catalog-mismatch", {node}, TaskPriority::P1);
        auto policy = retryPolicyFromCatalog(
            orchestratorCatalog(fixture), node.action,
            10'000'000LL, 40'000'000LL);
        expect(policy.idempotency_policy == "TARGET_STATE",
               "default Atomic retry contract must be TARGET_STATE");
        policy.idempotency_policy = "READ_ONLY";
        issueRetryPolicy(request, node.action, std::move(policy));
        const auto rejected = fixture.orchestrator.submit(
            request, fixture.agent_service_call);
        expect(
            !rejected.accepted &&
                rejected.reject_code ==
                    "ORCHESTRATOR_RETRY_POLICY_CAPABILITY_MISMATCH" &&
                fixture.provider->invocationCount() == 0U,
            "Admission policy that differs from the runtime Atomic "
            "catalog must be rejected before plan commit");
    }
}

/// A Failed observation is retry-eligible only when both the Provider hint
/// and the Admission/catalog error whitelist authorize it.
void testOrchestratorRequiresEveryRetryEligibilitySignal() {
    const auto run_terminal_failure =
        [](const std::string& tag, const std::string& error_code,
           bool retryable_hint) {
            OrchestratorFixture fixture;
            auto provider =
                std::make_shared<ScriptedRetryAtomicProvider>(
                    std::vector<ScriptedAtomicOutcome>{
                        {false, error_code,
                         SideEffectState::ConfirmedNotExecuted,
                         retryable_hint}});
            installRetryProvider(fixture, provider);
            const auto node =
                atomicRetryNode(fixture.clock, "retry-signal-" + tag);
            auto request = fixture.request(
                "retry-signal-" + tag, {node}, TaskPriority::P1);
            issueRetryPolicy(
                request, node.action,
                retryPolicyFromCatalog(
                    orchestratorCatalog(fixture), node.action,
                    10'000'000LL, 40'000'000LL));
            const auto committed = fixture.orchestrator.submit(
                request, fixture.agent_service_call);
            expect(committed.accepted &&
                       fixture.orchestrator
                           .runUntilPlanTerminal(committed.plan_id)
                           .ok,
                   "retry eligibility negative plan must settle");
            const auto terminal = fixture.orchestrator.getPlan(
                committed.plan_id, fixture.agent_service_call);
            const auto events = fixture.orchestrator.events();
            expect(
                terminal.value &&
                    terminal.value->state == orch::PlanState::Failed &&
                    terminal.value->nodes.at(node.node_id).state ==
                        orch::ActivationState::Failed &&
                    terminal.value->nodes.at(node.node_id).attempt_count ==
                        1U &&
                    terminal.value->nodes.at(node.node_id)
                            .retryable_hint ==
                        retryable_hint &&
                    terminal.value->nodes.at(node.node_id)
                            .retry_at_mono_ns ==
                        0 &&
                    provider->calls().size() == 1U &&
                    std::none_of(
                        events.begin(), events.end(),
                        [&committed](const auto& event) {
                            return event.plan_id ==
                                       committed.plan_id &&
                                   (event.event_type ==
                                        "NODE_RETRY_SCHEDULED" ||
                                    event.event_type ==
                                        "NODE_RETRY_READY");
                        }),
                "missing whitelist or retryable hint must remain one "
                "terminal Attempt; case=" +
                    tag);
        };

    run_terminal_failure(
        "error-not-whitelisted", "NON_WHITELISTED_FAILURE", true);
    run_terminal_failure(
        "hint-false", "PROVIDER_EXECUTION_FAILED", false);
}

/// The retry wake-up is an absolute monotonic point. It must not become Ready
/// or consume a new Attempt before that point; the next Attempt receives fresh
/// execution/operation/fence identities.
void testOrchestratorSchedulesAbsoluteAtomicRetry() {
    OrchestratorFixture fixture;
    auto provider =
        std::make_shared<ScriptedRetryAtomicProvider>(
            std::vector<ScriptedAtomicOutcome>{
                {false, "PROVIDER_EXECUTION_FAILED",
                 SideEffectState::ConfirmedNotExecuted, true},
                {true, {}, SideEffectState::Committed, false}});
    installRetryProvider(fixture, provider);
    const auto node =
        atomicRetryNode(fixture.clock, "absolute-retry");
    auto request = fixture.request(
        "absolute-retry", {node}, TaskPriority::P1);
    constexpr std::int64_t kBackoffNs = 10'000'000LL;
    issueRetryPolicy(
        request, node.action,
        retryPolicyFromCatalog(
            orchestratorCatalog(fixture), node.action,
            kBackoffNs, 40'000'000LL));
    const auto committed = fixture.orchestrator.submit(
        request, fixture.agent_service_call);
    expect(committed.accepted,
           "valid TARGET_STATE retry plan must commit");

    Result<orch::TaskPlanSnapshot> scheduled;
    for (std::size_t step = 0; step < 20U; ++step) {
        const bool progressed = fixture.orchestrator.pumpOne();
        scheduled = fixture.orchestrator.getPlan(
            committed.plan_id, fixture.agent_service_call);
        if (scheduled.value &&
            scheduled.value->nodes.at(node.node_id).state ==
                orch::ActivationState::Blocked &&
            scheduled.value->nodes.at(node.node_id)
                    .retry_at_mono_ns >
                0) {
            break;
        }
        if (!progressed) break;
    }
    expect(scheduled.value.has_value(),
           "scheduled retry snapshot must be queryable");
    const auto first_attempt =
        scheduled.value->nodes.at(node.node_id);
    const auto failure_observed_at =
        fixture.clock->monotonicNowNs();
    expect(
        first_attempt.state == orch::ActivationState::Blocked &&
            first_attempt.attempt_count == 1U &&
            first_attempt.retryable_hint &&
            first_attempt.error_code ==
                "PROVIDER_EXECUTION_FAILED" &&
            first_attempt.side_effect_state ==
                SideEffectState::ConfirmedNotExecuted &&
            first_attempt.retry_at_mono_ns ==
                failure_observed_at + kBackoffNs &&
            provider->calls().size() == 1U,
        "eligible TARGET_STATE failure must schedule one absolute "
        "retry_at without dispatching Attempt 2");

    for (std::size_t spin = 0; spin < 5U; ++spin) {
        expect(!fixture.orchestrator.pumpOne(),
               "blocked retry must not report false progress or busy-loop");
    }
    fixture.clock->advanceMs(9);
    expect(!fixture.orchestrator.pumpOne(),
           "retry must remain Blocked one millisecond before retry_at");
    const auto before_due = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(before_due.value &&
               before_due.value->nodes.at(node.node_id).state ==
                   orch::ActivationState::Blocked &&
               before_due.value->nodes.at(node.node_id).attempt_count ==
                   1U &&
               before_due.value->nodes.at(node.node_id).execution_id ==
                   first_attempt.execution_id &&
               provider->calls().size() == 1U,
           "pre-due pumps must preserve the first Attempt identity");

    fixture.clock->advanceMs(1);
    expect(fixture.orchestrator.pumpOne(),
           "retry_at must release the activation for Attempt 2");
    const auto second_dispatched = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(
        second_dispatched.value &&
            second_dispatched.value->nodes.at(node.node_id).attempt_count ==
                2U &&
            second_dispatched.value->nodes.at(node.node_id)
                    .retry_at_mono_ns ==
                0 &&
            second_dispatched.value->nodes.at(node.node_id).execution_id !=
                first_attempt.execution_id &&
            second_dispatched.value->nodes.at(node.node_id).operation_id !=
                first_attempt.operation_id &&
            second_dispatched.value->nodes.at(node.node_id).fencing_token >
                first_attempt.fencing_token &&
            provider->calls().size() == 1U,
        "due retry must allocate fresh execution, operation and fence "
        "before entering the Provider");

    expect(fixture.orchestrator
               .runUntilPlanTerminal(committed.plan_id)
               .ok,
           "second retry Attempt must drain to terminal");
    const auto terminal = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    const auto events = fixture.orchestrator.events();
    const auto scheduled_events =
        static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(),
            [&committed](const auto& event) {
                return event.plan_id == committed.plan_id &&
                       event.event_type ==
                           "NODE_RETRY_SCHEDULED";
            }));
    const auto ready_events =
        static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(),
            [&committed](const auto& event) {
                return event.plan_id == committed.plan_id &&
                       event.event_type == "NODE_RETRY_READY";
            }));
    expect(
        terminal.value &&
            terminal.value->state == orch::PlanState::Succeeded &&
            terminal.value->nodes.at(node.node_id).state ==
                orch::ActivationState::Succeeded &&
            terminal.value->nodes.at(node.node_id).attempt_count == 2U &&
            provider->calls().size() == 2U &&
            provider->calls().at(0).runtime.attempt_no == 1U &&
            provider->calls().at(1).runtime.attempt_no == 2U &&
            provider->calls().at(0).runtime.execution_id !=
                provider->calls().at(1).runtime.execution_id &&
            provider->calls().at(0).runtime.operation_id !=
                provider->calls().at(1).runtime.operation_id &&
            provider->calls().at(0).runtime.fencing_token <
                provider->calls().at(1).runtime.fencing_token &&
            scheduled_events == 1U && ready_events == 1U,
        "Attempt 2 must be physically invoked exactly once with a fresh "
        "sealed identity and one retry lifecycle");
}

/// A backoff at or beyond either effective deadline cannot authorize a new
/// side effect, even when every other retry signal is present.
void testOrchestratorDoesNotRetryAtOrBeyondDeadline() {
    const auto run_deadline_case =
        [](const std::string& tag, std::int64_t node_budget_ms,
           std::int64_t plan_budget_ms) {
            OrchestratorFixture fixture;
            auto provider =
                std::make_shared<ScriptedRetryAtomicProvider>(
                    std::vector<ScriptedAtomicOutcome>{
                        {false, "PROVIDER_EXECUTION_FAILED",
                         SideEffectState::ConfirmedNotExecuted, true}});
            installRetryProvider(fixture, provider);
            auto node = atomicRetryNode(
                fixture.clock, "retry-deadline-" + tag);
            const auto now = fixture.clock->monotonicNowNs();
            node.deadline_mono_ns =
                now + node_budget_ms * 1'000'000LL;
            auto request = fixture.request(
                "retry-deadline-" + tag, {node},
                TaskPriority::P1);
            request.dag.deadline_mono_ns =
                now + plan_budget_ms * 1'000'000LL;
            issueRetryPolicy(
                request, node.action,
                retryPolicyFromCatalog(
                    orchestratorCatalog(fixture), node.action,
                    10'000'000LL, 10'000'000LL));
            const auto committed = fixture.orchestrator.submit(
                request, fixture.agent_service_call);
            expect(committed.accepted &&
                       fixture.orchestrator
                           .runUntilPlanTerminal(committed.plan_id)
                           .ok,
                   "retry deadline boundary plan must settle");
            const auto terminal = fixture.orchestrator.getPlan(
                committed.plan_id, fixture.agent_service_call);
            const auto events = fixture.orchestrator.events();
            expect(
                terminal.value &&
                    terminal.value->state == orch::PlanState::Failed &&
                    terminal.value->nodes.at(node.node_id).state ==
                        orch::ActivationState::Failed &&
                    terminal.value->nodes.at(node.node_id).attempt_count ==
                        1U &&
                    terminal.value->nodes.at(node.node_id)
                            .retry_at_mono_ns ==
                        0 &&
                    provider->calls().size() == 1U &&
                    std::none_of(
                        events.begin(), events.end(),
                        [&committed](const auto& event) {
                            return event.plan_id ==
                                       committed.plan_id &&
                                   event.event_type ==
                                       "NODE_RETRY_SCHEDULED";
                        }),
                "retry_at at/beyond node or plan deadline must remain "
                "terminal; case=" +
                    tag);
        };

    // retry_at == node deadline while still before the plan deadline.
    run_deadline_case("node-bound", 10, 20);
    // retry_at == both node and effective plan deadlines.
    run_deadline_case("plan-bound", 10, 10);
}

void testOrchestratorReconcilesUnknownWithoutAutomaticRetry() {
    OrchestratorFixture fixture;
    fixture.provider->setNextInvocationState(
        atomic::ProviderInvocationState::Unknown);
    const auto invocation_count_before =
        fixture.provider->invocationCount();

    orch::DAGNode node;
    node.node_id = "unknown-atomic";
    node.executor = "atomic_service";
    node.action =
        "com_sgm_service_climate_setAutoFanSpeed";
    node.params =
        nlohmann::json{{"location", "FRONT"}, {"mode", "HIGH"}};
    node.base_priority = TaskPriority::P1;
    node.deadline_mono_ns = deadline(fixture.clock);
    node.max_attempts = 1;
    auto request = fixture.request(
        "unknown-no-retry", {node}, TaskPriority::P1);
    fixture.agent_service_call.request_id = request.dag.request_id;
    fixture.agent_service_call.trace_id = request.trace_id;
    const auto committed =
        fixture.orchestrator.submit(request, fixture.agent_service_call);
    expect(committed.accepted &&
               fixture.orchestrator
                   .runUntilPlanTerminal(committed.plan_id)
                   .ok,
           "UNKNOWN fixture plan must settle through reconciliation");
    const auto terminal = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(terminal.value &&
               terminal.value->state == orch::PlanState::Succeeded &&
               terminal.value->nodes.at("unknown-atomic").state ==
                   orch::ActivationState::Succeeded &&
               terminal.value->nodes.at("unknown-atomic")
                       .side_effect_state ==
                   SideEffectState::Committed &&
               terminal.value->nodes.at("unknown-atomic").attempt_count ==
                   1 &&
               fixture.provider->invocationCount() ==
                   invocation_count_before + 1 &&
               fixture.provider->reconciliationCount() == 1,
           "confirmed UNKNOWN must settle without creating a second "
           "Attempt");
    const auto events = fixture.orchestrator.events();
    expect(std::any_of(
               events.begin(), events.end(),
               [&committed](const auto& event) {
                   return event.plan_id == committed.plan_id &&
                          event.event_type == "NODE_RECONCILING";
               }) &&
               std::none_of(
                   events.begin(), events.end(),
                   [&committed](const auto& event) {
                       return event.plan_id == committed.plan_id &&
                              event.event_type == "NODE_RETRY_READY";
                   }),
           "UNKNOWN must enter RECONCILING and never masquerade as "
           "retry-ready");
}

void testOrchestratorKeepsStillUnknownIsolated() {
    OrchestratorFixture fixture;
    fixture.provider->setNextInvocationState(
        atomic::ProviderInvocationState::Unknown);
    fixture.provider->setUnknownReconcileStatus(
        atomic::ReconcileStatus::StillUnknown);
    orch::DAGNode node;
    node.node_id = "still-unknown-atomic";
    node.executor = "atomic_service";
    node.action =
        "com_sgm_service_climate_setAirCirculationMode";
    node.params = nlohmann::json{{"mode", "AUTO"}};
    node.base_priority = TaskPriority::P1;
    node.deadline_mono_ns = deadline(fixture.clock);
    node.max_attempts = 1;
    auto request = fixture.request(
        "still-unknown-isolated", {node}, TaskPriority::P1);
    fixture.agent_service_call.request_id = request.dag.request_id;
    fixture.agent_service_call.trace_id = request.trace_id;
    const auto committed =
        fixture.orchestrator.submit(request, fixture.agent_service_call);
    expect(committed.accepted,
           "STILL_UNKNOWN fixture plan must commit");

    bool entered_reconciling = false;
    for (std::size_t i = 0; i < 10; ++i) {
        expect(fixture.orchestrator.pumpOne(),
               "UNKNOWN execution must keep making bounded progress");
        const auto snapshot = fixture.orchestrator.getPlan(
            committed.plan_id, fixture.agent_service_call);
        if (snapshot.value &&
            snapshot.value->nodes.at("still-unknown-atomic").state ==
                orch::ActivationState::Reconciling) {
            entered_reconciling = true;
            break;
        }
    }
    expect(entered_reconciling,
           "UNKNOWN attempt must enter RECONCILING");
    expect(!fixture.orchestrator.pumpOne(),
           "STILL_UNKNOWN reconciliation must not report false state "
           "progress or spin the synchronous driver");
    const auto isolated = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(isolated.value &&
               isolated.value->state == orch::PlanState::Running &&
               isolated.value->nodes.at("still-unknown-atomic").state ==
                   orch::ActivationState::Reconciling &&
               isolated.value->nodes.at("still-unknown-atomic")
                       .attempt_count == 1 &&
               fixture.provider->invocationCount() == 1 &&
               fixture.provider->reconciliationCount() == 1,
           "STILL_UNKNOWN must remain isolated without success/failure "
           "branching or a new Attempt");
}

void testOrchestratorPropagatesP0PriorityToDependencyClosure() {
    OrchestratorFixture fixture;
    orch::DAGNode prerequisite;
    prerequisite.node_id = "p0-prerequisite";
    prerequisite.executor = "atomic_service";
    prerequisite.action =
        "com_sgm_service_climate_setAirCirculationMode";
    prerequisite.params = nlohmann::json{{"mode", "EXTERNAL"}};
    prerequisite.base_priority = TaskPriority::P2;
    prerequisite.deadline_mono_ns = deadline(fixture.clock);

    orch::DAGNode critical;
    critical.node_id = "p0-critical";
    critical.executor = "atomic_service";
    critical.action = "com_sgm_service_climate_setAutoFanSpeed";
    critical.params =
        nlohmann::json{{"location", "FRONT"}, {"mode", "HIGH"}};
    critical.dependencies = {"p0-prerequisite"};
    critical.base_priority = TaskPriority::P0;
    critical.deadline_mono_ns = deadline(fixture.clock);

    auto request = fixture.request(
        "p0-inheritance", {prerequisite, critical}, TaskPriority::P0);
    fixture.agent_service_call.request_id = request.dag.request_id;
    fixture.agent_service_call.trace_id = request.trace_id;
    fixture.agent_service_call.priority = TaskPriority::P0;
    const auto committed =
        fixture.orchestrator.submit(request, fixture.agent_service_call);
    expect(committed.accepted,
           "authorized P0 dependency DAG must commit");
    const auto snapshot = fixture.orchestrator.getPlan(
        committed.plan_id, fixture.agent_service_call);
    expect(snapshot.value &&
               snapshot.value->nodes.at("p0-prerequisite")
                       .effective_priority == TaskPriority::P0,
           "P0 dependent must donate priority to its prerequisite closure");
}

void testP0RequiresTrustedAuthorizationAcrossExecutionBoundaries() {
    AtomicFixture atomic_fixture;
    auto atomic_request = atomicEnvelope(
        atomic_fixture.catalog(), "execution-p0-denied",
        "operation-p0-denied", "idem-p0-denied", TaskPriority::P0, 1,
        "FRONT", "HIGH", deadline(atomic_fixture.clock));
    atomic_request.runtime.authorization_ref =
        "untrusted:p0-prefix-forgery";
    const auto atomic_denied = atomic_fixture.call(atomic_request);
    expect(!atomic_denied.accepted &&
               atomic_denied.reject_code ==
                   "ATOMIC_P0_AUTHORIZATION_REQUIRED",
           "AtomicService must reject forged P0 authorization");

    InferenceFixture inference_fixture;
    const auto inference_request = inferenceRequest(
        inference_fixture.clock, "p0-denied", TaskPriority::P0, false);
    inference_fixture.intent_call.request_id =
        inference_request.request_id;
    inference_fixture.intent_call.trace_id =
        inference_request.trace_id;
    inference_fixture.intent_call.priority = TaskPriority::P0;
    const auto inference_denied =
        inference_fixture.framework.submitInference(
            inference_request, inference_fixture.intent_call);
    expect(!inference_denied.accepted &&
               inference_denied.reject_code ==
                   "INFERENCE_P0_AUTHORIZATION_REQUIRED",
           "Inference must reject P0 without a trusted admission proof");

    auto dispatch_clock = std::make_shared<ManualRuntimeClock>();
    auto dispatch_ids =
        std::make_shared<IdGenerator>("resilience-p0-dispatch");
    dispatch::AgentDispatch scheduler(dispatch_clock, dispatch_ids);
    const auto target = manifest("p0-trip-agent", 1);
    auto agent = std::make_shared<sub::DeterministicSubAgent>(
        target, dispatch_clock, dispatch_ids, 1);
    CallContext bootstrap{CallerModuleId::AgentService, "bootstrap",
                          "trace-bootstrap", "principal-resilience",
                          TaskPriority::P1, deadline(dispatch_clock)};
    expect(scheduler.registerAgent(agent, bootstrap).ok,
           "P0 dispatch fixture Agent must register");
    auto dispatch_request = dispatchTask(
        dispatch_clock, target, "p0-denied", TaskPriority::P0);
    dispatch_request.authorization_ref = "untrusted:p0";
    bindCurrentDispatchCapacity(scheduler, dispatch_request);
    const auto dispatch_denied = scheduler.submitDispatch(
        dispatch_request, dispatchCall(dispatch_request));
    expect(!dispatch_denied.accepted &&
               dispatch_denied.reject_code ==
                   "DISPATCH_P0_AUTHORIZATION_REQUIRED",
           "AgentDispatch must reject forged P0 authorization");

    OrchestratorFixture orchestrator_fixture;
    orch::DAGNode node;
    node.node_id = "p0-denied";
    node.executor = "atomic_service";
    node.action =
        "com_sgm_service_climate_setAirCirculationMode";
    node.params = nlohmann::json{{"mode", "AUTO"}};
    node.base_priority = TaskPriority::P0;
    node.deadline_mono_ns = deadline(orchestrator_fixture.clock);
    auto plan_request = orchestrator_fixture.request(
        "p0-denied", {node}, TaskPriority::P0);
    plan_request.admission.p0_authorization = false;
    plan_request.admission.authorization_ref =
        "policy:" + plan_request.admission.policy_digest;
    orchestrator_fixture.agent_service_call.request_id =
        plan_request.dag.request_id;
    orchestrator_fixture.agent_service_call.trace_id =
        plan_request.trace_id;
    orchestrator_fixture.agent_service_call.priority = TaskPriority::P0;
    orchestrator_fixture.agent_service_call.authorization_ref =
        plan_request.admission.authorization_ref;
    const auto plan_denied = orchestrator_fixture.orchestrator.submit(
        plan_request, orchestrator_fixture.agent_service_call);
    expect(!plan_denied.accepted &&
               plan_denied.reject_code ==
                   "ORCHESTRATOR_P0_AUTHORIZATION_REQUIRED",
           "Orchestrator must reject a P0 DAG without trusted admission");
}

int runTests(
    const std::vector<std::pair<std::string, std::function<void()>>>& tests) {
    int failures = 0;
    for (const auto& test : tests) {
        std::cout << "[RUN ] " << test.first << std::endl;
        try {
            test.second();
            std::cout << "[PASS] " << test.first << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << error.what()
                      << std::endl;
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.first
                      << ": non-standard exception" << std::endl;
        }
    }
    std::cout << "Resilience cases: " << tests.size()
              << ", failures: " << failures << std::endl;
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() {
    return runTests({
        {"atomic duplicate execution identity",
         testAtomicRejectsDuplicateExecutionIdentity},
        {"atomic duplicate operation identity",
         testAtomicRejectsDuplicateOperationIdentity},
        {"atomic idempotency execution-context binding",
         testAtomicIdempotencyBindsExecutionContext},
        {"atomic execution-time fencing",
         testAtomicRechecksFenceImmediatelyBeforeSideEffect},
        {"atomic provider result after deadline",
         testAtomicQuarantinesProviderResultAfterDeadline},
        {"atomic P0 preemption and P2 restoration",
         testAtomicP0PreemptsAndP2ResumesAtSafePoint},
        {"atomic UNKNOWN reconciliation identity",
         testAtomicRejectsMismatchedUnknownReconciliationIdentity},
        {"atomic Provider invocation seal",
         testAtomicRejectsStaleProviderInvocationSeals},
        {"inference cancellation after immediate KV import",
         testInferenceCancelAfterImmediateKvImportStaysLeaseFree},
        {"inference preemption KV lease lifecycle",
         testInferencePreemptionDoesNotLeakOrDuplicateKvLease},
        {"inference prompt digest/idempotency binding",
         testInferenceBindsPromptToDigestAndIdempotency},
        {"dispatch provider epoch lease",
         testDispatchRejectsAgentEpochReplacementWhileLeased},
        {"dispatch provider result after deadline",
         testDispatchQuarantinesProviderResultAfterDeadline},
        {"dispatch idempotency conflicts",
         testDispatchIdempotencyConflicts},
        {"dispatch duplicate execution identity",
         testDispatchRejectsDuplicateExecutionIdentity},
        {"dispatch control epoch replay",
         testDispatchControlEpochReplayIsIdempotent},
        {"dispatch automatic P0 preemption and P2 restoration",
         testDispatchAutomaticallyPreemptsAndRestoresLowerPriorityWork},
        {"dispatch non-preemptible capacity honesty",
         testDispatchDoesNotOvercommitNonPreemptibleAgent},
        {"dispatch provider exception UNKNOWN lease",
         testDispatchProviderExceptionBecomesUnknownWithoutLeaseRelease},
        {"dispatch stale provider seals",
         testDispatchRejectsStaleProviderSeals},
        {"dispatch UNKNOWN sealed reconciliation",
         testDispatchReconcilesUnknownFromSealedTerminal},
        {"dispatch capacity generation and P0 reservation",
         testDispatchCapacityEpochAndReservedP0Credits},
        {"orchestrator dependency order and cycle rejection",
         testOrchestratorHonorsDependenciesAndRejectsCycles},
        {"orchestrator ready-node priority",
         testOrchestratorSelectsHighestPriorityReadyNode},
        {"orchestrator Agent capacity re-evaluation",
         testOrchestratorReevaluatesAgentCapacityWithoutAttempt},
        {"orchestrator idempotency dependency binding",
         testOrchestratorIdempotencyBindsDependencyGraph},
        {"orchestrator retry Admission contracts",
         testOrchestratorRejectsUntrustedRetryContracts},
        {"orchestrator retry eligibility signals",
         testOrchestratorRequiresEveryRetryEligibilitySignal},
        {"orchestrator absolute retry scheduling",
         testOrchestratorSchedulesAbsoluteAtomicRetry},
        {"orchestrator retry deadline bounds",
         testOrchestratorDoesNotRetryAtOrBeyondDeadline},
        {"orchestrator UNKNOWN reconciliation without retry",
         testOrchestratorReconcilesUnknownWithoutAutomaticRetry},
        {"orchestrator STILL_UNKNOWN isolation",
         testOrchestratorKeepsStillUnknownIsolated},
        {"orchestrator P0 dependency inheritance",
         testOrchestratorPropagatesP0PriorityToDependencyClosure},
        {"P0 trusted authorization negative matrix",
         testP0RequiresTrustedAuthorizationAcrossExecutionBoundaries},
    });
}
