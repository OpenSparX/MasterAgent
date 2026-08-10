/**
 * @file ipc.cpp
 * @brief Implements authenticated framed IPC and process supervision primitives.
 */

#include "master_agent/transport/ipc/ipc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace master_agent::ipc {
namespace {

constexpr std::uint32_t kMaxWireFrameBytes = 4U * 1024U * 1024U;

std::string endpointToken(const IpcEndpoint& endpoint) {

    std::string readable = endpoint.endpoint_id;
    for (auto& ch : readable) {
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_')) {
            ch = '_';
        }
    }
    return secureDigest(
               endpoint.runtime_directory.lexically_normal().string())
               .substr(0, 16) +
           "-" + readable;
}

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), converted.data(),
            count) != count) {
        return {};
    }
    return converted;
}

std::wstring pipeName(const IpcEndpoint& endpoint) {
    return L"\\\\.\\pipe\\master-agent-" +
           utf8ToWide(endpointToken(endpoint));
}

bool writeAll(HANDLE handle, const void* data, std::size_t size) {
    const auto* cursor =
        static_cast<const unsigned char*>(data);
    while (size != 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

bool readAll(HANDLE handle, void* data, std::size_t size) {
    auto* cursor = static_cast<unsigned char*>(data);
    while (size != 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(handle, cursor, chunk, &read, nullptr) ||
            read == 0) {
            return false;
        }
        cursor += read;
        size -= read;
    }
    return true;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'"') {
            result.append(slashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

#else

std::filesystem::path socketPath(const IpcEndpoint& endpoint) {
    const auto directory = endpoint.runtime_directory / "ipc";
    return directory / (endpointToken(endpoint) + ".sock");
}

bool writeAll(int fd, const void* data, std::size_t size) {
    const auto* cursor =
        static_cast<const unsigned char*>(data);
    while (size != 0) {
        const auto written = ::send(fd, cursor, size, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        cursor += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool readAll(int fd, void* data, std::size_t size) {
    auto* cursor = static_cast<unsigned char*>(data);
    while (size != 0) {
        const auto read = ::recv(fd, cursor, size, 0);
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) return false;
        cursor += read;
        size -= static_cast<std::size_t>(read);
    }
    return true;
}

#endif

template <typename Handle>
bool writeFrame(Handle handle, const std::string& encoded) {
    if (encoded.empty() ||
        encoded.size() > kMaxWireFrameBytes) {
        return false;
    }
    const auto size = static_cast<std::uint32_t>(encoded.size());
    std::array<unsigned char, 4> header{
        static_cast<unsigned char>(size & 0xffU),
        static_cast<unsigned char>((size >> 8U) & 0xffU),
        static_cast<unsigned char>((size >> 16U) & 0xffU),
        static_cast<unsigned char>((size >> 24U) & 0xffU)};
    return writeAll(handle, header.data(), header.size()) &&
           writeAll(handle, encoded.data(), encoded.size());
}

template <typename Handle>
Result<std::string> readFrame(Handle handle) {
    std::array<unsigned char, 4> header{};
    if (!readAll(handle, header.data(), header.size())) {
        return Result<std::string>::Failure(Status::Error(
            "ipc", "IPC_FRAME_HEADER_READ_FAILED",
            "IPC frame header could not be read", true));
    }
    const auto size =
        static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
    if (size == 0 || size > kMaxWireFrameBytes) {
        return Result<std::string>::Failure(Status::Error(
            "ipc", "IPC_FRAME_SIZE_INVALID",
            "IPC frame size is outside the bounded contract"));
    }
    std::string encoded(size, '\0');
    if (!readAll(handle, encoded.data(), encoded.size())) {
        return Result<std::string>::Failure(Status::Error(
            "ipc", "IPC_FRAME_BODY_READ_FAILED",
            "IPC frame body could not be read", true));
    }
    return Result<std::string>::Success(std::move(encoded));
}

IpcEnvelope errorResponse(
    const IpcEnvelope& request, const Status& status,
    std::uint64_t source_process_epoch) {
    IpcEnvelope response;
    response.message_id =
        "ipc-error-" + secureDigest(
                           request.message_id + "|" +
                           status.error.code)
                           .substr(0, 24);
    response.correlation_id = request.message_id;
    response.source_endpoint_id = request.target_endpoint_id;
    response.source_process_epoch =
        source_process_epoch == 0 ? 1 : source_process_epoch;
    response.target_endpoint_id = request.source_endpoint_id;
    response.operation = request.operation + ".response";
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.session_id = request.session_id;
    response.priority = request.priority;
    response.deadline_mono_ns = request.deadline_mono_ns;
    response.idempotency_key = request.idempotency_key;
    response.fencing_token = request.fencing_token;
    response.reality = request.reality;
    response.payload = {
        {"ok", false},
        {"error",
         {{"domain", status.error.domain},
          {"code", status.error.code},
          {"message", status.error.message},
          {"retryable", status.error.retryable},
          {"side_effect_state",
           static_cast<int>(
               status.error.side_effect_state)}}}};
    sealIpcEnvelope(response);
    return response;
}

}  // namespace

std::string ipcPayloadDigest(const IpcEnvelope& envelope) {
    const nlohmann::json sealed{
        {"schema_version", envelope.schema_version},
        {"message_id", envelope.message_id},
        {"correlation_id", envelope.correlation_id},
        {"source_endpoint_id", envelope.source_endpoint_id},
        {"source_process_epoch", envelope.source_process_epoch},
        {"target_endpoint_id", envelope.target_endpoint_id},
        {"operation", envelope.operation},
        {"request_id", envelope.request_id},
        {"trace_id", envelope.trace_id},
        {"session_id", envelope.session_id},
        {"priority", static_cast<int>(envelope.priority)},
        {"deadline_mono_ns", envelope.deadline_mono_ns},
        {"idempotency_key", envelope.idempotency_key},
        {"fencing_token", envelope.fencing_token},
        {"reality", envelope.reality},
        {"payload", envelope.payload}};
    return secureDigest(sealed.dump());
}

void sealIpcEnvelope(IpcEnvelope& envelope) {
    envelope.payload_digest = ipcPayloadDigest(envelope);
}

Status validateIpcEnvelope(const IpcEnvelope& envelope,
                           const IRuntimeClock& clock,
                           std::size_t max_payload_bytes) {
    if (envelope.schema_version != 2 ||
        envelope.message_id.empty() ||
        envelope.message_id.size() > 256 ||
        envelope.source_endpoint_id.empty() ||
        envelope.source_endpoint_id.size() > 128 ||
        envelope.source_process_epoch == 0 ||
        envelope.target_endpoint_id.empty() ||
        envelope.target_endpoint_id.size() > 128 ||
        envelope.operation.empty() ||
        envelope.operation.size() > 256 ||
        envelope.request_id.empty() ||
        envelope.request_id.size() > 256 ||
        envelope.trace_id.empty() ||
        envelope.trace_id.size() > 256 ||
        !isValidTaskPriority(envelope.priority) ||
        envelope.deadline_mono_ns <= 0 ||
        envelope.idempotency_key.empty() ||
        envelope.idempotency_key.size() > 512 ||
        envelope.reality != "SIMULATED" ||
        !envelope.payload.is_object() ||
        envelope.payload.dump().size() > max_payload_bytes ||
        envelope.payload_digest.empty() ||
        envelope.payload_digest != ipcPayloadDigest(envelope)) {
        return Status::Error(
            "ipc", "IPC_ENVELOPE_INVALID",
            "IPC envelope failed closed validation");
    }
    if (deadlineExpired(envelope.deadline_mono_ns, clock)) {
        return Status::Error(
            "ipc", "IPC_DEADLINE_EXCEEDED",
            "IPC envelope deadline has expired");
    }
    return Status::Ok();
}

nlohmann::json encodeIpcEnvelope(
    const IpcEnvelope& envelope) {
    return {
        {"schema_version", envelope.schema_version},
        {"message_id", envelope.message_id},
        {"correlation_id", envelope.correlation_id},
        {"source_endpoint_id", envelope.source_endpoint_id},
        {"source_process_epoch", envelope.source_process_epoch},
        {"target_endpoint_id", envelope.target_endpoint_id},
        {"operation", envelope.operation},
        {"request_id", envelope.request_id},
        {"trace_id", envelope.trace_id},
        {"session_id", envelope.session_id},
        {"priority", static_cast<int>(envelope.priority)},
        {"deadline_mono_ns", envelope.deadline_mono_ns},
        {"idempotency_key", envelope.idempotency_key},
        {"fencing_token", envelope.fencing_token},
        {"reality", envelope.reality},
        {"payload", envelope.payload},
        {"payload_digest", envelope.payload_digest}};
}

Result<IpcEnvelope> decodeIpcEnvelope(
    const nlohmann::json& encoded) {
    try {
        if (!encoded.is_object() || encoded.size() != 17) {
            throw std::runtime_error("closed envelope schema mismatch");
        }
        IpcEnvelope envelope;
        envelope.schema_version =
            encoded.at("schema_version").get<std::uint32_t>();
        envelope.message_id =
            encoded.at("message_id").get<std::string>();
        envelope.correlation_id =
            encoded.at("correlation_id").get<std::string>();
        envelope.source_endpoint_id =
            encoded.at("source_endpoint_id").get<std::string>();
        envelope.source_process_epoch =
            encoded.at("source_process_epoch")
                .get<std::uint64_t>();
        envelope.target_endpoint_id =
            encoded.at("target_endpoint_id").get<std::string>();
        envelope.operation =
            encoded.at("operation").get<std::string>();
        envelope.request_id =
            encoded.at("request_id").get<std::string>();
        envelope.trace_id =
            encoded.at("trace_id").get<std::string>();
        envelope.session_id =
            encoded.at("session_id").get<std::string>();
        const auto priority =
            encoded.at("priority").get<int>();
        if (priority < static_cast<int>(TaskPriority::P0) ||
            priority > static_cast<int>(TaskPriority::P2)) {
            throw std::runtime_error("invalid priority");
        }
        envelope.priority =
            static_cast<TaskPriority>(priority);
        envelope.deadline_mono_ns =
            encoded.at("deadline_mono_ns")
                .get<std::int64_t>();
        envelope.idempotency_key =
            encoded.at("idempotency_key").get<std::string>();
        envelope.fencing_token =
            encoded.at("fencing_token").get<std::uint64_t>();
        envelope.reality =
            encoded.at("reality").get<std::string>();
        envelope.payload = encoded.at("payload");
        envelope.payload_digest =
            encoded.at("payload_digest").get<std::string>();
        return Result<IpcEnvelope>::Success(
            std::move(envelope));
    } catch (...) {
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_ENVELOPE_DECODE_FAILED",
            "IPC envelope failed closed decoding"));
    }
}

struct IpcServer::Impl {
    IpcEndpoint endpoint;
    std::shared_ptr<IRuntimeClock> clock;
    std::size_t max_payload_bytes = 0;
    bool initialized = false;
#ifdef _WIN32
    std::mutex active_pipes_mutex;
    std::set<HANDLE> active_pipes;
#else
    int listen_fd = -1;
    std::filesystem::path socket_path;
#endif
};

IpcClient::IpcClient(
    IpcEndpoint endpoint,
    std::uint32_t connect_timeout_ms)
    : endpoint_(std::move(endpoint)),
      connect_timeout_ms_(connect_timeout_ms) {}

Result<IpcEnvelope> IpcClient::call(
    const IpcEnvelope& request) const {

    SystemRuntimeClock clock;
    const auto valid = validateIpcEnvelope(request, clock);
    if (!valid.ok) {
        return Result<IpcEnvelope>::Failure(valid);
    }
    const auto encoded = encodeIpcEnvelope(request).dump();
#ifdef _WIN32
    const auto name = pipeName(endpoint_);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        pipe = CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const auto native_error = GetLastError();
        if (native_error != ERROR_FILE_NOT_FOUND &&
            native_error != ERROR_PIPE_BUSY) {
            return Result<IpcEnvelope>::Failure(Status::Error(
                "ipc", "IPC_CONNECT_FAILED",
                "named pipe connection failed", true));
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() >= connect_timeout_ms_) {
            return Result<IpcEnvelope>::Failure(Status::Error(
                "ipc", "IPC_ENDPOINT_UNAVAILABLE",
                "named pipe endpoint is unavailable", true));
        }
        if (native_error == ERROR_PIPE_BUSY) {
            (void)WaitNamedPipeW(name.c_str(), 10);
        } else {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_CONNECT_FAILED",
            "named pipe connection failed", true));
    }
    const auto close_pipe = [&pipe]() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    };
    if (!writeFrame(pipe, encoded)) {
        close_pipe();
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_REQUEST_WRITE_FAILED",
            "IPC request could not be written", true,
            SideEffectState::Unknown));
    }
    const auto response = readFrame(pipe);
    close_pipe();
#else
    const auto path = socketPath(endpoint_);
    if (path.string().size() >= sizeof(sockaddr_un::sun_path)) {
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_ENDPOINT_PATH_TOO_LONG",
            "Unix domain socket path exceeds platform limit"));
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_SOCKET_CREATE_FAILED",
            "Unix domain socket could not be created", true));
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(),
                 sizeof(address.sun_path) - 1U);
    const auto start = std::chrono::steady_clock::now();
    while (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) != 0) {
        if (errno != ENOENT && errno != ECONNREFUSED &&
            errno != EINTR) {
            ::close(fd);
            return Result<IpcEnvelope>::Failure(Status::Error(
                "ipc", "IPC_CONNECT_FAILED",
                "Unix domain socket connection failed", true));
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() >= connect_timeout_ms_) {
            ::close(fd);
            return Result<IpcEnvelope>::Failure(Status::Error(
                "ipc", "IPC_ENDPOINT_UNAVAILABLE",
                "Unix domain socket endpoint is unavailable", true));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!writeFrame(fd, encoded)) {
        ::close(fd);
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_REQUEST_WRITE_FAILED",
            "IPC request could not be written", true,
            SideEffectState::Unknown));
    }
    const auto response = readFrame(fd);
    ::close(fd);
#endif
    if (!response.status.ok || !response.value) {
        return Result<IpcEnvelope>::Failure(response.status);
    }
    try {
        const auto decoded = decodeIpcEnvelope(
            nlohmann::json::parse(*response.value));
        if (!decoded.status.ok || !decoded.value) return decoded;
        const auto response_valid =
            validateIpcEnvelope(*decoded.value, clock);
        if (!response_valid.ok) {
            return Result<IpcEnvelope>::Failure(response_valid);
        }
        if (decoded.value->correlation_id != request.message_id ||
            decoded.value->target_endpoint_id !=
                request.source_endpoint_id ||
            decoded.value->source_endpoint_id !=
                request.target_endpoint_id ||
            (endpoint_.process_epoch != 0 &&
             decoded.value->source_process_epoch !=
                 endpoint_.process_epoch)) {
            return Result<IpcEnvelope>::Failure(Status::Error(
                "ipc", "IPC_RESPONSE_IDENTITY_MISMATCH",
                "IPC response does not bind the request identity"));
        }
        return decoded;
    } catch (...) {
        return Result<IpcEnvelope>::Failure(Status::Error(
            "ipc", "IPC_RESPONSE_DECODE_FAILED",
            "IPC response is not valid JSON"));
    }
}

IpcServer::IpcServer(
    IpcEndpoint endpoint,
    std::shared_ptr<IRuntimeClock> clock,
    std::size_t max_payload_bytes)
    : impl_(std::make_unique<Impl>()) {
    impl_->endpoint = std::move(endpoint);
    impl_->clock = std::move(clock);
    impl_->max_payload_bytes = max_payload_bytes;
}

IpcServer::~IpcServer() { close(); }

Status IpcServer::initialize() {

    if (!impl_ || !impl_->clock ||
        impl_->endpoint.endpoint_id.empty() ||
        impl_->endpoint.runtime_directory.empty() ||
        impl_->max_payload_bytes == 0 ||
        impl_->max_payload_bytes > kMaxWireFrameBytes) {
        return Status::Error(
            "ipc", "IPC_SERVER_CONFIGURATION_INVALID",
            "IPC server configuration is invalid");
    }
    if (impl_->initialized) return Status::Ok();
    std::error_code error;
    std::filesystem::create_directories(
        impl_->endpoint.runtime_directory / "ipc", error);
    if (error) {
        return Status::Error(
            "ipc", "IPC_DIRECTORY_CREATE_FAILED",
            "IPC runtime directory could not be created");
    }
#ifndef _WIN32
    impl_->socket_path = socketPath(impl_->endpoint);
    if (impl_->socket_path.string().size() >=
        sizeof(sockaddr_un::sun_path)) {
        return Status::Error(
            "ipc", "IPC_ENDPOINT_PATH_TOO_LONG",
            "Unix domain socket path exceeds platform limit");
    }
    std::filesystem::remove(impl_->socket_path, error);
    impl_->listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) {
        return Status::Error(
            "ipc", "IPC_SOCKET_CREATE_FAILED",
            "Unix domain listen socket could not be created");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path,
                 impl_->socket_path.c_str(),
                 sizeof(address.sun_path) - 1U);
    if (::bind(impl_->listen_fd,
               reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(impl_->listen_fd, 64) != 0) {
        close();
        return Status::Error(
            "ipc", "IPC_ENDPOINT_BIND_FAILED",
            "Unix domain endpoint could not be bound");
    }
    ::chmod(impl_->socket_path.c_str(), 0600);
    const auto flags = ::fcntl(impl_->listen_fd, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(impl_->listen_fd, F_SETFL,
                flags | O_NONBLOCK) != 0) {
        close();
        return Status::Error(
            "ipc", "IPC_ENDPOINT_NONBLOCK_FAILED",
            "Unix domain endpoint could not enter reactor mode");
    }
#endif
    impl_->initialized = true;
    return Status::Ok();
}

Status IpcServer::serve(
    const IpcHandler& handler,
    const std::atomic_bool& stop_requested) {

    if (!impl_ || !impl_->initialized || !handler) {
        return Status::Error(
            "ipc", "IPC_SERVER_NOT_READY",
            "IPC server is not initialized");
    }
    constexpr std::size_t kReactorWorkers = 4;
    std::atomic_bool reactor_failed{false};
    std::mutex failure_mutex;
    Status reactor_status = Status::Ok();
    const auto fail_reactor =
        [&](Status failure) {
            bool expected = false;
            if (reactor_failed.compare_exchange_strong(
                    expected, true)) {
                std::lock_guard<std::mutex> lock(failure_mutex);
                reactor_status = std::move(failure);
            }
        };
    const auto process_connection = [&](auto connection) {
        const auto frame = readFrame(connection);
        IpcEnvelope request;
        Result<IpcEnvelope> response;
        if (!frame.status.ok || !frame.value) {
            request.message_id = "unreadable";
            request.source_endpoint_id = "unknown";
            request.target_endpoint_id =
                impl_->endpoint.endpoint_id;
            request.request_id = "unknown";
            request.trace_id = "unknown";
            request.idempotency_key = "unreadable";
            request.deadline_mono_ns =
                impl_->clock->monotonicNowNs() +
                1'000'000'000LL;
            response = Result<IpcEnvelope>::Failure(
                frame.status);
        } else {
            try {
                const auto decoded = decodeIpcEnvelope(
                    nlohmann::json::parse(*frame.value));
                if (!decoded.status.ok || !decoded.value) {
                    response =
                        Result<IpcEnvelope>::Failure(
                            decoded.status);
                } else {
                    request = *decoded.value;
                    const auto validation = validateIpcEnvelope(
                        request, *impl_->clock,
                        impl_->max_payload_bytes);
                    if (!validation.ok) {
                        response =
                            Result<IpcEnvelope>::Failure(
                                validation);
                    } else if (
                        request.target_endpoint_id !=
                        impl_->endpoint.endpoint_id) {
                        response =
                            Result<IpcEnvelope>::Failure(
                                Status::Error(
                                    "ipc",
                                    "IPC_TARGET_MISMATCH",
                                    "IPC target endpoint does not own this server"));
                    } else {
                        response = handler(request);
                    }
                }
            } catch (...) {
                response = Result<IpcEnvelope>::Failure(
                    Status::Error(
                        "ipc", "IPC_REQUEST_DECODE_FAILED",
                        "IPC request is not valid JSON"));
            }
        }
        IpcEnvelope encoded_response =
            response.status.ok && response.value
                ? *response.value
                : errorResponse(
                      request, response.status,
                      impl_->endpoint.process_epoch);
        if (encoded_response.payload_digest.empty()) {
            sealIpcEnvelope(encoded_response);
        }
        const auto wire =
            encodeIpcEnvelope(encoded_response).dump();
        (void)writeFrame(connection, wire);
    };
    const auto worker = [&]() {
        while (!stop_requested.load() &&
               !reactor_failed.load()) {
#ifdef _WIN32
            const auto name = pipeName(impl_->endpoint);
            HANDLE connection = CreateNamedPipeW(
                name.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                    PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                16, kMaxWireFrameBytes,
                kMaxWireFrameBytes, 0, nullptr);
            if (connection == INVALID_HANDLE_VALUE) {
                fail_reactor(Status::Error(
                    "ipc", "IPC_PIPE_CREATE_FAILED",
                    "named pipe server could not be created",
                    true));
                break;
            }
            {
                std::lock_guard<std::mutex> lock(
                    impl_->active_pipes_mutex);
                impl_->active_pipes.insert(connection);
            }
            bool connected = false;
            while (!stop_requested.load() &&
                   !reactor_failed.load()) {
                if (ConnectNamedPipe(connection, nullptr) !=
                    FALSE) {
                    connected = true;
                    break;
                }
                const auto error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) {
                    connected = true;
                    break;
                }
                if (error != ERROR_PIPE_LISTENING &&
                    error != ERROR_NO_DATA) {
                    break;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
            }
            if (connected) {
                DWORD mode =
                    PIPE_READMODE_BYTE | PIPE_WAIT;
                if (SetNamedPipeHandleState(
                        connection, &mode, nullptr,
                        nullptr)) {
                    process_connection(connection);
                    FlushFileBuffers(connection);
                    DisconnectNamedPipe(connection);
                }
            }
            {
                std::lock_guard<std::mutex> lock(
                    impl_->active_pipes_mutex);
                impl_->active_pipes.erase(connection);
            }
            CloseHandle(connection);
#else
            pollfd descriptor{};
            descriptor.fd = impl_->listen_fd;
            descriptor.events = POLLIN;
            const auto ready =
                ::poll(&descriptor, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) continue;
                fail_reactor(Status::Error(
                    "ipc", "IPC_POLL_FAILED",
                    "Unix domain reactor poll failed", true));
                break;
            }
            if (ready == 0 ||
                (descriptor.revents & POLLIN) == 0) {
                continue;
            }
            const int connection =
                ::accept(impl_->listen_fd, nullptr, nullptr);
            if (connection < 0) {
                if (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                    continue;
                }
                if (stop_requested.load()) break;
                fail_reactor(Status::Error(
                    "ipc", "IPC_ACCEPT_FAILED",
                    "Unix domain connection accept failed",
                    true));
                break;
            }
            process_connection(connection);
            ::close(connection);
#endif
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(kReactorWorkers);
    for (std::size_t index = 0;
         index < kReactorWorkers; ++index) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }
    if (reactor_failed.load()) {
        std::lock_guard<std::mutex> lock(failure_mutex);
        return reactor_status;
    }
    return Status::Ok();
}

void IpcServer::close() {
    if (!impl_) return;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(
        impl_->active_pipes_mutex);
    for (const auto pipe : impl_->active_pipes) {
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    impl_->active_pipes.clear();
#else
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (!impl_->socket_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(impl_->socket_path, ignored);
    }
#endif
    impl_->initialized = false;
}

ProcessSupervisor::ProcessSupervisor() = default;

ProcessSupervisor::~ProcessSupervisor() {
    std::vector<ChildProcess> children;
    children.reserve(children_.size());
    for (const auto& pair : children_) {
        children.push_back(pair.second);
    }
    for (const auto& child : children) {
        if (isAlive(child)) {
            (void)terminate(child);
        }
        const auto waited = wait(child, 2000);
#ifdef _WIN32
        if (!waited.ok && child.native_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(
                child.native_handle));
        }
#endif
    }
    children_.clear();
}

ProcessRegistry::ProcessRegistry(
    std::filesystem::path runtime_directory)
    : registry_path_(
          std::move(runtime_directory) / "process_registry.json") {}

Status ProcessRegistry::publish(
    const std::vector<ChildProcess>& children,
    const ChildProcess& supervisor) {

    if (supervisor.endpoint_id.empty() ||
        supervisor.process_epoch == 0 ||
        supervisor.native_process_id == 0) {
        return Status::Error(
            "ipc", "PROCESS_REGISTRY_SUPERVISOR_INVALID",
            "supervisor identity is incomplete");
    }
    nlohmann::json entries = nlohmann::json::array();
    const auto append = [&entries](const ChildProcess& child) {
        entries.push_back(
            {{"endpoint_id", child.endpoint_id},
             {"process_epoch", child.process_epoch},
             {"native_process_id", child.native_process_id}});
    };
    append(supervisor);
    std::set<std::string> endpoints{supervisor.endpoint_id};
    for (const auto& child : children) {
        if (child.endpoint_id.empty() ||
            child.process_epoch == 0 ||
            child.native_process_id == 0 ||
            !endpoints.insert(child.endpoint_id).second) {
            return Status::Error(
                "ipc", "PROCESS_REGISTRY_CHILD_INVALID",
                "child identity is incomplete or duplicated");
        }
        append(child);
    }
    nlohmann::json document{
        {"schema_version", 2},
        {"entries", entries},
        {"digest", secureDigest(entries.dump())}};
    std::error_code error;
    std::filesystem::create_directories(
        registry_path_.parent_path(), error);
    if (error) {
        return Status::Error(
            "ipc", "PROCESS_REGISTRY_DIRECTORY_FAILED",
            "process registry directory could not be created");
    }
    auto temporary = registry_path_;
    temporary += ".tmp-" +
                 std::to_string(currentProcessId());
    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output ||
            !(output << document.dump(2) << '\n') ||
            !output.flush()) {
            return Status::Error(
                "ipc", "PROCESS_REGISTRY_WRITE_FAILED",
                "process registry could not be written");
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.wstring().c_str(),
            registry_path_.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return Status::Error(
            "ipc", "PROCESS_REGISTRY_REPLACE_FAILED",
            "process registry could not be atomically replaced");
    }
#else
    std::filesystem::rename(temporary, registry_path_, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return Status::Error(
            "ipc", "PROCESS_REGISTRY_REPLACE_FAILED",
            "process registry could not be atomically replaced");
    }
#endif
    return Status::Ok();
}

Result<std::map<std::string, ChildProcess>>
ProcessRegistry::load() const {

    try {
        std::ifstream input(registry_path_, std::ios::binary);
        if (!input) {
            throw std::runtime_error("registry is unavailable");
        }
        const auto document = nlohmann::json::parse(input);
        if (!document.is_object() || document.size() != 3 ||
            document.at("schema_version").get<std::uint32_t>() !=
                2 ||
            !document.at("entries").is_array() ||
            document.at("digest").get<std::string>() !=
                secureDigest(document.at("entries").dump())) {
            throw std::runtime_error("registry seal mismatch");
        }
        std::map<std::string, ChildProcess> result;
        for (const auto& encoded : document.at("entries")) {
            if (!encoded.is_object() || encoded.size() != 3) {
                throw std::runtime_error("registry entry mismatch");
            }
            ChildProcess child;
            child.endpoint_id =
                encoded.at("endpoint_id").get<std::string>();
            child.process_epoch =
                encoded.at("process_epoch").get<std::uint64_t>();
            child.native_process_id =
                encoded.at("native_process_id")
                    .get<std::uint64_t>();
            if (child.endpoint_id.empty() ||
                child.process_epoch == 0 ||
                child.native_process_id == 0 ||
                !result.emplace(child.endpoint_id, child).second) {
                throw std::runtime_error(
                    "registry identity invalid");
            }
        }
        return Result<std::map<std::string, ChildProcess>>::
            Success(std::move(result));
    } catch (...) {
        return Result<std::map<std::string, ChildProcess>>::
            Failure(Status::Error(
                "ipc", "PROCESS_REGISTRY_INVALID",
                "process registry is absent, malformed or unsealed",
                true));
    }
}

Status ProcessRegistry::authenticate(
    const std::string& endpoint_id,
    std::uint64_t process_epoch) const {
    const auto loaded = load();
    if (!loaded.status.ok || !loaded.value) {
        return loaded.status;
    }
    const auto found = loaded.value->find(endpoint_id);
    if (found == loaded.value->end() ||
        found->second.process_epoch != process_epoch) {
        return Status::Error(
            "ipc", "PROCESS_EPOCH_FENCED",
            "source endpoint or process epoch is not registered");
    }
    return Status::Ok();
}

std::uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

Result<ChildProcess> ProcessSupervisor::spawn(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::string& endpoint_id,
    std::uint64_t process_epoch) {

    if (executable.empty() || endpoint_id.empty() ||
        process_epoch == 0 ||
        !std::filesystem::exists(executable)) {
        return Result<ChildProcess>::Failure(Status::Error(
            "ipc", "SUPERVISOR_SPAWN_ARGUMENT_INVALID",
            "child process identity or executable is invalid"));
    }
#ifdef _WIN32
    auto command = quoteWindowsArgument(executable.wstring());
    for (const auto& argument : arguments) {
        const auto wide = utf8ToWide(argument);
        if (wide.empty() && !argument.empty()) {
            return Result<ChildProcess>::Failure(Status::Error(
                "ipc", "SUPERVISOR_ARGUMENT_ENCODING_FAILED",
                "child argument is not valid UTF-8"));
        }
        command += L" " + quoteWindowsArgument(wide);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(
        command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(
            executable.wstring().c_str(),
            mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr,
            executable.parent_path().wstring().c_str(),
            &startup, &process)) {
        return Result<ChildProcess>::Failure(Status::Error(
            "ipc", "SUPERVISOR_SPAWN_FAILED",
            "CreateProcess failed", true));
    }
    CloseHandle(process.hThread);
    ChildProcess child;
    child.endpoint_id = endpoint_id;
    child.process_epoch = process_epoch;
    child.native_process_id = process.dwProcessId;
    child.native_handle =
        reinterpret_cast<std::uintptr_t>(process.hProcess);
#else
    const auto child_pid = ::fork();
    if (child_pid < 0) {
        return Result<ChildProcess>::Failure(Status::Error(
            "ipc", "SUPERVISOR_SPAWN_FAILED",
            "fork failed", true));
    }
    if (child_pid == 0) {
        std::vector<std::string> storage;
        storage.push_back(executable.string());
        storage.insert(
            storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : storage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }
    ChildProcess child;
    child.endpoint_id = endpoint_id;
    child.process_epoch = process_epoch;
    child.native_process_id =
        static_cast<std::uint64_t>(child_pid);
    child.native_handle =
        static_cast<std::uintptr_t>(child_pid);
#endif
    children_[child.native_process_id] = child;
    return Result<ChildProcess>::Success(child);
}

bool ProcessSupervisor::isAlive(
    const ChildProcess& child) const {
#ifdef _WIN32
    if (child.native_handle == 0) return false;
    return WaitForSingleObject(
               reinterpret_cast<HANDLE>(child.native_handle), 0) ==
           WAIT_TIMEOUT;
#else
    if (child.native_process_id == 0) return false;
    return ::kill(
               static_cast<pid_t>(child.native_process_id), 0) == 0 ||
           errno == EPERM;
#endif
}

Status ProcessSupervisor::terminate(
    const ChildProcess& child,
    std::uint32_t exit_code) {
#ifdef _WIN32
    if (child.native_handle == 0 ||
        !TerminateProcess(
            reinterpret_cast<HANDLE>(child.native_handle),
            exit_code)) {
        return Status::Error(
            "ipc", "SUPERVISOR_TERMINATE_FAILED",
            "child process could not be terminated", true);
    }
#else
    (void)exit_code;
    if (child.native_process_id == 0 ||
        ::kill(static_cast<pid_t>(child.native_process_id),
               SIGTERM) != 0) {
        return Status::Error(
            "ipc", "SUPERVISOR_TERMINATE_FAILED",
            "child process could not be terminated", true);
    }
#endif
    return Status::Ok();
}

Status ProcessSupervisor::wait(
    const ChildProcess& child, std::uint32_t timeout_ms,
    std::optional<std::uint32_t>* exit_code) {
#ifdef _WIN32
    if (child.native_handle == 0) {
        return Status::Error(
            "ipc", "SUPERVISOR_WAIT_INVALID",
            "child process handle is invalid");
    }
    const auto waited = WaitForSingleObject(
        reinterpret_cast<HANDLE>(child.native_handle),
        timeout_ms);
    if (waited == WAIT_TIMEOUT) {
        return Status::Error(
            "ipc", "SUPERVISOR_WAIT_TIMEOUT",
            "child process did not exit before the deadline", true);
    }
    if (waited != WAIT_OBJECT_0) {
        return Status::Error(
            "ipc", "SUPERVISOR_WAIT_FAILED",
            "child process wait failed", true);
    }
    DWORD native_exit = 0;
    if (!GetExitCodeProcess(
            reinterpret_cast<HANDLE>(child.native_handle),
            &native_exit)) {
        return Status::Error(
            "ipc", "SUPERVISOR_EXIT_CODE_FAILED",
            "child exit code could not be read");
    }
    if (exit_code) *exit_code = native_exit;
    CloseHandle(
        reinterpret_cast<HANDLE>(child.native_handle));
#else
    const auto start = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        const auto waited =
            ::waitpid(static_cast<pid_t>(child.native_process_id),
                      &status, WNOHANG);
        if (waited == static_cast<pid_t>(
                          child.native_process_id)) {
            if (exit_code) {
                if (WIFEXITED(status)) {
                    *exit_code =
                        static_cast<std::uint32_t>(
                            WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    *exit_code =
                        static_cast<std::uint32_t>(
                            128 + WTERMSIG(status));
                }
            }
            break;
        }
        if (waited < 0) {
            return Status::Error(
                "ipc", "SUPERVISOR_WAIT_FAILED",
                "waitpid failed", true);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() >= timeout_ms) {
            return Status::Error(
                "ipc", "SUPERVISOR_WAIT_TIMEOUT",
                "child process did not exit before the deadline", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    children_.erase(child.native_process_id);
    return Status::Ok();
}

}  // namespace master_agent::ipc
