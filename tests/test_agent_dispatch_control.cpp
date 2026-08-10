/**
 * @file test_agent_dispatch_control.cpp
 * @brief Verifies the durable Agent Dispatch registry and production control APIs.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace master_agent;
namespace dispatch = master_agent::agent_dispatch;
namespace sub_agents = master_agent::sub_agents;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
                ("master-agent-dispatch-control-" +
                 std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

sub_agents::AgentManifest manifest() {
    sub_agents::AgentManifest value;
    value.agent_id = "review-agent";
    value.agent_epoch = 1;
    value.capability_version = "1";
    value.capabilities = {"review.action"};
    value.max_concurrency = 1;
    value.supports_safe_point_preemption = true;
    value.manifest_digest = secureDigest(
        value.agent_id + "|" + value.capability_version + "|review.action");
    return value;
}

CallContext agentServiceCall(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    std::int64_t deadline) {
    return CallContext{
        CallerModuleId::AgentService, "registry-request",
        "registry-trace", "principal", TaskPriority::P1,
        deadline};
}

dispatch::DispatchTask task(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    std::uint64_t capacity_epoch) {
    dispatch::DispatchTask value;
    value.request_id = "request-control";
    value.plan_id = "plan-control";
    value.pid = "pid-control";
    value.activation_id = "activation-control";
    value.execution_id = "execution-control";
    value.operation_id = "operation-control";
    value.task_id = "task-control";
    value.action = "review.action";
    value.params = nlohmann::json{{"topic", "delivery"}};
    value.priority = TaskPriority::P2;
    value.deadline_mono_ns =
        clock->monotonicNowNs() + 30'000'000'000LL;
    value.idempotency_key = "dispatch-control-idempotency";
    value.fencing_token = 1;
    value.capability_digest = dispatch::dispatchCapabilityDigest(
        value.action, value.input_schema_version,
        value.expected_output_schema_version);
    value.capacity_epoch = capacity_epoch;
    value.principal_id_hash = "principal";
    value.authorization_ref = "user-interaction";
    value.trace_id = "trace-control";
    return value;
}

CallContext orchestratorCall(const dispatch::DispatchTask& value) {
    return CallContext{
        CallerModuleId::TaskOrchestrationEngine,
        value.request_id, value.trace_id, value.principal_id_hash,
        value.priority, value.deadline_mono_ns, {}, 0,
        value.authorization_ref};
}

void testDurableControlAndRegistrySurface() {
    ScopedTempDirectory temp;
    auto clock = std::make_shared<ManualRuntimeClock>();
    std::string dispatch_id;
    dispatch::DispatchTask submitted_task;
    CallContext submitted_call;
    std::uint64_t acknowledged_cursor = 0;

    {
        auto ids = std::make_shared<IdGenerator>("dispatch-control-first");
        dispatch::AgentDispatch manager(temp.path(), clock, ids);
        expect(manager.initialize().ok,
               "durable Agent Dispatch must initialize");
        const auto registration_deadline =
            clock->monotonicNowNs() + 30'000'000'000LL;
        auto provider = std::make_shared<sub_agents::DeterministicSubAgent>(
            manifest(), clock, ids, 2);
        expect(manager.registerAgent(
                   provider,
                   agentServiceCall(clock, registration_deadline))
                   .ok,
               "Agent registration must persist the manifest and epoch");

        const auto capability_call = agentServiceCall(
            clock, registration_deadline);
        const auto capabilities = manager.snapshotAgentCapabilities(
            {}, capability_call);
        expect(capabilities.value &&
                   capabilities.value->manifests.size() == 1 &&
                   !capabilities.value->snapshot_digest.empty(),
               "capability snapshot must be immutable and digest sealed");
        dispatch::AgentCapabilityQuery stale_query;
        stale_query.expected_snapshot_digest = "stale-digest";
        expect(!manager
                    .snapshotAgentCapabilities(stale_query, capability_call)
                    .status.ok,
               "an expected capability digest conflict must fail closed");

        submitted_task = task(
            clock, capabilities.value->registry_generation);
        submitted_call = orchestratorCall(submitted_task);
        const auto accepted = manager.submitDispatch(
            submitted_task, submitted_call);
        expect(accepted.accepted && !accepted.existing,
               "Dispatch admission must acknowledge ownership only");
        dispatch_id = accepted.dispatch_id;

        const auto p0_denied = manager.updateDispatchPriority(
            dispatch_id, TaskPriority::P0, "dependency:p0", 1,
            submitted_call);
        expect(!p0_denied.accepted &&
                   p0_denied.error_code ==
                       "DISPATCH_P0_AUTHORIZATION_REQUIRED",
               "control input must not manufacture P0 authority");
        const auto raised = manager.updateDispatchPriority(
            dispatch_id, TaskPriority::P1, "dependency:p1", 1,
            submitted_call);
        expect(raised.accepted && !raised.existing,
               "authorized priority inheritance must update queued work");

        dispatch::DispatchEventSubscription subscription;
        subscription.consumer_id = "orchestrator-consumer";
        subscription.consumer_epoch = 2;
        subscription.max_events = 128;
        const auto events = manager.subscribeEvents(
            subscription, submitted_call);
        expect(events.value && events.value->events.size() >= 3,
               "reliable events must include admission and control facts");
        acknowledged_cursor = events.value->next_cursor;
        expect(manager
                   .ackDispatchEvent(
                       {subscription.consumer_id,
                        subscription.consumer_epoch,
                        acknowledged_cursor},
                       submitted_call)
                   .ok,
               "event acknowledgement must persist monotonically");

        const auto cancelled = manager.cancelDispatch(
            dispatch_id, "TEST_CANCEL", 2,
            submitted_task.deadline_mono_ns, submitted_call);
        const auto terminal = manager.queryDispatch(
            dispatch_id, submitted_call);
        expect(cancelled.accepted && terminal.value &&
                   terminal.value->state == dispatch::DispatchState::Cancelled &&
                   terminal.value->side_effect_state ==
                       SideEffectState::ConfirmedNotExecuted,
               "queued cancellation must prove that no Provider ran");
        const auto capacity_events = manager.subscribeCapacity(
            {"capacity-consumer", 1, 0, 128}, submitted_call);
        expect(capacity_events.value &&
                   !capacity_events.value->events.empty(),
               "capacity generations must be observable");
        const auto health = manager.health(capability_call);
        expect(health.healthy && health.durable && health.accepting,
               "health must expose registry, durability, and admission state");
    }

    auto recovered_ids =
        std::make_shared<IdGenerator>("dispatch-control-recovered");
    dispatch::AgentDispatch recovered(temp.path(), clock, recovered_ids);
    expect(recovered.initialize().ok,
           "Agent Dispatch state must recover with checksum validation");
    auto recovered_provider =
        std::make_shared<sub_agents::DeterministicSubAgent>(
            manifest(), clock, recovered_ids, 1);
    const auto management_deadline =
        clock->monotonicNowNs() + 30'000'000'000LL;
    const auto management_call =
        agentServiceCall(clock, management_deadline);
    expect(recovered.registerAgent(
               recovered_provider, management_call)
               .ok,
           "same-epoch Provider must match the durable manifest");
    const auto terminal = recovered.queryDispatch(
        dispatch_id, submitted_call);
    expect(terminal.value &&
               terminal.value->state == dispatch::DispatchState::Cancelled,
           "terminal Dispatch ownership must survive restart");

    dispatch::DispatchEventSubscription stale_subscription;
    stale_subscription.consumer_id = "orchestrator-consumer";
    stale_subscription.consumer_epoch = 1;
    stale_subscription.cursor = acknowledged_cursor;
    expect(!recovered
                .subscribeEvents(stale_subscription, submitted_call)
                .status.ok,
           "a restarted consumer must not roll back its durable epoch");
    expect(recovered.unregisterAgent(
               manifest().agent_id, manifest().agent_epoch,
               management_deadline, management_call)
               .ok,
           "an idle Agent must drain and unregister cleanly");
    expect(recovered.drain(management_deadline, management_call).ok,
           "the control plane must close new admission during drain");
    auto rejected = submitted_task;
    rejected.operation_id = "operation-after-drain";
    rejected.execution_id = "execution-after-drain";
    rejected.task_id = "task-after-drain";
    rejected.idempotency_key = "idempotency-after-drain";
    const auto rejected_call = orchestratorCall(rejected);
    expect(!recovered.submitDispatch(rejected, rejected_call).accepted,
           "draining Dispatch must reject new work");
}

}  // namespace

int main() {
    try {
        testDurableControlAndRegistrySurface();
        std::cout << "agent dispatch control tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "agent dispatch control tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
