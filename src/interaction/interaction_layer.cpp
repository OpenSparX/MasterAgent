/**
 * @file interaction_layer.cpp
 * @brief Implements validated ingress normalization and turn identity allocation.
 */

#include "master_agent/interaction/interaction_layer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace master_agent::interaction {
namespace {

class TurnFileLock {
public:
    explicit TurnFileLock(const std::filesystem::path& path) {
#ifdef _WIN32
        for (std::size_t attempt = 0; attempt < 5000; ++attempt) {
            handle_ = ::CreateFileW(
                path.wstring().c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                locked_ = true;
                break;
            }
            if (::GetLastError() != ERROR_SHARING_VIOLATION &&
                ::GetLastError() != ERROR_LOCK_VIOLATION) {
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
#else
        descriptor_ =
            ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ >= 0 &&
            ::flock(descriptor_, LOCK_EX) == 0) {
            locked_ = true;
        }
#endif
    }

    TurnFileLock(const TurnFileLock&) = delete;
    TurnFileLock& operator=(const TurnFileLock&) = delete;

    ~TurnFileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            if (locked_) (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

    bool locked() const { return locked_; }

private:
    bool locked_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

bool replaceCounter(const std::filesystem::path& target,
                    const std::filesystem::path& temporary,
                    const std::string& logical_key_digest,
                    std::uint64_t value) {
    const auto checksum = secureDigest(
        "turn-counter-v1|" + logical_key_digest + "|" +
        std::to_string(value));
    const auto encoded =
        nlohmann::json{{"schema_version", 1},
                       {"logical_key_digest", logical_key_digest},
                       {"turn_id", value},
                       {"checksum", checksum}}
            .dump() +
        "\n";
#ifdef _WIN32
    const auto handle = ::CreateFileW(
        temporary.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote =
        ::WriteFile(handle, encoded.data(),
                    static_cast<DWORD>(encoded.size()), &written,
                    nullptr) != FALSE &&
        written == static_cast<DWORD>(encoded.size()) &&
        ::FlushFileBuffers(handle) != FALSE;
    ::CloseHandle(handle);
    if (!wrote) {
        (void)::DeleteFileW(temporary.wstring().c_str());
        return false;
    }
    if (::MoveFileExW(
            temporary.wstring().c_str(), target.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH) == FALSE) {
        (void)::DeleteFileW(temporary.wstring().c_str());
        return false;
    }
    return true;
#else
    const int descriptor =
        ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (descriptor < 0) return false;
    std::size_t offset = 0;
    bool wrote = true;
    while (offset < encoded.size()) {
        const auto count = ::write(
            descriptor, encoded.data() + offset,
            encoded.size() - offset);
        if (count <= 0) {
            wrote = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (wrote) wrote = ::fsync(descriptor) == 0;
    (void)::close(descriptor);
    if (!wrote ||
        ::rename(temporary.c_str(), target.c_str()) != 0) {
        (void)::unlink(temporary.c_str());
        return false;
    }
    const int directory = ::open(
        target.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) return false;
    const bool directory_synced = ::fsync(directory) == 0;
    (void)::close(directory);
    return directory_synced;
#endif
}

Result<std::uint64_t> readCounter(
    const std::filesystem::path& counter,
    const std::string& logical_key_digest) {
    std::ifstream input(counter, std::ios::binary);
    std::string encoded(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    try {
        if (!encoded.empty() && encoded.front() == '{') {
            const auto value = nlohmann::json::parse(encoded);
            const auto turn_id =
                value.at("turn_id").get<std::uint64_t>();
            const auto key =
                value.at("logical_key_digest").get<std::string>();
            const auto checksum =
                value.at("checksum").get<std::string>();
            if (value.at("schema_version").get<int>() != 1 ||
                key != logical_key_digest ||
                checksum != secureDigest(
                    "turn-counter-v1|" + key + "|" +
                    std::to_string(turn_id))) {
                throw std::runtime_error("counter seal mismatch");
            }
            return Result<std::uint64_t>::Success(turn_id);
        }
        std::size_t consumed = 0;
        const auto turn_id =
            static_cast<std::uint64_t>(
                std::stoull(encoded, &consumed, 10));
        const auto first_non_space = std::find_if_not(
            encoded.begin(), encoded.end(), [](unsigned char byte) {
                return std::isspace(byte) != 0;
            });
        if (consumed == 0 || first_non_space == encoded.end() ||
            !std::isdigit(
                static_cast<unsigned char>(*first_non_space)) ||
            !std::all_of(
                encoded.begin() + static_cast<std::ptrdiff_t>(consumed),
                encoded.end(), [](unsigned char byte) {
                    return std::isspace(byte) != 0;
                })) {
            throw std::runtime_error(
                "legacy counter is not a bare unsigned integer");
        }
        return Result<std::uint64_t>::Success(turn_id);
    } catch (...) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_STATE_CORRUPT",
            "persisted turn counter seal is invalid"));
    }
}

}  // namespace

InteractionLayer::InteractionLayer(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::filesystem::path turn_state_directory,
    TurnFloorLookup turn_floor_lookup)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      turn_state_directory_(std::move(turn_state_directory)),
      turn_floor_lookup_(std::move(turn_floor_lookup)) {
    if (!turn_state_directory_.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(
            turn_state_directory_, ignored);
    }
}

Result<std::uint64_t> InteractionLayer::allocateTurn(
    const std::string& user_id, const std::string& session_id,
    const std::string& request_id) {

    const auto logical_key =
        std::to_string(user_id.size()) + ":" + user_id +
        std::to_string(session_id.size()) + ":" + session_id;
    std::shared_ptr<std::mutex> allocation_lane;
    {
        std::lock_guard<std::mutex> registry_lock(
            lane_registry_mutex_);
        for (auto lane = allocation_lanes_.begin();
             lane != allocation_lanes_.end();) {
            if (lane->second.expired()) {
                lane = allocation_lanes_.erase(lane);
            } else {
                ++lane;
            }
        }
        auto& weak_lane = allocation_lanes_[secureDigest(logical_key)];
        allocation_lane = weak_lane.lock();
        if (!allocation_lane) {
            allocation_lane = std::make_shared<std::mutex>();
            weak_lane = allocation_lane;
        }
    }
    std::unique_lock<std::mutex> lane_lock(*allocation_lane);
    if (turn_state_directory_.empty()) {

        std::lock_guard<std::mutex> turns_lock(
            memory_turns_mutex_);
        return Result<std::uint64_t>::Success(
            ++session_turns_[logical_key]);
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        turn_state_directory_, directory_error);
    if (directory_error) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_STATE_UNAVAILABLE",
            "turn state directory cannot be created", true));
    }
    const auto key = secureDigest(logical_key);
    const auto counter =
        turn_state_directory_ / (key + ".counter");
    const auto lock_path =
        turn_state_directory_ / (key + ".lock");
    TurnFileLock file_lock(lock_path);
    if (!file_lock.locked()) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_LOCK_UNAVAILABLE",
            "turn allocator lock cannot be acquired", true));
    }

    std::error_code counter_error;
    const bool counter_exists =
        std::filesystem::exists(counter, counter_error);
    if (counter_error) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_STATE_UNAVAILABLE",
            "turn counter existence cannot be determined", true));
    }

    std::uint64_t current = 0;
    if (counter_exists) {
        const auto persisted = readCounter(counter, key);
        if (!persisted.status.ok || !persisted.value) return persisted;
        current = *persisted.value;
    } else if (turn_floor_lookup_) {

        try {
            const auto floor =
                turn_floor_lookup_(user_id, session_id);
            if (!floor.status.ok || !floor.value ||
                *floor.value ==
                    std::numeric_limits<std::uint64_t>::max()) {
                return Result<std::uint64_t>::Failure(Status::Error(
                    "interaction",
                    "INTERACTION_TURN_MIGRATION_FAILED",
                    "missing turn counter could not be safely recovered "
                    "from memory",
                    true));
            }
            current = *floor.value;
        } catch (...) {
            return Result<std::uint64_t>::Failure(Status::Error(
                "interaction",
                "INTERACTION_TURN_MIGRATION_FAILED",
                "missing turn counter recovery raised an exception",
                true));
        }
    }
    if (current == std::numeric_limits<std::uint64_t>::max()) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_SEQUENCE_EXHAUSTED",
            "turn counter cannot advance"));
    }
    const auto next = current + 1;
    const auto temporary =
        turn_state_directory_ /
        (key + "." + secureDigest(request_id).substr(0, 16) + ".tmp");
    if (!replaceCounter(counter, temporary, key, next)) {
        return Result<std::uint64_t>::Failure(Status::Error(
            "interaction", "INTERACTION_TURN_PERSIST_FAILED",
            "turn counter could not be durably committed", true));
    }
    {
        std::lock_guard<std::mutex> turns_lock(
            memory_turns_mutex_);
        session_turns_[logical_key] = next;
    }
    return Result<std::uint64_t>::Success(next);
}

Result<StandardRequest> InteractionLayer::submitText(
    const TextInput& input) {

    if (!clock_ || !ids_) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_NOT_READY",
            "interaction clock or id generator is not configured"));
    }
    if (input.text.empty()) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_EMPTY_TEXT",
            "text input must not be empty"));
    }
    if (input.text.size() > 16384) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_TEXT_TOO_LARGE",
            "text input must not exceed 16384 UTF-8 bytes"));
    }
    if (input.source.empty()) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_SOURCE_REQUIRED",
            "input source must not be empty"));
    }
    if (input.user_id.size() > 256U) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_USER_ID_TOO_LARGE",
            "user_id must not exceed 256 UTF-8 bytes"));
    }
    if (input.session_id.size() > 256U) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_SESSION_ID_TOO_LARGE",
            "session_id must not exceed 256 UTF-8 bytes"));
    }
    // The external Memory SDK serializes identity as
    // user:session:turn:version; accepting ':' would make that identity
    // non-injective even though the local turn counter key is length-prefixed.
    if (input.user_id.find(':') != std::string::npos ||
        input.session_id.find(':') != std::string::npos) {
        return Result<StandardRequest>::Failure(Status::Error(
            "interaction", "INTERACTION_IDENTITY_DELIMITER_INVALID",
            "user_id and session_id must not contain ':'"));
    }

    StandardRequest request;
    request.request_id = ids_->next("request");
    request.trace_id = ids_->next("trace");
    // Preserve caller parameters verbatim; only the two normalized ingress
    // metadata keys below are owned by this layer.
    request.text = input.text;
    request.params = input.params;
    request.params["input_source"] = input.source;
    request.params["input_type"] = "text";
    request.timestamp_utc_ms = clock_->utcNowMs();
    request.deadline_mono_ns =
        clock_->monotonicNowNs() + 30LL * 1000LL * 1000LL * 1000LL;
    // The interaction design does not require user_id on TextInput. Preserve
    // a supplied identity for Memory integration and use a stable anonymous
    // principal when older clients omit this optional extension.
    request.user_id =
        input.user_id.empty() ? "anonymous" : input.user_id;

    request.session_id =
        input.session_id.empty() ? ids_->next("session")
                                 : input.session_id;
    const auto turn = allocateTurn(
        request.user_id, request.session_id, request.request_id);
    if (!turn.status.ok || !turn.value) {
        return Result<StandardRequest>::Failure(turn.status);
    }
    request.turn_id = *turn.value;

    request.priority = TaskPriority::P1;
    return Result<StandardRequest>::Success(std::move(request));
}

std::unique_ptr<IInteractionLayer> createInteractionLayer() {
    auto clock = std::make_shared<SystemRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "interaction-" + std::to_string(clock->utcNowMs()));
    return std::make_unique<InteractionLayer>(
        std::move(clock), std::move(ids));
}

}  // namespace master_agent::interaction
