/**
 * @file types.cpp
 * @brief Implements shared identity, deadline, digest, and priority primitives.
 */

#include "master_agent/common/types.h"

#include <array>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "vehicle_memory/checksum.h"

namespace master_agent {
namespace {

std::string bootNonce() {
    std::array<unsigned char, 16> bytes{};
    bool generated = false;
#ifdef _WIN32
    generated =
        ::BCryptGenRandom(
            nullptr, bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    const int descriptor = ::open("/dev/urandom", O_RDONLY);
    if (descriptor >= 0) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::read(
                descriptor, bytes.data() + offset,
                bytes.size() - offset);
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        generated = offset == bytes.size();
        (void)::close(descriptor);
    }
#endif
    if (!generated) {
        std::random_device random;
        for (auto& byte : bytes) {
            byte = static_cast<unsigned char>(random());
        }
    }
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        encoded << std::setw(2)
                << static_cast<unsigned int>(byte);
    }
    return encoded.str();
}

}  // namespace

CallContext::CallContext(
    CallerModuleId caller_module, std::string request, std::string trace,
    std::string principal_hash, TaskPriority task_priority,
    std::int64_t absolute_deadline_mono_ns, std::string endpoint,
    std::uint64_t process_epoch, std::string authorization)
    : caller(caller_module),
      request_id(std::move(request)),
      trace_id(std::move(trace)),
      principal_id_hash(std::move(principal_hash)),
      priority(task_priority),
      deadline_mono_ns(absolute_deadline_mono_ns),
      caller_endpoint_id(
          endpoint.empty() && caller_module != CallerModuleId::Invalid
              ? hostModuleEndpoint(caller_module)
              : std::move(endpoint)),
      caller_process_epoch(
          process_epoch == 0 && caller_module != CallerModuleId::Invalid
              ? hostModuleProcessEpoch(caller_module)
              : process_epoch),
      authorization_ref(std::move(authorization)) {}

std::string hostModuleEndpoint(CallerModuleId module) {
    return module == CallerModuleId::Invalid
               ? std::string{}
               : "inproc:" + toString(module);
}

std::uint64_t hostModuleProcessEpoch(CallerModuleId module) {
    if (module == CallerModuleId::Invalid) return 0;
    static const std::uint64_t boot_epoch = [] {
        const auto nonce = bootNonce();
        auto value = static_cast<std::uint64_t>(
            std::stoull(nonce.substr(0, 16), nullptr, 16));
        return value == 0 ? std::uint64_t{1} : value;
    }();
    const auto ordinal =
        static_cast<std::uint64_t>(module);
    auto value =
        boot_epoch ^ (ordinal * 0x9e3779b97f4a7c15ULL);
    return value == 0 ? std::uint64_t{1} : value;
}

bool hasHostModuleIdentity(const CallContext& call,
                           CallerModuleId expected_module) {
    return call.caller == expected_module &&
           call.caller_endpoint_id ==
               hostModuleEndpoint(expected_module) &&
           call.caller_process_epoch ==
               hostModuleProcessEpoch(expected_module);
}

CallContext makeChildCallContext(const CallContext& parent,
                                 CallerModuleId child_module) {
    return {child_module,
            parent.request_id,
            parent.trace_id,
            parent.principal_id_hash,
            parent.priority,
            parent.deadline_mono_ns,
            hostModuleEndpoint(child_module),
            hostModuleProcessEpoch(child_module),
            parent.authorization_ref};
}

Status Status::Ok() {
    return {};
}

Status Status::Error(std::string domain, std::string code,
                     std::string message, bool retryable,
                     SideEffectState side_effect_state) {
    Status status;
    status.ok = false;
    status.error.domain = std::move(domain);
    status.error.code = std::move(code);
    status.error.message = std::move(message);
    status.error.retryable = retryable;
    status.error.side_effect_state = side_effect_state;
    status.error.source_module = status.error.domain;
    status.error.recoverable_hint = retryable;
    status.error.safe_detail_code = status.error.code;
    status.error.safe_detail_summary = status.error.message;
    return status;
}

std::int64_t SystemRuntimeClock::utcNowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t SystemRuntimeClock::monotonicNowNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ManualRuntimeClock::ManualRuntimeClock(std::int64_t utc_ms,
                                       std::int64_t mono_ns)
    : utc_ms_(utc_ms), mono_ns_(mono_ns) {}

std::int64_t ManualRuntimeClock::utcNowMs() const {
    return utc_ms_.load();
}

std::int64_t ManualRuntimeClock::monotonicNowNs() const {
    return mono_ns_.load();
}

void ManualRuntimeClock::advanceMs(std::int64_t delta_ms) {
    utc_ms_.fetch_add(delta_ms);
    mono_ns_.fetch_add(delta_ms * 1000000LL);
}

IdGenerator::IdGenerator(std::string boot_prefix)
    : boot_prefix_(std::move(boot_prefix)) {
    // OS entropy prevents identical clocks and sequence positions from
    // reissuing IDs across process restarts. The instance counter also keeps
    // deterministic same-process stress fixtures distinct.
    static std::atomic<std::uint64_t> runtime_instance{1};
    boot_prefix_ += "-nonce-" + bootNonce() + "-instance-" +
                    std::to_string(runtime_instance.fetch_add(1));
}

std::string IdGenerator::next(const std::string& kind) {
    return kind + "-" + boot_prefix_ + "-" +
           std::to_string(sequence_.fetch_add(1));
}

bool isHigherPriority(TaskPriority left, TaskPriority right) {
    return static_cast<std::uint8_t>(left) <
           static_cast<std::uint8_t>(right);
}

bool isValidTaskPriority(TaskPriority priority) {
    return priority == TaskPriority::P0 ||
           priority == TaskPriority::P1 ||
           priority == TaskPriority::P2;
}

bool isValidSideEffectState(SideEffectState state) {
    return state == SideEffectState::NotApplicable ||
           state == SideEffectState::NotStarted ||
           state == SideEffectState::Committed ||
           state == SideEffectState::ConfirmedNotExecuted ||
           state == SideEffectState::Unknown ||
           state == SideEffectState::Compensated;
}

bool deadlineExpired(std::int64_t deadline_mono_ns,
                     const IRuntimeClock& clock) {
    return deadline_mono_ns > 0 &&
           clock.monotonicNowNs() >= deadline_mono_ns;
}

std::string toString(CallerModuleId module) {
    switch (module) {
        case CallerModuleId::Invalid:
            return "Invalid";
        case CallerModuleId::InteractionIngress:
            return "InteractionIngress";
        case CallerModuleId::AgentService:
            return "AgentService";
        case CallerModuleId::IntentRecognitionEngine:
            return "IntentRecognitionEngine";
        case CallerModuleId::PreprocessingEngine:
            return "PreprocessingEngine";
        case CallerModuleId::MemoryService:
            return "MemoryService";
        case CallerModuleId::SkillEngine:
            return "SkillEngine";
        case CallerModuleId::PromptEngine:
            return "PromptEngine";
        case CallerModuleId::InferenceFramework:
            return "InferenceFramework";
        case CallerModuleId::TaskOrchestrationEngine:
            return "TaskOrchestrationEngine";
        case CallerModuleId::AgentDispatch:
            return "AgentDispatch";
        case CallerModuleId::SubAgent:
            return "SubAgent";
        case CallerModuleId::AtomicServiceManager:
            return "AtomicServiceManager";
        case CallerModuleId::KvCacheManager:
            return "KvCacheManager";
        case CallerModuleId::DataLogManager:
            return "DataLogManager";
        case CallerModuleId::ExceptionManager:
            return "ExceptionManager";
    }
    return "UnknownModule";
}

std::string toString(TaskPriority priority) {
    switch (priority) {
        case TaskPriority::P0:
            return "P0";
        case TaskPriority::P1:
            return "P1";
        case TaskPriority::P2:
            return "P2";
    }
    return "P2";
}

std::string toString(SideEffectState state) {
    switch (state) {
        case SideEffectState::NotApplicable:
            return "NOT_APPLICABLE";
        case SideEffectState::NotStarted:
            return "NOT_STARTED";
        case SideEffectState::Committed:
            return "COMMITTED";
        case SideEffectState::ConfirmedNotExecuted:
            return "CONFIRMED_NOT_EXECUTED";
        case SideEffectState::Unknown:
            return "UNKNOWN";
        case SideEffectState::Compensated:
            return "COMPENSATED";
    }
    return "UNKNOWN";
}

std::string stableDigest(const std::string& value) {
    // FNV-1a is deterministic across processes and standard libraries.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string secureDigest(const std::string& value) {
    return vehicle_memory::Sha256Hex(value);
}

std::string scopedIdempotencyLedgerKey(
    const std::string& principal_id_hash,
    const std::string& idempotency_key) {
    const auto framed =
        std::to_string(principal_id_hash.size()) + ":" +
        principal_id_hash +
        std::to_string(idempotency_key.size()) + ":" +
        idempotency_key;
    return secureDigest(framed);
}

}  // namespace master_agent
