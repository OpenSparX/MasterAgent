#pragma once

/**
 * @file ipc.h
 * @brief Defines authenticated IPC envelopes, endpoints, process identity, and supervision.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/common/types.h"

namespace master_agent::ipc {

/// Frozen cross-process envelope. Every request and response carries the
/// complete identity/version/deadline/fencing chain required by the joint
/// process baseline; payload_digest prevents transport or routing code
/// from silently changing the body.
struct IpcEnvelope {
    std::uint32_t schema_version = 2;
    std::string message_id;
    std::string correlation_id;
    std::string source_endpoint_id;
    std::uint64_t source_process_epoch = 0;
    std::string target_endpoint_id;
    std::string operation;
    std::string request_id;
    std::string trace_id;
    std::string session_id;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string idempotency_key;
    std::uint64_t fencing_token = 0;
    std::string reality = "SIMULATED";
    nlohmann::json payload = nlohmann::json::object();
    std::string payload_digest;
};

std::string ipcPayloadDigest(const IpcEnvelope& envelope);

/// Fills payload_digest after all immutable request fields are frozen.
void sealIpcEnvelope(IpcEnvelope& envelope);

/// Performs closed-schema, identity, deadline, payload-size and digest
/// validation before a message reaches any module Owner.
Status validateIpcEnvelope(const IpcEnvelope& envelope,
                           const IRuntimeClock& clock,
                           std::size_t max_payload_bytes =
                               1024U * 1024U);

nlohmann::json encodeIpcEnvelope(const IpcEnvelope& envelope);
Result<IpcEnvelope> decodeIpcEnvelope(const nlohmann::json& encoded);

/// Runtime-scoped endpoint address. Windows uses a named pipe and
/// Linux/Android host integration uses an AF_UNIX socket.
struct IpcEndpoint {
    std::filesystem::path runtime_directory;
    std::string endpoint_id;
    // Zero disables client-side epoch pinning. Multi-process launchers should
    // populate this from the signed process registry.
    std::uint64_t process_epoch = 0;
};

using IpcHandler =
    std::function<Result<IpcEnvelope>(const IpcEnvelope&)>;

/// One-request-per-connection synchronous client. The API is deliberately
/**
 * @brief Sends one authenticated request to a fixed endpoint and process epoch.
 *
 * The client validates both the outbound envelope and the response identity.
 * It never follows a restarted endpoint implicitly.
 */
class IpcClient {
public:
    explicit IpcClient(IpcEndpoint endpoint,
                          std::uint32_t connect_timeout_ms = 3000);

    /**
     * Sends exactly one framed request and validates the response correlation.
     * Transport success does not strengthen the business operation's semantics.
     */
    Result<IpcEnvelope> call(
        const IpcEnvelope& request) const;

private:
    IpcEndpoint endpoint_;
    std::uint32_t connect_timeout_ms_;
};

/**
 * @brief Receives framed IPC messages and dispatches validated envelopes.
 *
 * The blocking reactor performs no business work: it validates one bounded
 * request, invokes the handler, seals the response, and closes the connection.
 * Process entry points enqueue validated requests into their owner mailbox.
 */
class IpcServer {
public:
    IpcServer(IpcEndpoint endpoint,
                 std::shared_ptr<IRuntimeClock> clock,
                 std::size_t max_payload_bytes = 1024U * 1024U);
    ~IpcServer();

    /// Acquires the endpoint and begins listening; no requests are served yet.
    Status initialize();

    /// Serves until stop_requested is set or a transport error terminates the loop.
    Status serve(const IpcHandler& handler,
                 const std::atomic_bool& stop_requested);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Child process identity owned by the Supervisor.
struct ChildProcess {
    std::string endpoint_id;
    std::uint64_t process_epoch = 0;
    std::uint64_t native_process_id = 0;
    std::uintptr_t native_handle = 0;
};

/// Minimal cross-platform supervisor seam for an optional multi-process host
/// and fault-injection tests.
class ProcessSupervisor {
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    /// Starts one endpoint generation and records its native process identity.
    Result<ChildProcess> spawn(
        const std::filesystem::path& executable,
        const std::vector<std::string>& arguments,
        const std::string& endpoint_id,
        std::uint64_t process_epoch);

    bool isAlive(const ChildProcess& child) const;
    Status terminate(const ChildProcess& child,
                     std::uint32_t exit_code = 143);
    Status wait(const ChildProcess& child,
                std::uint32_t timeout_ms,
                std::optional<std::uint32_t>* exit_code = nullptr);

private:
    std::map<std::uint64_t, ChildProcess> children_;
};

/// Durable Supervisor-owned registry used to authenticate an IPC source
/// endpoint and reject messages from a dead/restarted generation.
class ProcessRegistry {
public:
    explicit ProcessRegistry(
        std::filesystem::path runtime_directory);

    /// Atomically publishes the supervisor-authorized endpoint generations.
    Status publish(const std::vector<ChildProcess>& children,
                   const ChildProcess& supervisor);
    Result<std::map<std::string, ChildProcess>> load() const;
    /// Rejects unknown endpoints and stale generations after a process restart.
    Status authenticate(const std::string& endpoint_id,
                        std::uint64_t process_epoch) const;

private:
    std::filesystem::path registry_path_;
};

/// Native PID is evidence only; process_epoch remains the restart fence.
std::uint64_t currentProcessId();

}  // namespace master_agent::ipc
