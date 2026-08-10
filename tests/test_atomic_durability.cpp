/**
 * @file test_atomic_durability.cpp
 * @brief Verifies atomic execution recovery and idempotency after interruption.
 */

#include "master_agent/atomic_service/atomic_service.h"

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
using namespace master_agent::atomic_service;

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
        "atomic-durable-bootstrap",
        "trace-atomic-durable-bootstrap",
        "system",
        TaskPriority::P1,
        clock->monotonicNowNs() + 60'000'000'000LL};
}

Status registerClimate(
    AtomicServiceManager& manager,
    const std::shared_ptr<DeterministicClimateProvider>& provider,
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    return manager.registerTools(
        defaultClimateMcpTools(),
        defaultClimateRuntimePolicies(1),
        provider, bootstrapCall(clock));
}

AtomicMcpCallEnvelope durableEnvelope(
    const McpToolCatalogSnapshot& catalog,
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    const std::string tool =
        "com_sgm_service_climate_setAutoFanSpeed";
    AtomicMcpCallEnvelope envelope;
    envelope.mcp_request.id = "operation-durable-1";
    envelope.mcp_request.name = tool;
    envelope.mcp_request.arguments =
        nlohmann::json{{"location", "FRONT"},
                       {"mode", "HIGH"}};
    envelope.runtime.caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    envelope.runtime.request_id = "request-durable-1";
    envelope.runtime.trace_id = "trace-durable-1";
    envelope.runtime.plan_id = "plan-durable-1";
    envelope.runtime.pid = "pid-durable-1";
    envelope.runtime.activation_id = "activation-durable-1";
    envelope.runtime.execution_id = "execution-durable-1";
    envelope.runtime.attempt_no = 1;
    envelope.runtime.operation_id = envelope.mcp_request.id;
    envelope.runtime.priority = TaskPriority::P1;
    envelope.runtime.deadline_mono_ns =
        clock->monotonicNowNs() + 30'000'000'000LL;
    envelope.runtime.idempotency_key = "idempotency-durable-1";
    envelope.runtime.fencing_token = 41;
    envelope.runtime.tool_catalog_snapshot_id = catalog.snapshot_id;
    envelope.runtime.tool_digest = catalog.tool_digests.at(tool);
    envelope.runtime.policy_digest = catalog.policy_digests.at(tool);
    envelope.runtime.granted_permissions = {
        "vehicle.climate.write"};
    envelope.runtime.principal_id_hash =
        "principal-durable-1";
    envelope.runtime.authorization_ref =
        "authorization-durable-1";
    return envelope;
}

CallContext executionCall(
    const AtomicMcpCallEnvelope& envelope) {
    return CallContext{
        CallerModuleId::TaskOrchestrationEngine,
        envelope.runtime.request_id,
        envelope.runtime.trace_id,
        envelope.runtime.principal_id_hash,
        envelope.runtime.priority,
        envelope.runtime.deadline_mono_ns,
        {},
        0,
        envelope.runtime.authorization_ref};
}

std::vector<std::string> readWalLines(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "atomic WAL must be readable");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void writeWalLines(const std::filesystem::path& path,
                   const std::vector<std::string>& lines) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(
        path, std::ios::binary | std::ios::out | std::ios::trunc);
    expect(output.is_open(), "atomic WAL fixture must be writable");
    for (const auto& line : lines) output << line << '\n';
    output.flush();
    expect(output.good(), "atomic WAL fixture write must succeed");
}

std::vector<std::string> throughProviderSeal(
    const std::vector<std::string>& lines) {
    std::size_t sealed_index = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto frame = nlohmann::json::parse(lines[i]);
        const auto& snapshot =
            frame.at("payload").at("snapshot");
        if (!snapshot.at("provider_invocation").is_null() &&
            snapshot.at("state").get<std::uint8_t>() ==
                static_cast<std::uint8_t>(
                    AtomicExecutionState::Running)) {
            sealed_index = i;
            break;
        }
    }
    expect(sealed_index < lines.size(),
           "WAL must contain a pre-Provider durable seal");
    return std::vector<std::string>(
        lines.begin(), lines.begin() + sealed_index + 1);
}

void testTerminalRestartReplayDoesNotInvokeProviderTwice() {
    ScopedTempDirectory temp("master-agent-atomic-terminal");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto first_provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicMcpCallEnvelope envelope;
    {
        AtomicServiceManager manager(
            clock,
            std::make_shared<IdGenerator>("atomic-first"),
            1, nullptr, temp.path());
        expect(registerClimate(manager, first_provider, clock).ok,
               "first durable Atomic manager must register");
        const auto catalog =
            manager.getToolCatalogSnapshot(bootstrapCall(clock));
        expect(catalog.value.has_value(),
               "first catalog must be available");
        envelope = durableEnvelope(*catalog.value, clock);
        const auto accepted =
            manager.callTool(envelope, executionCall(envelope));
        expect(accepted.accepted && !accepted.existing,
               "durable Atomic execution must be admitted");
        expect(manager.runUntilIdle().ok &&
                   first_provider->invocationCount() == 1,
               "first process must invoke Provider exactly once");
    }

    auto recovered_provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager recovered(
        clock,
        std::make_shared<IdGenerator>("atomic-recovered"),
        1, nullptr, temp.path());
    expect(registerClimate(recovered, recovered_provider, clock).ok,
           "terminal Atomic WAL must recover");
    const auto replay =
        recovered.callTool(envelope, executionCall(envelope));
    const auto snapshot = recovered.queryExecution(
        envelope.runtime.execution_id, executionCall(envelope));
    expect(replay.accepted && replay.existing &&
               snapshot.value &&
               snapshot.value->state ==
                   AtomicExecutionState::Succeeded,
           "restart replay must return the recovered terminal execution");
    expect(recovered.runUntilIdle().ok &&
               recovered_provider->invocationCount() == 0,
           "terminal restart replay must never call Provider again");
}

void testSealedCrashRecoversUnknownAndRequiresReconcile() {
    ScopedTempDirectory source("master-agent-atomic-source");
    ScopedTempDirectory crash("master-agent-atomic-crash");
    auto clock = std::make_shared<ManualRuntimeClock>();
    AtomicMcpCallEnvelope envelope;
    {
        auto provider =
            std::make_shared<DeterministicClimateProvider>();
        AtomicServiceManager manager(
            clock,
            std::make_shared<IdGenerator>("atomic-seal-source"),
            1, nullptr, source.path());
        expect(registerClimate(manager, provider, clock).ok,
               "seal source manager must register");
        const auto catalog =
            manager.getToolCatalogSnapshot(bootstrapCall(clock));
        expect(catalog.value.has_value(),
               "seal source catalog must be available");
        envelope = durableEnvelope(*catalog.value, clock);
        expect(manager.callTool(
                   envelope, executionCall(envelope)).accepted &&
                   manager.runUntilIdle().ok,
               "seal source execution must complete");
    }

    const auto source_wal =
        source.path() / "atomic_execution.wal";
    const auto crash_wal =
        crash.path() / "atomic_execution.wal";
    writeWalLines(
        crash_wal,
        throughProviderSeal(readWalLines(source_wal)));

    auto recovered_provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager recovered(
        clock,
        std::make_shared<IdGenerator>("atomic-seal-recovered"),
        1, nullptr, crash.path());
    expect(registerClimate(
               recovered, recovered_provider, clock).ok,
           "sealed crash WAL must recover");
    auto snapshot = recovered.queryExecution(
        envelope.runtime.execution_id, executionCall(envelope));
    expect(snapshot.value &&
               snapshot.value->state ==
                   AtomicExecutionState::Unknown &&
               snapshot.value->provider_invocation &&
               snapshot.value->error_code ==
                   "ATOMIC_RECOVERED_INFLIGHT_REQUIRES_RECONCILE",
           "sealed non-terminal invocation must recover UNKNOWN");
    const auto replay =
        recovered.callTool(envelope, executionCall(envelope));
    expect(replay.accepted && replay.existing &&
               recovered.runUntilIdle().ok &&
               recovered_provider->invocationCount() == 0,
           "UNKNOWN replay must not cross the Provider call boundary");

    const auto reconciled = recovered.reconcileExecution(
        envelope.runtime.operation_id, executionCall(envelope));
    snapshot = recovered.queryExecution(
        envelope.runtime.execution_id, executionCall(envelope));
    expect(reconciled.status.ok && snapshot.value &&
               snapshot.value->state ==
                   AtomicExecutionState::Succeeded &&
               recovered_provider->invocationCount() == 0 &&
               recovered_provider->reconciliationCount() == 1,
           "only explicit reconciliation may settle recovered UNKNOWN");
}

void testTornTailIsTruncatedButCommittedCorruptionFailsClosed() {
    ScopedTempDirectory temp("master-agent-atomic-tail");
    auto clock = std::make_shared<ManualRuntimeClock>();
    AtomicMcpCallEnvelope envelope;
    {
        auto provider =
            std::make_shared<DeterministicClimateProvider>();
        AtomicServiceManager manager(
            clock,
            std::make_shared<IdGenerator>("atomic-tail-source"),
            1, nullptr, temp.path());
        expect(registerClimate(manager, provider, clock).ok,
               "tail source manager must register");
        const auto catalog =
            manager.getToolCatalogSnapshot(bootstrapCall(clock));
        expect(catalog.value.has_value(),
               "tail source catalog must be available");
        envelope = durableEnvelope(*catalog.value, clock);
        expect(manager.callTool(
                   envelope, executionCall(envelope)).accepted &&
                   manager.runUntilIdle().ok,
               "tail source execution must complete");
    }
    const auto wal = temp.path() / "atomic_execution.wal";
    {
        std::ofstream tail(
            wal, std::ios::binary | std::ios::out | std::ios::app);
        tail << "{\"torn\":";
    }
    auto recovered_provider =
        std::make_shared<DeterministicClimateProvider>();
    AtomicServiceManager recovered(
        clock,
        std::make_shared<IdGenerator>("atomic-tail-recovered"),
        1, nullptr, temp.path());
    expect(registerClimate(
               recovered, recovered_provider, clock).ok,
           "unterminated physical tail must be truncated safely");
    const auto snapshot = recovered.queryExecution(
        envelope.runtime.execution_id, executionCall(envelope));
    expect(snapshot.value &&
               snapshot.value->state ==
                   AtomicExecutionState::Succeeded,
           "tail truncation must retain every committed frame");

    ScopedTempDirectory corrupt("master-agent-atomic-corrupt");
    auto lines = readWalLines(wal);
    auto first = nlohmann::json::parse(lines.front());
    first["checksum"] = "forged-checksum";
    lines.front() = first.dump();
    writeWalLines(
        corrupt.path() / "atomic_execution.wal", lines);
    AtomicServiceManager poisoned(
        clock,
        std::make_shared<IdGenerator>("atomic-corrupt-recovered"),
        1, nullptr, corrupt.path());
    auto poisoned_provider =
        std::make_shared<DeterministicClimateProvider>();
    const auto registration =
        registerClimate(poisoned, poisoned_provider, clock);
    expect(!registration.ok &&
               registration.error.code == "ATOMIC_WAL_CORRUPT" &&
               poisoned_provider->invocationCount() == 0,
           "newline-committed corruption must keep the manager fail-closed");
}

}  // namespace

int main() {
    testTerminalRestartReplayDoesNotInvokeProviderTwice();
    testSealedCrashRecoversUnknownAndRequiresReconcile();
    testTornTailIsTruncatedButCommittedCorruptionFailsClosed();
    std::cout << "test_atomic_durability passed\n";
    return 0;
}
