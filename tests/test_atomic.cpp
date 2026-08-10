/**
 * @file test_atomic.cpp
 * @brief Verifies MCP admission, priority scheduling, fencing, and reconciliation.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/sub_agents/sub_agent.h"
#include "test_support.h"

using namespace master_agent;
using namespace master_agent::atomic_service;
using master_agent::test_support::expect;
namespace agent_dispatch = master_agent::agent_dispatch;
namespace sub_agents = master_agent::sub_agents;

namespace {

AtomicMcpCallEnvelope envelope(
    const McpToolCatalogSnapshot& catalog, const std::string& execution,
    const std::string& operation, const std::string& idempotency,
    TaskPriority priority, std::uint64_t fencing,
    const std::string& location, const std::string& mode,
    std::int64_t deadline) {
    AtomicMcpCallEnvelope request;
    request.mcp_request.id = operation;
    request.mcp_request.name =
        "com_sgm_service_climate_setAutoFanSpeed";
    request.mcp_request.arguments =
        nlohmann::json{{"location", location}, {"mode", mode}};
    request.runtime.caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    request.runtime.request_id = "request-" + operation;
    request.runtime.trace_id = "trace-" + operation;
    request.runtime.plan_id = "plan-" + operation;
    request.runtime.pid = "pid-" + operation;
    request.runtime.activation_id = "activation-" + operation;
    request.runtime.execution_id = execution;
    request.runtime.operation_id = operation;
    request.runtime.priority = priority;
    request.runtime.deadline_mono_ns = deadline;
    request.runtime.idempotency_key = idempotency;
    request.runtime.fencing_token = fencing;
    request.runtime.tool_catalog_snapshot_id = catalog.snapshot_id;
    request.runtime.tool_digest =
        catalog.tool_digests.at(request.mcp_request.name);
    request.runtime.policy_digest =
        catalog.policy_digests.at(request.mcp_request.name);
    request.runtime.granted_permissions = {
        "vehicle.climate.write"};
    request.runtime.principal_id_hash = "principal";
    request.runtime.authorization_ref =
        priority == TaskPriority::P0 ? "trusted-safety:test-grant"
                                     : "auth-v1";
    return request;
}

void bindCall(CallContext& call,
              const AtomicMcpCallEnvelope& request) {
    call.request_id = request.runtime.request_id;
    call.trace_id = request.runtime.trace_id;
    call.principal_id_hash = request.runtime.principal_id_hash;
    call.priority = request.runtime.priority;
    call.deadline_mono_ns = request.runtime.deadline_mono_ns;
    call.authorization_ref =
        request.runtime.authorization_ref;
}

/// Build a child call whose trusted fields exactly echo a frozen, running
/// parent Dispatch/AgentLease. The child's execution/operation identities
/// remain independent, as required for a separately ledgered Tool effect.
AtomicMcpCallEnvelope childEnvelope(
    const McpToolCatalogSnapshot& catalog,
    const agent_dispatch::DispatchSnapshot& parent,
    const std::string& suffix) {
    auto child = envelope(
        catalog, "child-execution-" + suffix,
        "child-operation-" + suffix, "child-idem-" + suffix,
        parent.task.priority, 1, "FRONT", "NORMAL",
        parent.task.deadline_mono_ns - 1'000'000LL);
    child.runtime.caller_module_id = CallerModuleId::SubAgent;
    child.runtime.request_id = parent.task.request_id;
    child.runtime.trace_id = parent.task.trace_id;
    child.runtime.plan_id = parent.task.plan_id;
    child.runtime.pid = parent.task.pid;
    child.runtime.activation_id = parent.task.activation_id;
    child.runtime.principal_id_hash =
        parent.task.principal_id_hash;
    child.runtime.authorization_ref =
        parent.task.authorization_ref;
    child.runtime.granted_permissions = {
        "vehicle.climate.write"};
    child.runtime.parent_operation_id =
        parent.task.operation_id;
    child.runtime.parent_dispatch_id = parent.dispatch_id;
    child.runtime.parent_agent_id = parent.route.agent_id;
    child.runtime.parent_agent_epoch =
        parent.route.agent_epoch;
    child.runtime.parent_lease_id = parent.route.lease_id;
    child.runtime.parent_fencing_token =
        parent.task.fencing_token;
    child.runtime.fencing_token = parent.task.fencing_token;
    child.runtime.idempotency_key =
        agent_dispatch::atomicChildIdempotencyKey(
            parent.dispatch_id, child.runtime.operation_id);
    return child;
}

void expectLineageRejected(
    AtomicServiceManager& manager,
    const std::shared_ptr<DeterministicClimateProvider>& provider,
    const AtomicMcpCallEnvelope& request,
    const std::string& expected_code,
    const std::string& message) {
    CallContext call{
        CallerModuleId::SubAgent, request.runtime.request_id,
        request.runtime.trace_id,
        request.runtime.principal_id_hash,
        request.runtime.priority,
        request.runtime.deadline_mono_ns, {}, 0,
        request.runtime.authorization_ref};
    const auto before = provider->invocationCount();
    const auto rejected = manager.callTool(request, call);
    expect(!rejected.accepted &&
               rejected.reject_code == expected_code &&
               provider->invocationCount() == before,
           message + "; reject=" + rejected.reject_code);
}

/// A controllable Provider boundary used to hold the Atomic child invocation
/// reservation while AgentDispatch observes the parent Provider terminal.
class BlockingAtomicProvider final : public IAtomicProvider {
public:
    ProviderInvocationResult call(
        const AtomicMcpCallEnvelope& request,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++invocation_count_;
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return released_; });
        }

        CallToolResult result;
        result.structured_content =
            nlohmann::json{
                {"success", true},
                {"appliedLocation",
                 request.mcp_request.arguments.at("location")},
                {"appliedMode",
                 request.mcp_request.arguments.at("mode")},
                {"errorCode", ""}};
        result.text_content.push_back(
            result.structured_content.dump());

        ProviderInvocationResult invocation;
        invocation.state = ProviderInvocationState::Succeeded;
        invocation.result = std::move(result);
        invocation.side_effect_state = SideEffectState::Committed;
        invocation.completion_evidence =
            CompletionEvidence::StateVerified;
        invocation.invocation_seal = invocation_seal;
        return invocation;
    }

    AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& request,
        const AtomicProviderInvocationSeal& invocation_seal) override {
        AtomicReconcileResult result;
        result.operation_id = request.runtime.operation_id;
        result.execution_id = request.runtime.execution_id;
        result.tool_name = request.mcp_request.name;
        result.status = ReconcileStatus::StillUnknown;
        result.fencing_token = request.runtime.fencing_token;
        result.side_effect_state = SideEffectState::Unknown;
        result.invocation_seal = invocation_seal;
        return result;
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    std::size_t invocationCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return invocation_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
    std::size_t invocation_count_ = 0;
};

struct Fixture {
    std::shared_ptr<ManualRuntimeClock> clock =
        std::make_shared<ManualRuntimeClock>();
    std::shared_ptr<IdGenerator> ids =
        std::make_shared<IdGenerator>("atomic-test");
    std::shared_ptr<DeterministicClimateProvider> provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager manager{clock, ids, 1};
    CallContext bootstrap{CallerModuleId::AgentService,
                          "bootstrap", "trace", "system", TaskPriority::P1,
                          clock->monotonicNowNs() + 10000000000LL};

    Fixture() {
        expect(manager
                   .registerTools(defaultClimateMcpTools(),
                                  defaultClimateRuntimePolicies(4), provider,
                                  bootstrap)
                   .ok,
               "tool registration must succeed");
    }

    McpToolCatalogSnapshot catalog() {
        const auto result = manager.getToolCatalogSnapshot(bootstrap);
        expect(result.status.ok && result.value,
               "catalog snapshot must be available");
        return *result.value;
    }
};

class ReadOnlyAtomicProvider final : public IAtomicProvider {
public:
    ProviderInvocationResult call(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& seal) override {
        ++invocations_;
        ProviderInvocationResult result;
        result.invocation_seal = seal;
        if (envelope.mcp_request.method != "tools/call" ||
            envelope.mcp_request.name !=
                "com_sgm_service_vehicle_getRange") {
            result.state = ProviderInvocationState::Failed;
            result.error_code = "READ_PROVIDER_REQUEST_INVALID";
            result.side_effect_state =
                SideEffectState::NotApplicable;
            return result;
        }
        result.state = ProviderInvocationState::Succeeded;
        result.side_effect_state = SideEffectState::NotApplicable;
        result.completion_evidence =
            CompletionEvidence::ReturnConfirmed;
        result.result.structured_content = {
            {"range", 428},
            {"unit", envelope.mcp_request.arguments.at("unit")}};
        result.result.text_content.push_back(
            result.result.structured_content.dump());
        return result;
    }

    AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& seal) override {
        AtomicReconcileResult result;
        result.operation_id = envelope.runtime.operation_id;
        result.execution_id = envelope.runtime.execution_id;
        result.tool_name = envelope.mcp_request.name;
        result.status = ReconcileStatus::StillUnknown;
        result.side_effect_state = SideEffectState::NotApplicable;
        result.invocation_seal = seal;
        return result;
    }

    std::size_t invocations() const { return invocations_; }

private:
    std::size_t invocations_ = 0;
};

void testReadOnlyMcpQueryUsesRegisteredProvider() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("atomic-read-only");
    auto provider = std::make_shared<ReadOnlyAtomicProvider>();
    AtomicServiceManager manager(clock, ids, 1);

    McpToolDefinition definition;
    definition.name = "com_sgm_service_vehicle_getRange";
    definition.description = "Returns the current estimated vehicle range.";
    definition.input_schema = {
        {"type", "object"},
        {"properties",
         {{"unit",
           {{"type", "string"}, {"enum", {"KM", "MI"}}}}}},
        {"required", {"unit"}},
        {"additionalProperties", false}};
    definition.output_schema = {
        {"type", "object"},
        {"properties",
         {{"range", {{"type", "integer"}}},
          {"unit",
           {{"type", "string"}, {"enum", {"KM", "MI"}}}}}},
        {"required", {"range", "unit"}},
        {"additionalProperties", false}};
    definition.annotations.title = "getRange";
    definition.annotations.read_only_hint = true;
    definition.annotations.idempotent_hint = true;

    AtomicToolRuntimePolicy policy;
    policy.tool_name = definition.name;
    policy.idempotency_policy = "READ_ONLY";
    policy.completion_policy = CompletionPolicy::ReturnConfirmed;
    policy.supports_preemption = false;
    policy.supports_reconcile = false;

    const auto deadline =
        clock->monotonicNowNs() + 30'000'000'000LL;
    const CallContext registration{
        CallerModuleId::AgentService, "read-register",
        "read-trace", "system", TaskPriority::P1, deadline};
    expect(manager.registerTools(
               {definition}, {policy}, provider, registration)
               .ok,
           "READ_ONLY MCP Tool registration must bind schema and policy");
    const auto catalog = manager.getToolCatalogSnapshot(registration);
    expect(catalog.status.ok && catalog.value,
           "read-only query must use a frozen MCP catalog");

    AtomicReadOnlyMcpRequest request;
    request.mcp_request.jsonrpc = "2.0";
    request.mcp_request.id = "Q-range-1";
    request.mcp_request.method = "tools/call";
    request.mcp_request.name = definition.name;
    request.mcp_request.arguments = {{"unit", "KM"}};
    request.expected_catalog_digest =
        catalog.value->catalog_digest;
    const CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine,
        "read-request", "read-trace", "principal",
        TaskPriority::P1, deadline};
    const auto result = manager.queryReadOnly(request, intent_call);
    expect(result.status.ok && result.value &&
               result.value->tool_name == definition.name &&
               result.value->result.structured_content.at("range") == 428 &&
               provider->invocations() == 1,
           "queryReadOnly must execute one standard MCP tools/call Provider request");

    request.mcp_request.method = "custom/call";
    const auto non_mcp = manager.queryReadOnly(request, intent_call);
    expect(!non_mcp.status.ok && provider->invocations() == 1,
           "non-MCP invocation formats must be rejected before Provider I/O");
}

void testMcpShapeAndCallerAllowlist() {
    Fixture fixture;
    CallContext skill_call{CallerModuleId::SkillEngine, "r", "t", {},
                           TaskPriority::P1,
                           fixture.clock->monotonicNowNs() + 1000000000LL};
    const auto tools = fixture.manager.listTools(skill_call);
    expect(tools.status.ok && tools.value && tools.value->size() == 2,
           "MCP tools/list must return two tools; status=" +
               tools.status.error.code + " size=" +
               std::to_string(tools.value ? tools.value->size() : 0));
    const auto& schema = tools.value->at(1).input_schema;
    expect(schema.at("type") == "object" &&
               schema.at("required").size() == 2 &&
               schema.at("additionalProperties") == false,
           "MCP inputSchema must be strict JSON Schema");

    const auto request = envelope(
        fixture.catalog(), "e-denied", "o-denied", "idem-denied",
        TaskPriority::P1, 1, "FRONT", "NORMAL",
        fixture.clock->monotonicNowNs() + 1000000000LL);
    const auto denied =
        fixture.manager.callTool(request, fixture.bootstrap);
    expect(!denied.accepted &&
               denied.reject_code == "ATOMIC_CALLER_MODULE_NOT_ALLOWED",
           "AgentService must not call provider-facing tools/call");
}

void testPriorityAndSafePointPreemption() {
    Fixture fixture;
    const auto catalog = fixture.catalog();
    CallContext orchestrator_call{
        CallerModuleId::TaskOrchestrationEngine, "r", "t", "principal",
        TaskPriority::P1,
        fixture.clock->monotonicNowNs() + 10000000000LL};
    const auto low = envelope(
        catalog, "execution-low", "operation-low", "idem-low",
        TaskPriority::P2, 1, "FRONT", "LOW",
        fixture.clock->monotonicNowNs() + 9000000000LL);
    bindCall(orchestrator_call, low);
    expect(fixture.manager.callTool(low, orchestrator_call).accepted,
           "P2 operation must be accepted");
    expect(fixture.manager.pumpOne(), "P2 must start");
    const auto high = envelope(
        catalog, "execution-high", "operation-high", "idem-high",
        TaskPriority::P0, 1, "REAR", "HIGH",
        fixture.clock->monotonicNowNs() + 5000000000LL);
    bindCall(orchestrator_call, high);
    expect(fixture.manager.callTool(high, orchestrator_call).accepted,
           "P0 operation must be accepted");
    expect(fixture.manager.runUntilIdle().ok,
           "preempted operations must settle");
    auto low_query = orchestrator_call;
    bindCall(low_query, low);
    auto high_query = orchestrator_call;
    bindCall(high_query, high);
    const auto low_state =
        fixture.manager.queryExecution("execution-low", low_query);
    const auto high_state =
        fixture.manager.queryExecution("execution-high", high_query);
    expect(low_state.value &&
               low_state.value->state == AtomicExecutionState::Succeeded &&
               high_state.value &&
               high_state.value->state == AtomicExecutionState::Succeeded,
           "P0 and resumed P2 operations must both succeed");
    const auto events = fixture.manager.events();
    expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
               return event.execution_id == "execution-low" &&
                      event.event_type == "SUSPENDED" && event.safe_point &&
                      event.resource_released;
           }),
           "P2 must be suspended only at a safe point");
}

void testIdempotencyFencingAndUnknown() {
    Fixture fixture;
    const auto catalog = fixture.catalog();
    CallContext call{CallerModuleId::TaskOrchestrationEngine, "r", "t",
                     "principal", TaskPriority::P1,
                     fixture.clock->monotonicNowNs() + 10000000000LL};
    const auto first = envelope(
        catalog, "execution-first", "operation-first", "idem-first",
        TaskPriority::P1, 10, "FRONT", "NORMAL",
        fixture.clock->monotonicNowNs() + 5000000000LL);
    bindCall(call, first);
    const auto accepted = fixture.manager.callTool(first, call);
    expect(accepted.accepted, "first fenced call must be accepted");
    const auto replay = fixture.manager.callTool(first, call);
    expect(replay.accepted && replay.existing,
           "same idempotency key and digest must replay");
    expect(fixture.manager.runUntilIdle().ok, "first call must settle");

    const auto stale = envelope(
        catalog, "execution-stale", "operation-stale", "idem-stale",
        TaskPriority::P1, 9, "FRONT", "HIGH",
        fixture.clock->monotonicNowNs() + 5000000000LL);
    bindCall(call, stale);
    const auto stale_result = fixture.manager.callTool(stale, call);
    expect(!stale_result.accepted &&
               stale_result.reject_code == "ATOMIC_STALE_FENCING_TOKEN",
           "stale fencing token must be rejected before side effects");

    fixture.provider->setNextInvocationState(
        ProviderInvocationState::Unknown);
    const auto unknown = envelope(
        catalog, "execution-unknown", "operation-unknown", "idem-unknown",
        TaskPriority::P1, 1, "REAR", "NORMAL",
        fixture.clock->monotonicNowNs() + 5000000000LL);
    bindCall(call, unknown);
    expect(fixture.manager.callTool(unknown, call).accepted,
           "unknown test call must be accepted");
    expect(fixture.manager.runUntilIdle().ok,
           "unknown physical invocation must leave worker idle");
    expect(fixture.provider->invocationCount() == 2,
           "UNKNOWN response loss must not trigger an automatic retry");
    const auto unknown_state =
        fixture.manager.queryExecution("execution-unknown", call);
    expect(unknown_state.value &&
               unknown_state.value->state == AtomicExecutionState::Unknown &&
               unknown_state.value->side_effect_state ==
                   SideEffectState::Unknown,
           "response loss must remain explicit UNKNOWN");
    const auto reconciled =
        fixture.manager.reconcileExecution("operation-unknown", call);
    expect(reconciled.status.ok && reconciled.value &&
               reconciled.value->status ==
                   ReconcileStatus::ConfirmedSuccess,
           "UNKNOWN must settle only through reconciliation");
    const auto settled =
        fixture.manager.queryExecution("execution-unknown", call);
    expect(settled.status.ok && settled.value &&
               settled.value->state ==
                   AtomicExecutionState::Succeeded &&
               settled.value->side_effect_state ==
                   SideEffectState::Committed &&
               settled.value->result &&
               settled.value->result->structured_content.at("success") ==
                   true &&
               fixture.provider->invocationCount() == 2 &&
               fixture.provider->reconciliationCount() == 1,
           "reconciliation must commit the bound result without replaying "
           "the physical side effect");
}

void testSubAgentCallFailsClosedWithoutLineageValidator() {
    Fixture fixture;
    auto child = envelope(
        fixture.catalog(), "child-no-validator-execution",
        "child-no-validator-operation",
        "child-no-validator-idem", TaskPriority::P1, 1,
        "FRONT", "NORMAL",
        fixture.clock->monotonicNowNs() +
            5'000'000'000LL);
    child.runtime.caller_module_id = CallerModuleId::SubAgent;
    child.runtime.parent_operation_id =
        "unverifiable-parent-operation";
    child.runtime.parent_dispatch_id =
        "unverifiable-parent-dispatch";
    child.runtime.parent_agent_id = "unverifiable-agent";
    child.runtime.parent_agent_epoch = 1;
    child.runtime.parent_lease_id = "unverifiable-lease";
    child.runtime.parent_fencing_token = 1;

    expectLineageRejected(
        fixture.manager, fixture.provider, child,
        "ATOMIC_PARENT_LINEAGE_VALIDATOR_UNAVAILABLE",
        "SubAgent tools/call must fail closed without a parent validator");
    expect(fixture.provider->invocationCount() == 0U,
           "missing lineage validator must fence the Provider boundary");
}

/// Establish one real Running parent through AgentDispatch, admit exactly
/// one correctly bound child Tool, then mutate every security-sensitive
/// lineage dimension independently.
void testSubAgentParentLineageSecurityMatrix() {
    const std::string child_tool =
        "com_sgm_service_climate_setAutoFanSpeed";
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "atomic-parent-lineage");
    auto dispatch =
        std::make_shared<agent_dispatch::AgentDispatch>(
            clock, ids);

    sub_agents::AgentManifest manifest;
    manifest.agent_id = "lineage-agent";
    manifest.agent_epoch = 7;
    manifest.capability_version = "1";
    manifest.capabilities = {"parent.workflow"};
    manifest.max_concurrency = 1;
    auto sub_agent =
        std::make_shared<sub_agents::DeterministicSubAgent>(
            manifest, clock, ids, 16);
    const auto parent_deadline =
        clock->monotonicNowNs() + 10'000'000'000LL;
    CallContext register_call{
        CallerModuleId::AgentService, "register-lineage-agent",
        "trace-register-lineage", "system", TaskPriority::P1,
        parent_deadline};
    expect(dispatch->registerAgent(sub_agent, register_call).ok,
           "lineage test Agent must register");

    CallContext capacity_call{
        CallerModuleId::TaskOrchestrationEngine,
        "lineage-capacity", "trace-lineage-capacity",
        "parent-principal", TaskPriority::P1,
        parent_deadline};
    const auto capacity =
        dispatch->getCapacity(capacity_call);
    expect(capacity.health_state == "READY" &&
               capacity.available_credits == 1U,
           "registered lineage Agent must publish one credit");

    agent_dispatch::DispatchTask parent_task;
    parent_task.request_id = "parent-request";
    parent_task.plan_id = "parent-plan";
    parent_task.pid = "parent-pid";
    parent_task.activation_id = "parent-activation";
    parent_task.execution_id = "parent-execution";
    parent_task.operation_id = "parent-operation";
    parent_task.task_id = "parent-task";
    parent_task.action = "parent.workflow";
    parent_task.target_agent = manifest.agent_id;
    parent_task.params =
        nlohmann::json{{"goal", "exercise child lineage"}};
    parent_task.priority = TaskPriority::P1;
    parent_task.deadline_mono_ns = parent_deadline;
    parent_task.idempotency_key = "parent-idempotency";
    parent_task.fencing_token = 77;
    parent_task.capability_digest =
        agent_dispatch::dispatchCapabilityDigest(
            parent_task.action,
            parent_task.input_schema_version,
            parent_task.expected_output_schema_version);
    parent_task.capacity_epoch = capacity.capacity_epoch;
    parent_task.granted_permissions = {
        "vehicle.climate.write", "parent.telemetry.read"};
    parent_task.allowed_child_capabilities = {child_tool};
    parent_task.principal_id_hash = "parent-principal";
    parent_task.authorization_ref = "parent-auth-v1";
    parent_task.trace_id = "trace-parent";
    CallContext parent_call{
        CallerModuleId::TaskOrchestrationEngine,
        parent_task.request_id, parent_task.trace_id,
        parent_task.principal_id_hash, parent_task.priority,
        parent_task.deadline_mono_ns, {}, 0,
        parent_task.authorization_ref};
    const auto parent_acceptance =
        dispatch->submitDispatch(parent_task, parent_call);
    expect(parent_acceptance.accepted &&
               !parent_acceptance.dispatch_id.empty(),
           "parent Dispatch must be accepted");
    expect(dispatch->pumpOne(),
           "parent Dispatch must cross the Provider boundary");
    const auto parent_result = dispatch->queryDispatch(
        parent_acceptance.dispatch_id, parent_call);
    expect(parent_result.status.ok && parent_result.value &&
               parent_result.value->state ==
                   agent_dispatch::DispatchState::Running &&
               parent_result.value->route.agent_id ==
                   manifest.agent_id &&
               parent_result.value->route.agent_epoch ==
                   manifest.agent_epoch &&
               !parent_result.value->route.lease_id.empty(),
           "parent must own a Running AgentLease before child admission");
    const auto parent = *parent_result.value;

    auto provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager manager(clock, ids, 1, dispatch);
    CallContext bootstrap{
        CallerModuleId::AgentService, "atomic-lineage-bootstrap",
        "trace-atomic-lineage", "system", TaskPriority::P1,
        parent_deadline};
    expect(manager
               .registerTools(defaultClimateMcpTools(),
                              defaultClimateRuntimePolicies(1),
                              provider, bootstrap)
               .ok,
           "lineage Atomic catalog must register");
    const auto catalog_result =
        manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog_result.status.ok && catalog_result.value,
           "lineage Atomic catalog must be available");
    const auto catalog = *catalog_result.value;

    const auto valid =
        childEnvelope(catalog, parent, "valid");
    expect(
        valid.runtime.fencing_token ==
                parent.task.fencing_token &&
            valid.runtime.idempotency_key ==
                agent_dispatch::atomicChildIdempotencyKey(
                    parent.dispatch_id,
                    valid.runtime.operation_id),
        "valid child must inherit the parent task fence and derive its "
        "idempotency key from parent dispatch plus child operation");
    CallContext valid_call{
        CallerModuleId::SubAgent, valid.runtime.request_id,
        valid.runtime.trace_id, valid.runtime.principal_id_hash,
        valid.runtime.priority, valid.runtime.deadline_mono_ns,
        {}, 0, valid.runtime.authorization_ref};
    const auto valid_acceptance =
        manager.callTool(valid, valid_call);
    expect(valid_acceptance.accepted &&
               manager.runUntilIdle().ok &&
               provider->invocationCount() == 1U,
           "exact parent lineage, permission subset and allowed Tool "
           "must admit one physical invocation");

    // Exercise the same intended execution identity with forged security
    // fields first. A fail-closed lineage rejection must occur before the
    // Atomic fencing/idempotency ledgers, otherwise the later legitimate
    // child would be poisoned as stale or conflicting.
    const auto poison_resistant =
        childEnvelope(catalog, parent, "poison-resistant");
    auto forged_max_fence = poison_resistant;
    forged_max_fence.runtime.fencing_token =
        std::numeric_limits<std::uint64_t>::max();
    expectLineageRejected(
        manager, provider, forged_max_fence,
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "UINT64_MAX child fence must be rejected before Provider and "
        "fencing ledger mutation");

    auto forged_non_parent_fence = poison_resistant;
    forged_non_parent_fence.runtime.fencing_token =
        parent.task.fencing_token + 1U;
    expectLineageRejected(
        manager, provider, forged_non_parent_fence,
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "non-parent child fence must be rejected before Provider and "
        "fencing ledger mutation");

    auto forged_idempotency = poison_resistant;
    forged_idempotency.runtime.idempotency_key =
        "forged-child-idempotency";
    expectLineageRejected(
        manager, provider, forged_idempotency,
        "ATOMIC_CHILD_IDEMPOTENCY_KEY_INVALID",
        "non-derived child idempotency key must be rejected before "
        "Provider and idempotency ledger mutation");

    CallContext poison_resistant_call{
        CallerModuleId::SubAgent,
        poison_resistant.runtime.request_id,
        poison_resistant.runtime.trace_id,
        poison_resistant.runtime.principal_id_hash,
        poison_resistant.runtime.priority,
        poison_resistant.runtime.deadline_mono_ns,
        {},
        0,
        poison_resistant.runtime.authorization_ref};
    const auto poison_resistant_acceptance =
        manager.callTool(poison_resistant, poison_resistant_call);
    expect(
        poison_resistant_acceptance.accepted &&
            !poison_resistant_acceptance.existing &&
            manager.runUntilIdle().ok &&
            provider->invocationCount() == 2U,
        "forged child calls must not poison the subsequent legitimate "
        "execution or its one physical Provider invocation");

    const auto reject =
        [&](const std::string& suffix, const auto& mutate,
            const std::string& expected_code,
            const std::string& message) {
            auto candidate =
                childEnvelope(catalog, parent, suffix);
            mutate(candidate);
            expectLineageRejected(
                manager, provider, candidate, expected_code,
                message);
        };

    reject(
        "forged-operation",
        [](auto& child) {
            child.runtime.parent_operation_id =
                "forged-parent-operation";
        },
        "ATOMIC_PARENT_OPERATION_NOT_FOUND",
        "forged parent operation must be rejected");
    reject(
        "forged-dispatch",
        [](auto& child) {
            child.runtime.parent_dispatch_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent dispatch ID must be rejected");
    reject(
        "forged-agent",
        [](auto& child) {
            child.runtime.parent_agent_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent Agent ID must be rejected");
    reject(
        "forged-agent-epoch",
        [](auto& child) {
            ++child.runtime.parent_agent_epoch;
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent Agent epoch must be rejected");
    reject(
        "forged-lease",
        [](auto& child) {
            child.runtime.parent_lease_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent AgentLease must be rejected");
    reject(
        "forged-fence",
        [](auto& child) {
            ++child.runtime.parent_fencing_token;
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent fencing token must be rejected");
    reject(
        "forged-request",
        [](auto& child) {
            child.runtime.request_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent request identity must be rejected");
    reject(
        "forged-trace",
        [](auto& child) {
            child.runtime.trace_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent trace identity must be rejected");
    reject(
        "forged-plan",
        [](auto& child) {
            child.runtime.plan_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent Plan identity must be rejected");
    reject(
        "forged-pid",
        [](auto& child) {
            child.runtime.pid += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent PID must be rejected");
    reject(
        "forged-activation",
        [](auto& child) {
            child.runtime.activation_id += "-forged";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent Activation must be rejected");
    reject(
        "forged-principal",
        [](auto& child) {
            child.runtime.principal_id_hash =
                "forged-principal";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent principal must be rejected");
    reject(
        "forged-authorization",
        [](auto& child) {
            child.runtime.authorization_ref =
                "forged-authorization";
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "forged parent authorization must be rejected");
    reject(
        "raised-priority",
        [](auto& child) {
            child.runtime.priority = TaskPriority::P0;
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "child must not raise the parent priority");
    reject(
        "extended-deadline",
        [&parent](auto& child) {
            child.runtime.deadline_mono_ns =
                parent.task.deadline_mono_ns + 1;
        },
        "ATOMIC_PARENT_LINEAGE_MISMATCH",
        "child must not extend the parent deadline");
    reject(
        "permission-expansion",
        [](auto& child) {
            child.runtime.granted_permissions.push_back(
                "vehicle.admin");
        },
        "ATOMIC_CHILD_PERMISSION_ESCALATION",
        "child permissions must be a parent subset");
    reject(
        "permission-missing",
        [](auto& child) {
            child.runtime.granted_permissions.clear();
        },
        "ATOMIC_PERMISSION_DENIED",
        "child still needs every Tool permission");
    reject(
        "capability-not-allowed",
        [](auto& child) {
            child.mcp_request.name =
                "com_sgm_service_climate_setAirCirculationMode";
        },
        "ATOMIC_CHILD_CAPABILITY_NOT_AUTHORIZED",
        "parent must explicitly allow the child Tool capability");
    reject(
        "resource-lease-expansion",
        [](auto& child) {
            child.runtime.resource_lease_refs = {
                "lease:forged-parent-resource"};
        },
        "ATOMIC_CHILD_RESOURCE_LEASE_ESCALATION",
        "child resource leases must be a parent subset");

    expect(dispatch->runUntilIdle().ok,
           "parent Dispatch must become terminal for the final check");
    const auto terminal_parent = dispatch->queryDispatch(
        parent_acceptance.dispatch_id, parent_call);
    expect(terminal_parent.value &&
               terminal_parent.value->state ==
                   agent_dispatch::DispatchState::Succeeded,
           "parent Provider must prove a non-Running terminal state");
    const auto after_parent =
        childEnvelope(catalog, parent, "parent-terminal");
    expectLineageRejected(
        manager, provider, after_parent,
        "ATOMIC_PARENT_DISPATCH_NOT_RUNNING",
        "terminal parent must not authorize new child side effects");
    expect(provider->invocationCount() == 2U,
           "all lineage negatives must remain before Provider invocation");
}

/// Hold a child Atomic Provider call across the parent Agent's terminal
/// observation. The invocation reservation must pin the AgentLease: control
/// is rejected while the child is active and the parent terminal publication
/// is delayed until the physical child boundary returns.
void testAtomicChildInvocationPinsParentDispatch() {
    const std::string child_tool =
        "com_sgm_service_climate_setAutoFanSpeed";
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "atomic-child-parent-pin");
    auto dispatch =
        std::make_shared<agent_dispatch::AgentDispatch>(
            clock, ids);

    sub_agents::AgentManifest manifest;
    manifest.agent_id = "parent-pin-agent";
    manifest.agent_epoch = 11;
    manifest.capability_version = "1";
    manifest.capabilities = {"parent.pin.workflow"};
    manifest.max_concurrency = 1;
    auto sub_agent =
        std::make_shared<sub_agents::DeterministicSubAgent>(
            manifest, clock, ids, 1);
    const auto deadline =
        clock->monotonicNowNs() + 15'000'000'000LL;
    CallContext register_call{
        CallerModuleId::AgentService,
        "register-parent-pin-agent",
        "trace-register-parent-pin-agent",
        "system",
        TaskPriority::P1,
        deadline};
    expect(dispatch->registerAgent(sub_agent, register_call).ok,
           "parent-pin Agent must register");

    CallContext capacity_call{
        CallerModuleId::TaskOrchestrationEngine,
        "parent-pin-capacity",
        "trace-parent-pin-capacity",
        "parent-pin-principal",
        TaskPriority::P1,
        deadline};
    const auto capacity = dispatch->getCapacity(capacity_call);
    expect(capacity.health_state == "READY" &&
               capacity.available_credits == 1U,
           "parent-pin Agent must expose one available credit");

    agent_dispatch::DispatchTask parent_task;
    parent_task.request_id = "parent-pin-request";
    parent_task.plan_id = "parent-pin-plan";
    parent_task.pid = "parent-pin-pid";
    parent_task.activation_id = "parent-pin-activation";
    parent_task.execution_id = "parent-pin-execution";
    parent_task.operation_id = "parent-pin-operation";
    parent_task.task_id = "parent-pin-task";
    parent_task.action = "parent.pin.workflow";
    parent_task.target_agent = manifest.agent_id;
    parent_task.params =
        nlohmann::json{{"goal", "hold child invocation"}};
    parent_task.priority = TaskPriority::P1;
    parent_task.deadline_mono_ns = deadline;
    parent_task.idempotency_key = "parent-pin-idempotency";
    parent_task.fencing_token = 91;
    parent_task.capability_digest =
        agent_dispatch::dispatchCapabilityDigest(
            parent_task.action,
            parent_task.input_schema_version,
            parent_task.expected_output_schema_version);
    parent_task.capacity_epoch = capacity.capacity_epoch;
    parent_task.granted_permissions = {
        "vehicle.climate.write"};
    parent_task.allowed_child_capabilities = {child_tool};
    parent_task.principal_id_hash = "parent-pin-principal";
    parent_task.authorization_ref = "parent-pin-auth";
    parent_task.trace_id = "trace-parent-pin";
    CallContext parent_call{
        CallerModuleId::TaskOrchestrationEngine,
        parent_task.request_id,
        parent_task.trace_id,
        parent_task.principal_id_hash,
        parent_task.priority,
        parent_task.deadline_mono_ns,
        {},
        0,
        parent_task.authorization_ref};
    const auto parent_acceptance =
        dispatch->submitDispatch(parent_task, parent_call);
    expect(parent_acceptance.accepted &&
               dispatch->pumpOne(),
           "parent-pin Dispatch must be submitted and started");
    const auto running_parent = dispatch->queryDispatch(
        parent_acceptance.dispatch_id, parent_call);
    expect(running_parent.status.ok &&
               running_parent.value &&
               running_parent.value->state ==
                   agent_dispatch::DispatchState::Running,
           "parent-pin Dispatch must be Running before child admission");
    const auto parent = *running_parent.value;

    auto blocking_provider =
        std::make_shared<BlockingAtomicProvider>();
    AtomicServiceManager manager(
        clock, ids, 1, dispatch);
    CallContext bootstrap{
        CallerModuleId::AgentService,
        "parent-pin-atomic-bootstrap",
        "trace-parent-pin-atomic-bootstrap",
        "system",
        TaskPriority::P1,
        deadline};
    expect(manager
               .registerTools(defaultClimateMcpTools(),
                              defaultClimateRuntimePolicies(1),
                              blocking_provider, bootstrap)
               .ok,
           "parent-pin Atomic catalog must register");
    const auto catalog_result =
        manager.getToolCatalogSnapshot(bootstrap);
    expect(catalog_result.status.ok && catalog_result.value,
           "parent-pin Atomic catalog must be available");

    const auto child =
        childEnvelope(*catalog_result.value, parent, "parent-pin");
    CallContext child_call{
        CallerModuleId::SubAgent,
        child.runtime.request_id,
        child.runtime.trace_id,
        child.runtime.principal_id_hash,
        child.runtime.priority,
        child.runtime.deadline_mono_ns,
        {},
        0,
        child.runtime.authorization_ref};
    const auto child_acceptance =
        manager.callTool(child, child_call);
    expect(child_acceptance.accepted,
           "parent-pin child invocation must be admitted");

    auto atomic_run = std::async(
        std::launch::async,
        [&manager] { return manager.runUntilIdle(); });
    const bool child_entered =
        blocking_provider->waitUntilEntered(
            std::chrono::milliseconds(2000));

    Status preempt_status = Status::Error(
        "test", "CHILD_PROVIDER_NOT_ENTERED",
        "blocking child Provider was not entered");
    bool parent_provider_pumped = false;
    Result<agent_dispatch::DispatchSnapshot>
        parent_during_child;
    std::size_t succeeded_events_during_child = 0;
    if (child_entered) {
        CallContext preempt_call{
            CallerModuleId::TaskOrchestrationEngine,
            "parent-pin-preempt",
            "trace-parent-pin-preempt",
            "safety-principal",
            TaskPriority::P0,
            deadline,
            {},
            0,
            "trusted-safety:parent-pin"};
        preempt_status = dispatch->requestPreempt(
            parent.dispatch_id, TaskPriority::P0, 1,
            preempt_call);

        // work_units=1: this pump makes the SubAgent Provider report
        // Succeeded. AgentDispatch must retain Running until the child lease
        // is released.
        parent_provider_pumped = dispatch->pumpOne();
        parent_during_child = dispatch->queryDispatch(
            parent.dispatch_id, parent_call);
        const auto during_events = dispatch->events();
        succeeded_events_during_child =
            static_cast<std::size_t>(std::count_if(
                during_events.begin(), during_events.end(),
                [&parent](const auto& event) {
                    return event.dispatch_id ==
                               parent.dispatch_id &&
                           event.event_type == "SUCCEEDED";
                }));
    }

    // Always release before asserting so a failed synchronization assertion
    // cannot strand the worker future at the controlled Provider boundary.
    blocking_provider->release();
    const auto atomic_status = atomic_run.get();
    const auto parent_after_child = dispatch->queryDispatch(
        parent.dispatch_id, parent_call);
    const auto child_after_release = manager.queryExecution(
        child.runtime.execution_id, parent_call);
    const auto after_events = dispatch->events();
    const auto succeeded_events_after_child =
        static_cast<std::size_t>(std::count_if(
            after_events.begin(), after_events.end(),
            [&parent](const auto& event) {
                return event.dispatch_id == parent.dispatch_id &&
                       event.event_type == "SUCCEEDED";
            }));

    expect(child_entered &&
               blocking_provider->invocationCount() == 1U,
           "blocking child Provider must be entered exactly once");
    expect(!preempt_status.ok &&
               preempt_status.error.code ==
                   "DISPATCH_CHILD_INVOCATION_ACTIVE",
           "active child invocation must reject parent preemption with "
           "DISPATCH_CHILD_INVOCATION_ACTIVE");
    expect(parent_provider_pumped &&
               parent_during_child.status.ok &&
               parent_during_child.value &&
               parent_during_child.value->state ==
                   agent_dispatch::DispatchState::Running &&
               succeeded_events_during_child == 0U,
           "parent terminal observation must remain unpublished while the "
           "child Provider invocation owns its reservation");
    expect(atomic_status.ok &&
               child_after_release.status.ok &&
               child_after_release.value &&
               child_after_release.value->state ==
                   AtomicExecutionState::Succeeded,
           "released child Provider must settle its Atomic execution; "
           "run=" +
               atomic_status.error.code + " query=" +
               child_after_release.status.error.code + " state=" +
               (child_after_release.value
                    ? std::to_string(static_cast<unsigned>(
                          child_after_release.value->state))
                    : "missing") +
               " error=" +
               (child_after_release.value
                    ? child_after_release.value->error_code
                    : "missing"));
    expect(parent_after_child.status.ok &&
               parent_after_child.value &&
               parent_after_child.value->state ==
                   agent_dispatch::DispatchState::Succeeded &&
               succeeded_events_after_child == 1U,
           "child release must publish the deferred parent terminal exactly "
           "once without another Dispatch pump");
}

}  // namespace

int main() {
    testReadOnlyMcpQueryUsesRegisteredProvider();
    testMcpShapeAndCallerAllowlist();
    testPriorityAndSafePointPreemption();
    testIdempotencyFencingAndUnknown();
    testSubAgentCallFailsClosedWithoutLineageValidator();
    testSubAgentParentLineageSecurityMatrix();
    testAtomicChildInvocationPinsParentDispatch();
    return 0;
}
