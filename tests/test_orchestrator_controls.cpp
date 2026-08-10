/**
 * @file test_orchestrator_controls.cpp
 * @brief Verifies Trigger, Join, binding, control, and reliable event contracts.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/orchestrator/orchestrator.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace master_agent;
namespace atomic = master_agent::atomic_service;
namespace dispatch = master_agent::agent_dispatch;
namespace orch = master_agent::orchestrator;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::shared_ptr<ManualRuntimeClock> clock =
        std::make_shared<ManualRuntimeClock>();
    std::shared_ptr<IdGenerator> ids =
        std::make_shared<IdGenerator>("orchestrator-control");
    std::shared_ptr<dispatch::AgentDispatch> dispatch_manager =
        std::make_shared<dispatch::AgentDispatch>(clock, ids);
    std::shared_ptr<atomic::AtomicServiceManager> atomic_manager =
        std::make_shared<atomic::AtomicServiceManager>(
            clock, ids, 2, dispatch_manager);
    std::shared_ptr<atomic::DeterministicClimateProvider> provider =
        std::make_shared<atomic::DeterministicClimateProvider>();
    std::shared_ptr<orch::Orchestrator> orchestrator;

    Fixture() {
        const auto status = atomic_manager->registerTools(
            atomic::defaultClimateMcpTools(),
            atomic::defaultClimateRuntimePolicies(2),
            provider, bootstrapCall());
        expect(status.ok, "Atomic catalog registration failed");
        orchestrator = std::make_shared<orch::Orchestrator>(
            clock, ids, atomic_manager, dispatch_manager);
    }

    CallContext bootstrapCall() const {
        return CallContext{
            CallerModuleId::AgentService, "bootstrap",
            "trace-bootstrap", "system", TaskPriority::P1,
            clock->monotonicNowNs() + 60'000'000'000LL};
    }

    orch::OrchestratorSubmitRequest request(
        const std::string& suffix,
        std::vector<orch::DAGNode> nodes,
        std::vector<orch::DAGEdge> edges = {}) const {
        const auto deadline =
            clock->monotonicNowNs() + 30'000'000'000LL;
        const auto catalog =
            atomic_manager->getToolCatalogSnapshot(
                bootstrapCall());
        expect(catalog.value.has_value(),
               "catalog snapshot is required");
        for (auto& node : nodes) {
            node.deadline_mono_ns = deadline;
        }
        orch::OrchestratorSubmitRequest value;
        value.dag.dag_id = "dag-" + suffix;
        value.dag.request_id = "request-" + suffix;
        value.dag.nodes = std::move(nodes);
        value.dag.edges = std::move(edges);
        value.dag.priority = TaskPriority::P1;
        value.dag.deadline_mono_ns = deadline;
        value.dag.idempotency_key = "idem-" + suffix;
        value.admission.principal_id_hash = "principal";
        value.admission.granted_priority = TaskPriority::P1;
        value.admission.policy_snapshot_id = "policy";
        value.admission.policy_digest =
            secureDigest(value.admission.policy_snapshot_id);
        value.admission.authorization_ref = "authorization";
        value.admission.allowed_capabilities = {
            "com_sgm_service_climate_setAirCirculationMode",
            "com_sgm_service_climate_setAutoFanSpeed"};
        value.admission.granted_permissions = {
            "vehicle.climate.write"};
        value.admission.deadline_mono_ns = deadline;
        value.idempotency_key = value.dag.idempotency_key;
        value.expected_capability_digest =
            catalog.value->catalog_digest;
        value.trace_id = "trace-" + suffix;
        value.submitted_at_utc_ms = clock->utcNowMs();
        return value;
    }

    CallContext call(
        const orch::OrchestratorSubmitRequest& request) const {
        return CallContext{
            CallerModuleId::AgentService,
            request.dag.request_id, request.trace_id,
            request.admission.principal_id_hash,
            request.admission.granted_priority,
            request.admission.deadline_mono_ns, {}, 0,
            request.admission.authorization_ref};
    }
};

orch::DAGNode circulation(
    const std::string& node_id,
    const std::string& mode = "AUTO") {
    orch::DAGNode node;
    node.node_id = node_id;
    node.executor = "atomic_service";
    node.action =
        "com_sgm_service_climate_setAirCirculationMode";
    node.params = nlohmann::json{{"mode", mode}};
    return node;
}

void testTimerAndSignalTriggers() {
    Fixture timer;
    auto timer_node = circulation("timer");
    timer_node.trigger.type = orch::TriggerType::Timer;
    timer_node.trigger.source = "platform_alarm";
    timer_node.trigger.next_fire_at_utc_ms =
        timer.clock->utcNowMs() + 100;
    const auto timer_request =
        timer.request("timer", {timer_node});
    const auto timer_call = timer.call(timer_request);
    const auto timer_commit =
        timer.orchestrator->submit(timer_request, timer_call);
    expect(timer_commit.accepted,
           "timer plan must be accepted");
    expect(!timer.orchestrator->pumpOne() &&
               timer.provider->invocationCount() == 0,
           "timer must remain blocked before its UTC fire time");
    timer.clock->advanceMs(100);
    expect(timer.orchestrator
                   ->runUntilPlanTerminal(timer_commit.plan_id)
                   .ok &&
               timer.provider->invocationCount() == 1,
           "due timer must create runnable work");

    Fixture signal;
    auto signal_node = circulation("signal");
    signal_node.trigger.type = orch::TriggerType::Signal;
    signal_node.trigger.source = "vehicle_state";
    signal_node.trigger.expression =
        nlohmann::json{{"event_name", "RAIN_STARTED"}};
    const auto signal_request =
        signal.request("signal", {signal_node});
    const auto signal_call = signal.call(signal_request);
    const auto signal_commit =
        signal.orchestrator->submit(
            signal_request, signal_call);
    expect(signal_commit.accepted &&
               !signal.orchestrator->pumpOne(),
           "signal plan must wait for a verified event");
    orch::SignalEvent event;
    event.event_id = "rain-event-1";
    event.source = "vehicle_state";
    event.event_name = "RAIN_STARTED";
    event.payload = nlohmann::json{{"rain", true}};
    event.source_epoch = 1;
    event.cursor = 1;
    event.occurred_at_utc_ms = signal.clock->utcNowMs();
    event.received_at_utc_ms = signal.clock->utcNowMs();
    event.signature_ref = "verified:vehicle-state";
    const auto applied =
        signal.orchestrator->publishSignal(
            event, signal_call);
    const auto duplicate =
        signal.orchestrator->publishSignal(
            event, signal_call);
    expect(applied.accepted &&
               applied.activated_count == 1 &&
               duplicate.accepted && duplicate.duplicate,
           "signal inbox must activate once and deduplicate replay");
    expect(signal.orchestrator
                   ->runUntilPlanTerminal(signal_commit.plan_id)
                   .ok,
           "signal-triggered plan must finish");
}

void testRecurringTimerCreatesDistinctActivations() {
    Fixture fixture;
    auto recurring = circulation("recurring");
    recurring.trigger.type = orch::TriggerType::Timer;
    recurring.trigger.source = "platform_alarm";
    recurring.trigger.next_fire_at_utc_ms =
        fixture.clock->utcNowMs() + 10;
    recurring.trigger.repeat_policy = orch::RepeatPolicy::FixedDelay;
    recurring.trigger.missed_fire_policy =
        orch::MissedFirePolicy::Coalesce;
    recurring.trigger.overlap_policy = orch::OverlapPolicy::Serial;
    recurring.lifecycle.max_activations = 3;
    recurring.lifecycle.repeat_interval_ms = 10;
    const auto request = fixture.request("recurring", {recurring});
    const auto call = fixture.call(request);
    const auto committed =
        fixture.orchestrator->submit(request, call);
    expect(committed.accepted,
           "recurring Timer plan must be admitted");

    for (std::uint32_t activation = 1; activation <= 3;
         ++activation) {
        fixture.clock->advanceMs(10);
        for (std::size_t step = 0;
             step < 100 && fixture.orchestrator->pumpOne(); ++step) {
        }
        const auto snapshot = fixture.orchestrator->getNodeStatus(
            committed.plan_id, "recurring", call);
        expect(snapshot.value &&
                   snapshot.value->activation_count == activation &&
                   snapshot.value->activation_history.size() == activation,
               "each Timer fire must retain one immutable Activation");
        if (activation < 3) {
            expect(snapshot.value->state == orch::ActivationState::Blocked &&
                       !snapshot.value->trigger_satisfied,
                   "bounded recurrence must wait without reusing identity");
        }
    }
    const auto plan = fixture.orchestrator->getPlan(
        committed.plan_id, call);
    expect(plan.value &&
               plan.value->state == orch::PlanState::Succeeded &&
               fixture.provider->invocationCount() == 3,
           "the plan must terminate after the lifecycle activation bound");
    const auto& history =
        plan.value->nodes.at("recurring").activation_history;
    expect(history[0].activation_id != history[1].activation_id &&
               history[1].activation_id != history[2].activation_id,
           "a stable PID must never reuse an Activation identity");
}

void testJoinAndResultBinding() {
    Fixture fixture;
    auto first = circulation("first", "INTERNAL");
    auto second = circulation("second", "EXTERNAL");
    auto joined = circulation("joined");
    joined.params = nlohmann::json::object();
    joined.join_policy.kind = orch::JoinKind::All;
    joined.result_bindings.push_back(
        {"first", "appliedMode", "mode", "IDENTITY",
         "FAIL_NODE", {}, 1, 1});
    std::vector<orch::DAGEdge> edges{
        {"edge-first", "first", "joined", "SUCCESS", {}, true},
        {"edge-second", "second", "joined", "SUCCESS", {}, true}};
    const auto request = fixture.request(
        "join-binding", {first, second, joined}, edges);
    const auto call = fixture.call(request);
    const auto committed =
        fixture.orchestrator->submit(request, call);
    expect(committed.accepted,
           "join plan must be admitted; reject=" +
               committed.reject_code);
    expect(fixture.orchestrator
                   ->runUntilPlanTerminal(committed.plan_id)
                   .ok,
           "join plan must terminate");
    const auto plan =
        fixture.orchestrator->getPlan(
            committed.plan_id, call);
    const auto diagnostic =
        !plan.value
            ? std::string{"missing plan"}
            : std::string{"state="} +
                  std::to_string(
                      static_cast<int>(plan.value->state)) +
                  " joined=" +
                  plan.value->nodes.at("joined").bound_params.dump() +
                  " node_state=" +
                  std::to_string(static_cast<int>(
                      plan.value->nodes.at("joined").state)) +
                  " error=" +
                  plan.value->nodes.at("joined").error_code +
                  " first=" +
                  std::to_string(static_cast<int>(
                      plan.value->nodes.at("first").state)) +
                  ":" + plan.value->nodes.at("first").error_code +
                  " second=" +
                  std::to_string(static_cast<int>(
                      plan.value->nodes.at("second").state)) +
                  ":" + plan.value->nodes.at("second").error_code +
                  " calls=" +
                  std::to_string(fixture.provider->invocationCount());
    expect(plan.value &&
               plan.value->state == orch::PlanState::Succeeded &&
               plan.value->nodes.at("joined")
                       .bound_params.at("mode") ==
                   "INTERNAL" &&
               fixture.provider->invocationCount() == 3,
           "ALL Join and typed result binding must be applied; " +
               diagnostic);
}

void testControlAndReliableEvents() {
    Fixture fixture;
    const auto request =
        fixture.request("control", {circulation("node")});
    const auto call = fixture.call(request);
    const auto committed =
        fixture.orchestrator->submit(request, call);
    expect(committed.accepted,
           "control plan must be admitted");
    auto snapshot =
        fixture.orchestrator->getPlan(
            committed.plan_id, call);
    expect(snapshot.value.has_value(),
           "control plan snapshot is required");

    orch::PlanControlRequest suspend;
    suspend.plan_id = committed.plan_id;
    suspend.control_epoch = 1;
    suspend.expected_plan_version =
        snapshot.value->version;
    suspend.reason = "TEST_SUSPEND";
    suspend.deadline_mono_ns = call.deadline_mono_ns;
    const auto suspended =
        fixture.orchestrator->suspend(suspend, call);
    expect(suspended.accepted &&
               !fixture.orchestrator->pumpOne() &&
               fixture.provider->invocationCount() == 0,
           "suspended plan must not dispatch new work");

    orch::PlanControlRequest resume = suspend;
    resume.control_epoch = 2;
    resume.expected_plan_version =
        suspended.plan_version;
    resume.reason = "TEST_RESUME";
    const auto resumed =
        fixture.orchestrator->resume(resume, call);
    expect(resumed.accepted &&
               fixture.orchestrator
                   ->runUntilPlanTerminal(committed.plan_id)
                   .ok,
           "resumed plan must continue");

    orch::EventSubscriptionRequest subscription;
    subscription.consumer_id = "agent-service-test";
    subscription.consumer_epoch = 1;
    subscription.cursor = 0;
    subscription.max_events = 128;
    subscription.plan_id = committed.plan_id;
    const auto page =
        fixture.orchestrator->subscribeEvents(
            subscription, call);
    expect(page.value && !page.value->events.empty(),
           "event subscription must replay plan events");
    const auto ack = fixture.orchestrator->ackEvents(
        {subscription.consumer_id,
         subscription.consumer_epoch,
         page.value->next_cursor},
        call);
    const auto health =
        fixture.orchestrator->health(
            orch::HealthDetailLevel::Detailed, call);
    expect(ack.ok && health.healthy &&
               health.plan_count == 1,
           "event acknowledgement and health must be available");
}

}  // namespace

int main() {
    try {
        testTimerAndSignalTriggers();
        testRecurringTimerCreatesDistinctActivations();
        testJoinAndResultBinding();
        testControlAndReliableEvents();
        std::cout << "orchestrator control tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "orchestrator control tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
