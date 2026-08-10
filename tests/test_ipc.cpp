/**
 * @file test_ipc.cpp
 * @brief Verifies framed IPC identity, authentication, deadlines, and size bounds.
 */

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "master_agent/transport/ipc/remote_services.h"

namespace {

using namespace master_agent;
using namespace master_agent::ipc;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

IpcEnvelope requestFor(
    const IRuntimeClock& clock,
    const std::string& source,
    std::uint64_t source_epoch,
    const std::string& target) {
    IpcEnvelope request;
    request.message_id = "message-1";
    request.source_endpoint_id = source;
    request.source_process_epoch = source_epoch;
    request.target_endpoint_id = target;
    request.operation = "ipc.echo";
    request.request_id = "request-1";
    request.trace_id = "trace-1";
    request.session_id = "session-1";
    request.priority = TaskPriority::P1;
    request.deadline_mono_ns =
        clock.monotonicNowNs() + 5'000'000'000LL;
    request.idempotency_key = "idempotency-1";
    request.fencing_token = 7;
    request.payload = {{"text", "hello"}};
    sealIpcEnvelope(request);
    return request;
}

}  // namespace

int main() {
    try {
        ManualRuntimeClock manual;
        auto request =
            requestFor(manual, "source", 11, "target");
        require(validateIpcEnvelope(request, manual).ok,
                "sealed envelope was rejected");
        auto tampered = request;
        tampered.payload["text"] = "changed";
        require(!validateIpcEnvelope(tampered, manual).ok,
                "payload tampering was accepted");
        auto unknown_field = encodeIpcEnvelope(request);
        unknown_field["future_unreviewed_field"] = true;
        require(!decodeIpcEnvelope(unknown_field).status.ok,
                "open envelope schema was accepted");
        manual.advanceMs(5001);
        require(
            validateIpcEnvelope(request, manual)
                    .error.code ==
                "IPC_DEADLINE_EXCEEDED",
            "expired envelope did not fail closed");

        auto clock = std::make_shared<SystemRuntimeClock>();
        const auto runtime =
            std::filesystem::temp_directory_path() /
            ("master-agent-ipc-test-" +
             secureDigest(
                 std::to_string(clock->utcNowMs()) + "|" +
                 std::to_string(currentProcessId()))
                 .substr(0, 12));
        std::filesystem::create_directories(runtime);
        ProcessRegistry registry(runtime);
        const ChildProcess supervisor{
            "supervisor", 1, currentProcessId(), 0};
        const ChildProcess server_identity{
            "echo-server", 42, currentProcessId(), 0};
        auto status =
            registry.publish({server_identity}, supervisor);
        require(status.ok, status.error.code);
        require(registry.authenticate("echo-server", 42).ok,
                "registered epoch was rejected");
        require(
            registry.authenticate("echo-server", 41)
                    .error.code ==
                "PROCESS_EPOCH_FENCED",
            "stale registry epoch was accepted");

        std::atomic_bool stop{false};
        IpcServer server(
            {runtime, "echo-server", 42}, clock);
        status = server.initialize();
        require(status.ok, status.error.code);
        Status serve_status;
        std::thread server_thread([&] {
            serve_status = server.serve(
                [&](const IpcEnvelope& incoming) {
                    stop.store(true);
                    return Result<IpcEnvelope>::Success(
                        makeRpcResponse(
                            incoming, 42, Status::Ok(),
                            incoming.payload));
                },
                stop);
        });
        auto live_request =
            requestFor(*clock, "echo-client", 9,
                       "echo-server");
        const auto response = IpcClient(
            {runtime, "echo-server", 42})
                                  .call(live_request);
        server_thread.join();
        require(serve_status.ok, serve_status.error.code);
        require(response.status.ok && response.value,
                response.status.error.code);
        require(
            response.value->payload.at("value")
                    .at("text")
                    .get<std::string>() == "hello",
            "framed IPC echo payload changed");
        require(
            response.value->source_process_epoch == 42 &&
                response.value->fencing_token == 7,
            "response epoch or fencing identity changed");

        std::cout
            << nlohmann::json{
                   {"success", true},
                   {"digest_tamper_rejected", true},
                   {"closed_schema_enforced", true},
                   {"deadline_enforced", true},
                   {"registry_epoch_fenced", true},
                   {"native_ipc_echo", true}}
                   .dump(2)
            << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
