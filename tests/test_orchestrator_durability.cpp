/**
 * @file test_orchestrator_durability.cpp
 * @brief Verifies durable plan recovery and activation identity continuity.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/orchestrator/orchestrator.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace master_agent;
namespace atomic = master_agent::atomic_service;
namespace dispatch = master_agent::agent_dispatch;
namespace orch = master_agent::orchestrator;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(const std::string& prefix) {
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "-" + std::to_string(nonce));
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

CallContext bootstrapCall(
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    return CallContext{
        CallerModuleId::AgentService,
        "orchestrator-durable-bootstrap",
        "trace-orchestrator-durable-bootstrap",
        "system",
        TaskPriority::P1,
        clock->monotonicNowNs() + 60'000'000'000LL};
}

struct RuntimeFixture {
    std::shared_ptr<atomic::DeterministicClimateProvider>
        provider;
    std::shared_ptr<atomic::AtomicServiceManager> atomic_manager;
    std::shared_ptr<dispatch::AgentDispatch> dispatch_manager;
    std::shared_ptr<orch::Orchestrator> orchestrator;
};

RuntimeFixture runtimeFixture(
    const std::filesystem::path& root,
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const std::string& id_prefix) {
    RuntimeFixture fixture;
    auto ids =
        std::make_shared<IdGenerator>(id_prefix);
    fixture.provider =
        std::make_shared<
            atomic::DeterministicClimateProvider>();
    fixture.dispatch_manager =
        std::make_shared<dispatch::AgentDispatch>(
            clock, ids);
    fixture.atomic_manager =
        std::make_shared<atomic::AtomicServiceManager>(
            clock, ids, 1, fixture.dispatch_manager,
            root / "atomic");
    const auto registered =
        fixture.atomic_manager->registerTools(
            atomic::defaultClimateMcpTools(),
            atomic::defaultClimateRuntimePolicies(1),
            fixture.provider, bootstrapCall(clock));
    expect(registered.ok,
           "durable fixture Atomic catalog must register: " +
               registered.error.code);
    fixture.orchestrator =
        std::make_shared<orch::Orchestrator>(
            clock, ids, fixture.atomic_manager,
            fixture.dispatch_manager, root / "orchestrator");
    return fixture;
}

struct SubmitFixture {
    orch::OrchestratorSubmitRequest request;
    CallContext call;
};

SubmitFixture submitFixture(
    const RuntimeFixture& runtime,
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const std::string& suffix) {
    const auto catalog =
        runtime.atomic_manager->getToolCatalogSnapshot(
            bootstrapCall(clock));
    expect(catalog.value.has_value(),
           "durable submit fixture requires catalog");
    const std::string tool =
        "com_sgm_service_climate_setAutoFanSpeed";
    const auto deadline =
        clock->monotonicNowNs() + 30'000'000'000LL;

    SubmitFixture fixture;
    fixture.request.dag.dag_id = "dag-" + suffix;
    fixture.request.dag.request_id = "request-" + suffix;
    fixture.request.dag.priority = TaskPriority::P1;
    fixture.request.dag.deadline_mono_ns = deadline;
    fixture.request.dag.idempotency_key =
        "orchestrator-idempotency-" + suffix;
    orch::DAGNode node;
    node.node_id = "climate";
    node.executor = "atomic_service";
    node.action = tool;
    node.params =
        nlohmann::json{{"location", "FRONT"},
                       {"mode", "NORMAL"}};
    node.deadline_mono_ns = deadline;
    fixture.request.dag.nodes = {node};

    auto& admission = fixture.request.admission;
    admission.principal_id_hash =
        "principal-orchestrator-durable";
    admission.granted_priority = TaskPriority::P1;
    admission.policy_snapshot_id =
        "orchestrator-durable-policy";
    admission.policy_digest =
        secureDigest(admission.policy_snapshot_id);
    admission.authorization_ref =
        "orchestrator-durable-authorization";
    admission.allowed_capabilities = {tool};
    admission.granted_permissions = {
        "vehicle.climate.write"};
    admission.deadline_mono_ns = deadline;
    fixture.request.idempotency_key =
        fixture.request.dag.idempotency_key;
    fixture.request.expected_capability_digest =
        catalog.value->catalog_digest;
    fixture.request.trace_id = "trace-" + suffix;
    fixture.call = CallContext{
        CallerModuleId::AgentService,
        fixture.request.dag.request_id,
        fixture.request.trace_id,
        admission.principal_id_hash,
        admission.granted_priority,
        deadline,
        {},
        0,
        admission.authorization_ref};
    return fixture;
}

void testTerminalPlanReplaysAcrossRestart() {
    ScopedTempDirectory temp(
        "master-agent-orchestrator-terminal");
    auto clock = std::make_shared<ManualRuntimeClock>();
    SubmitFixture submit;
    std::string plan_id;
    {
        auto first =
            runtimeFixture(temp.path(), clock, "orch-first");
        submit = submitFixture(first, clock, "terminal");
        const auto accepted =
            first.orchestrator->submit(
                submit.request, submit.call);
        expect(accepted.accepted && !accepted.existing,
               "first Plan must commit durably");
        plan_id = accepted.plan_id;
        expect(first.orchestrator
                       ->runUntilPlanTerminal(plan_id)
                       .ok &&
                   first.provider->invocationCount() == 1,
               "first durable Plan must execute exactly once");
    }

    auto recovered =
        runtimeFixture(temp.path(), clock, "orch-recovered");
    const auto replay = recovered.orchestrator->submit(
        submit.request, submit.call);
    const auto plan = recovered.orchestrator->getPlan(
        plan_id, submit.call);
    expect(replay.accepted && replay.existing &&
               replay.plan_id == plan_id && plan.value &&
               plan.value->state == orch::PlanState::Succeeded,
           "restart must replay the same terminal Plan identity");
    expect(recovered.orchestrator
                   ->runUntilPlanTerminal(plan_id)
                   .ok &&
               recovered.provider->invocationCount() == 0,
           "terminal Plan replay must not redispatch a Tool");
}

void testCommittedButUndispatchedPlanRebuildsReadyQueue() {
    ScopedTempDirectory temp(
        "master-agent-orchestrator-inflight");
    auto clock = std::make_shared<ManualRuntimeClock>();
    SubmitFixture submit;
    std::string plan_id;
    {
        auto first =
            runtimeFixture(temp.path(), clock, "orch-inflight-first");
        submit = submitFixture(first, clock, "inflight");
        const auto accepted =
            first.orchestrator->submit(
                submit.request, submit.call);
        expect(accepted.accepted,
               "unfinished Plan must commit before simulated crash");
        plan_id = accepted.plan_id;
        expect(first.provider->invocationCount() == 0,
               "simulated crash must occur before dispatch");
    }

    auto recovered =
        runtimeFixture(temp.path(), clock, "orch-inflight-recovered");
    const auto plan = recovered.orchestrator->getPlan(
        plan_id, submit.call);
    const auto recovered_state =
        plan.value
            ? std::to_string(static_cast<int>(plan.value->state)) + "/" +
                  std::to_string(static_cast<int>(
                      plan.value->nodes.at("climate").state)) + "/" +
                  std::to_string(static_cast<int>(
                      plan.value->nodes.at("climate").side_effect_state))
            : std::string{"missing"};
    expect(plan.value &&
               plan.value->state == orch::PlanState::Running &&
               plan.value->nodes.at("climate").state ==
                   orch::ActivationState::Ready &&
               plan.value->nodes.at("climate").side_effect_state ==
                   SideEffectState::NotStarted &&
               recovered.provider->invocationCount() == 0,
           "committed work with no Dispatch attempt must rebuild ReadyQueue; " +
               recovered_state);
    const auto completed = recovered.orchestrator
                               ->runUntilPlanTerminal(plan_id);
    const auto final_plan = recovered.orchestrator->getPlan(
        plan_id, submit.call);
    const auto final_diagnostic =
        final_plan.value
            ? std::to_string(static_cast<int>(final_plan.value->state)) +
                  "/" + std::to_string(static_cast<int>(
                              final_plan.value->nodes.at("climate").state)) +
                  "/" +
                  final_plan.value->nodes.at("climate").error_code
            : std::string{"missing"};
    expect(completed.ok && recovered.provider->invocationCount() == 1,
           "recovered READY work may dispatch exactly once; status=" +
               completed.error.code + "; calls=" +
               std::to_string(recovered.provider->invocationCount()) +
               "; final=" + final_diagnostic);
}

void testTimerRegistrationSurvivesRestart() {
    ScopedTempDirectory temp("master-agent-orchestrator-timer");
    auto clock = std::make_shared<ManualRuntimeClock>();
    SubmitFixture submit;
    std::string plan_id;
    {
        auto first = runtimeFixture(
            temp.path(), clock, "orch-timer-first");
        submit = submitFixture(first, clock, "timer-recovery");
        auto& node = submit.request.dag.nodes.front();
        node.trigger.type = orch::TriggerType::Timer;
        node.trigger.source = "platform_alarm";
        node.trigger.next_fire_at_utc_ms = clock->utcNowMs() + 100;
        const auto accepted = first.orchestrator->submit(
            submit.request, submit.call);
        expect(accepted.accepted && !first.orchestrator->pumpOne(),
               "future Timer must commit without dispatching");
        plan_id = accepted.plan_id;
    }

    auto recovered = runtimeFixture(
        temp.path(), clock, "orch-timer-recovered");
    const auto waiting = recovered.orchestrator->getNodeStatus(
        plan_id, "climate", submit.call);
    expect(waiting.value &&
               waiting.value->state == orch::ActivationState::Blocked &&
               !waiting.value->trigger_satisfied &&
               waiting.value->activation_count == 0 &&
               waiting.value->next_fire_at_utc_ms ==
                   submit.request.dag.nodes.front()
                       .trigger.next_fire_at_utc_ms,
           "restart must restore the absolute Timer cursor");
    clock->advanceMs(100);
    expect(recovered.orchestrator
                   ->runUntilPlanTerminal(plan_id)
                   .ok &&
               recovered.provider->invocationCount() == 1,
           "restored Timer must create one fresh Activation when due");
}

void testCommittedPlanWalCorruptionRejectsAllWork() {
    ScopedTempDirectory temp(
        "master-agent-orchestrator-corrupt");
    auto clock = std::make_shared<ManualRuntimeClock>();
    {
        auto first =
            runtimeFixture(temp.path(), clock, "orch-corrupt-first");
        const auto submit =
            submitFixture(first, clock, "corrupt");
        expect(first.orchestrator
                   ->submit(submit.request, submit.call)
                   .accepted,
               "corruption fixture Plan must commit");
    }
    const auto wal =
        temp.path() / "orchestrator" /
        "orchestrator_plans.wal";
    std::ifstream input(wal, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    expect(!lines.empty(),
           "Plan WAL corruption fixture must contain frames");
    auto frame = nlohmann::json::parse(lines.front());
    frame["checksum"] = "forged-checksum";
    lines.front() = frame.dump();
    std::ofstream output(
        wal, std::ios::binary | std::ios::out | std::ios::trunc);
    for (const auto& encoded : lines) output << encoded << '\n';
    output.close();

    auto provider =
        std::make_shared<
            atomic::DeterministicClimateProvider>();
    auto ids =
        std::make_shared<IdGenerator>("orch-corrupt-recovered");
    auto dispatch_manager =
        std::make_shared<dispatch::AgentDispatch>(
            clock, ids);
    auto atomic_manager =
        std::make_shared<atomic::AtomicServiceManager>(
            clock, ids, 1, dispatch_manager,
            temp.path() / "atomic");
    expect(atomic_manager
               ->registerTools(
                   atomic::defaultClimateMcpTools(),
                   atomic::defaultClimateRuntimePolicies(1),
                   provider, bootstrapCall(clock))
               .ok,
           "Atomic state remains independently recoverable");
    auto poisoned =
        std::make_shared<orch::Orchestrator>(
            clock, ids, atomic_manager, dispatch_manager,
            temp.path() / "orchestrator");
    const auto candidate_runtime =
        RuntimeFixture{provider, atomic_manager,
                       dispatch_manager, poisoned};
    const auto candidate =
        submitFixture(candidate_runtime, clock, "after-corruption");
    const auto rejected =
        poisoned->submit(candidate.request, candidate.call);
    expect(!rejected.accepted &&
               rejected.reject_code ==
                   "ORCHESTRATOR_WAL_CORRUPT" &&
               provider->invocationCount() == 0,
           "committed Plan WAL corruption must fail closed");
}

}  // namespace

int main() {
    try {
        testTerminalPlanReplaysAcrossRestart();
        testCommittedButUndispatchedPlanRebuildsReadyQueue();
        testTimerRegistrationSurvivesRestart();
        testCommittedPlanWalCorruptionRejectsAllWork();
        std::cout << "test_orchestrator_durability passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_orchestrator_durability failed: "
                  << error.what() << '\n';
        return 1;
    }
}
