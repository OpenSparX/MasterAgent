/**
 * @file memory_service.cpp
 * @brief Implements bounded short-term context and idempotent turn persistence.
 */

#include "master_agent/memory/memory_service.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#include "vehicle_memory/atomic_file_writer.h"
#include "vehicle_memory/conversation_journal.h"

namespace master_agent::memory {
namespace {

constexpr std::size_t kMaximumIdentityBytes = 256U;
constexpr std::size_t kMaximumContextSourceBytes = 2048U;
constexpr std::size_t kUtf8BytesPerTokenBudgetUnit = 4U;

Status validateAgentService(const CallContext& call) {
    if (!hasHostModuleIdentity(call, CallerModuleId::AgentService)) {
        return Status::Error("memory", "MEMORY_CALLER_NOT_ALLOWED",
                             "Memory Service is called through AgentService");
    }
    return Status::Ok();
}

Status validateMemoryReader(const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService) &&
        !hasHostModuleIdentity(
            call,
            CallerModuleId::IntentRecognitionEngine)) {
        return Status::Error(
            "memory", "MEMORY_CALLER_NOT_ALLOWED",
            "Memory reads require AgentService or IntentEngine");
    }
    return Status::Ok();
}

std::string storageCode(vehicle_memory::MemoryErrorCode code) {
    using Code = vehicle_memory::MemoryErrorCode;
    switch (code) {
        case Code::kNone:
            return "";
        case Code::kConfigurationError:
            return "configuration_error";
        case Code::kStorageCorrupted:
            return "storage_corrupted";
        case Code::kStorageCommitFailed:
            return "storage_commit_failed";
        case Code::kRequestInvalid:
            return "invalid_request";
        case Code::kRevisionConflict:
            return "revision_conflict";
        default:
            return "memory_error";
    }
}

Status validateMemoryIdentity(const std::string& user_id,
                              const std::string& session_id) {
    if (user_id.empty() || session_id.empty()) {
        return Status::Error(
            "memory", "MEMORY_IDENTITY_INVALID",
            "memory user_id and session_id must not be empty");
    }
    if (user_id.find(':') != std::string::npos ||
        session_id.find(':') != std::string::npos) {
        return Status::Error(
            "memory", "MEMORY_IDENTITY_INVALID",
            "memory user_id and session_id must not contain ':'");
    }
    if (user_id.size() > kMaximumIdentityBytes ||
        session_id.size() > kMaximumIdentityBytes) {
        return Status::Error(
            "memory", "MEMORY_IDENTITY_TOO_LARGE",
            "memory user_id and session_id must not exceed 256 UTF-8 bytes");
    }
    return Status::Ok();
}

bool isKnownStorageFailure(const std::string& code) {
    return code == "configuration_error" ||
           code == "storage_corrupted" ||
           code == "storage_commit_failed" ||
           code == "invalid_request" ||
           code == "revision_conflict";
}

SideEffectState writeFailureSideEffect(const std::string& code) {
    if (code == "storage_commit_failed" ||
        !isKnownStorageFailure(code)) {
        return SideEffectState::Unknown;
    }
    return SideEffectState::NotStarted;
}

std::string expectedMemoryId(
    const master_agent::memory::WriteMemoryRequest& request) {
    return request.user_id + ":" + request.session_id + ":" +
           std::to_string(request.turn_id) + ":" +
           std::to_string(request.record_version);
}

bool validSourceMemoryId(
    const master_agent::memory::ContextBlock& block,
    const master_agent::memory::GetContextRequest& request) {
    const auto prefix =
        "short-term:" + request.user_id + ":" +
        request.session_id + ":" + std::to_string(block.turn_id) + ":";
    if (block.source_memory_id.size() > kMaximumContextSourceBytes ||
        block.source_memory_id.rfind(prefix, 0) != 0) {
        return false;
    }
    const auto version =
        block.source_memory_id.substr(prefix.size());
    return !version.empty() &&
           std::all_of(version.begin(), version.end(),
                       [](unsigned char byte) {
                           return byte >= '0' && byte <= '9';
                       }) &&
           std::any_of(version.begin(), version.end(),
                       [](unsigned char byte) {
                           return byte >= '1' && byte <= '9';
                       });
}

/// The token budget is conservatively converted to UTF-8 bytes (4 bytes per
/// budget unit), and the flattened separator bytes are included.
Result<MemoryContext> validateContextResult(
    const master_agent::memory::GetContextResult& result,
    const master_agent::memory::GetContextRequest& request) {
    if (!result.error_code.empty() || !result.error_message.empty()) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CONTEXT_RESULT_INVALID",
            "memory context success contains contradictory error fields"));
    }
    if (request.max_turns <= 0 ||
        result.context_blocks.size() >
            static_cast<std::size_t>(request.max_turns)) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CONTEXT_RESULT_INVALID",
            "memory context result exceeds the requested turn limit"));
    }

    const auto positive_budget =
        request.token_budget > 0 ? request.token_budget : 0;
    const auto byte_budget =
        static_cast<std::size_t>(positive_budget) *
        kUtf8BytesPerTokenBudgetUnit;
    std::size_t used_bytes = 0;
    std::set<std::string> source_ids;
    std::set<std::pair<std::string, std::uint64_t>> turn_ids;
    MemoryContext context;
    std::ostringstream flattened;
    for (std::size_t index = 0;
         index < result.context_blocks.size(); ++index) {
        const auto& block = result.context_blocks[index];
        const bool metadata_valid =
            block.memory_type == "short_term" &&
            !block.content.empty() &&
            block.session_id == request.session_id &&
            block.turn_id != 0 &&
            block.timestamp_ms != 0 &&
            block.timestamp_ms <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) &&
            std::isfinite(block.relevance_score) &&
            block.relevance_score >= 0.0 &&
            block.relevance_score <= 1.0 &&
            validSourceMemoryId(block, request);
        const bool unique =
            source_ids.insert(block.source_memory_id).second &&
            turn_ids.emplace(block.session_id, block.turn_id).second;
        const std::size_t separator_bytes = index == 0 ? 0U : 5U;
        const bool within_budget =
            used_bytes <= byte_budget &&
            separator_bytes <= byte_budget - used_bytes &&
            block.content.size() <=
                byte_budget - used_bytes - separator_bytes;
        if (!metadata_valid || !unique || !within_budget) {
            return Result<MemoryContext>::Failure(Status::Error(
                "memory", "MEMORY_CONTEXT_RESULT_INVALID",
                "memory context result failed its response seal"));
        }
        if (separator_bytes != 0U) flattened << "\n---\n";
        flattened << block.content;
        used_bytes += separator_bytes + block.content.size();
        context.blocks.push_back(block);
    }
    context.flattened_context = flattened.str();
    return Result<MemoryContext>::Success(std::move(context));
}

class JournalMemoryClient final : public master_agent::memory::IMemoryClient {
public:
    explicit JournalMemoryClient(
        std::shared_ptr<vehicle_memory::ConversationJournal> journal,
        vehicle_memory::MemoryError initialization_error)
        : journal_(std::move(journal)),
          initialization_error_(std::move(initialization_error)) {}

    master_agent::memory::WriteMemoryResult writeMemory(
        const master_agent::memory::WriteMemoryRequest& request) override {
        master_agent::memory::WriteMemoryResult result;
        if (initialization_error_) {
            result.error_code = storageCode(initialization_error_.code);
            result.error_message = initialization_error_.message;
            return result;
        }
        vehicle_memory::ConversationTurn turn;
        turn.user_id = request.user_id;
        turn.session_id = request.session_id;
        turn.turn_id = request.turn_id;
        turn.record_version = request.record_version;
        turn.timestamp_ms = request.event_time;
        turn.user_text = request.user_input;
        turn.assistant_text = request.assistant_output;
        turn.scene = request.scene;
        const auto appended = journal_->Append(turn);
        if (appended.error) {
            result.error_code = storageCode(appended.error.code);
            result.error_message = appended.error.message;
            return result;
        }
        result.success = true;
        result.memory_id = request.user_id + ":" + request.session_id + ":" +
                           std::to_string(request.turn_id) + ":" +
                           std::to_string(request.record_version);
        result.revision = appended.revision;
        result.idempotent = appended.idempotent;
        return result;
    }

    master_agent::memory::GetContextResult getContext(
        const master_agent::memory::GetContextRequest& request) override {
        master_agent::memory::GetContextResult result;
        if (initialization_error_) {
            result.error_code = storageCode(initialization_error_.code);
            result.error_message = initialization_error_.message;
            return result;
        }
        const auto recent = journal_->Recent(
            request.user_id, request.session_id,
            static_cast<std::size_t>(request.max_turns));
        if (recent.error) {
            result.error_code = storageCode(recent.error.code);
            result.error_message = recent.error.message;
            return result;
        }
        for (const auto& turn : recent.turns) {
            master_agent::memory::ContextBlock block;
            block.content = "USER_ORIGINAL: " + turn.user_text +
                            "\nVEHICLE_REPLY: " + turn.assistant_text;
            block.source_memory_id =
                "short-term:" + turn.user_id + ":" + turn.session_id + ":" +
                std::to_string(turn.turn_id) + ":" +
                std::to_string(turn.record_version);
            block.session_id = turn.session_id;
            block.turn_id = turn.turn_id;
            block.timestamp_ms = turn.timestamp_ms;
            result.context_blocks.push_back(std::move(block));
        }
        result.success = true;
        return result;
    }

private:
    std::shared_ptr<vehicle_memory::ConversationJournal> journal_;
    vehicle_memory::MemoryError initialization_error_;
};

}  // namespace

MemoryService::MemoryService(
    std::shared_ptr<master_agent::memory::IMemoryClient> client,
    std::shared_ptr<IRuntimeClock> clock)
    : client_(std::move(client)), clock_(std::move(clock)) {}

Result<MemoryContext> MemoryService::getContext(
    const interaction::StandardRequest& request,
    const std::string& normalized_query, const CallContext& call) {

    const auto caller = validateMemoryReader(call);
    if (!caller.ok) {
        return Result<MemoryContext>::Failure(caller);
    }
    if (!client_ || !clock_) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_NOT_READY", "memory client is not configured"));
    }
    const auto identity =
        validateMemoryIdentity(request.user_id, request.session_id);
    if (!identity.ok) {
        return Result<MemoryContext>::Failure(identity);
    }
    if (call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns ||
        call.deadline_mono_ns <= 0) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CALL_IDENTITY_INVALID",
            "memory recall must bind the active request"));
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CALL_EXPIRED",
            "memory recall deadline expired before external I/O"));
    }
    master_agent::memory::GetContextRequest query;
    query.user_id = request.user_id;
    query.session_id = request.session_id;
    query.query = normalized_query;
    query.scene = "cockpit";
    query.max_turns = 3;
    query.token_budget = 256;
    master_agent::memory::GetContextResult result;
    try {
        result = client_->getContext(query);
    } catch (const std::exception&) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CONTEXT_CLIENT_EXCEPTION",
            "memory context provider raised an exception", true,
            SideEffectState::NotApplicable));
    } catch (...) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CONTEXT_CLIENT_EXCEPTION",
            "memory client raised a non-standard exception", true,
            SideEffectState::NotApplicable));
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", "MEMORY_CONTEXT_RESULT_AFTER_DEADLINE",
            "late memory context cannot be consumed by this turn",
            false, SideEffectState::NotApplicable));
    }
    if (!result.success) {
        const auto code =
            isKnownStorageFailure(result.error_code)
                ? result.error_code
                : std::string{"MEMORY_CONTEXT_FAILED"};
        return Result<MemoryContext>::Failure(Status::Error(
            "memory", code,
            "memory context provider rejected the request",
            result.error_code == "storage_commit_failed",
            SideEffectState::NotApplicable));
    }
    return validateContextResult(result, query);
}

Status MemoryService::writeTurn(const CompletedTurn& turn,
                                   const CallContext& call) {

    const auto caller = validateAgentService(call);
    if (!caller.ok) {
        return caller;
    }
    if (!client_ || !clock_) {
        return Status::Error("memory", "MEMORY_NOT_READY",
                             "memory client is not configured");
    }
    const auto identity = validateMemoryIdentity(
        turn.request.user_id, turn.request.session_id);
    if (!identity.ok) return identity;
    if (call.request_id != turn.request.request_id ||
        call.trace_id != turn.request.trace_id ||
        call.priority != turn.request.priority ||
        call.deadline_mono_ns !=
            turn.request.deadline_mono_ns ||
        call.deadline_mono_ns <= 0) {
        return Status::Error(
            "memory", "MEMORY_CALL_IDENTITY_INVALID",
            "memory write must bind the completed turn");
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "memory", "MEMORY_CALL_EXPIRED",
            "memory write deadline expired before external I/O");
    }

    if (turn.request.turn_id == 0 ||
        turn.request.timestamp_utc_ms <= 0 ||
        turn.record_version == 0 ||
        turn.normalized_user_input.empty() ||
        turn.normalized_user_input.size() > 64U * 1024U ||
        turn.assistant_output.size() > 64U * 1024U ||
        turn.scene.size() > 256U) {
        return Status::Error(
            "memory", "MEMORY_WRITE_REQUEST_INVALID",
            "completed turn does not satisfy the memory SDK contract",
            false, SideEffectState::NotStarted);
    }
    master_agent::memory::WriteMemoryRequest request;
    request.user_id = turn.request.user_id;
    request.session_id = turn.request.session_id;
    request.turn_id = turn.request.turn_id;
    request.record_version = turn.record_version;
    request.event_time = static_cast<std::uint64_t>(
        turn.request.timestamp_utc_ms);
    request.user_input = turn.normalized_user_input;
    request.assistant_output = turn.assistant_output;
    request.scene = turn.scene;
    master_agent::memory::WriteMemoryResult result;
    try {
        result = client_->writeMemory(request);
    } catch (const std::exception&) {
        return Status::Error(
            "memory", "MEMORY_WRITE_CLIENT_EXCEPTION",
            "memory write provider raised an exception",
            true, SideEffectState::Unknown);
    } catch (...) {
        return Status::Error(
            "memory", "MEMORY_WRITE_CLIENT_EXCEPTION",
            "memory client raised a non-standard exception", true,
            SideEffectState::Unknown);
    }
    if (result.success &&
        (result.memory_id != expectedMemoryId(request) ||
         result.revision == 0 || !result.error_code.empty() ||
         !result.error_message.empty())) {
        return Status::Error(
            "memory", "MEMORY_WRITE_RESULT_INVALID",
            "memory write acknowledgement failed its response seal",
            false, SideEffectState::Unknown);
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {

        return Status::Error(
            "memory",
            result.success
                ? "MEMORY_WRITE_COMMITTED_AFTER_DEADLINE"
                : "MEMORY_WRITE_RESULT_AFTER_DEADLINE",
            result.success
                ? "memory committed after the caller deadline"
                : "memory write completion is ambiguous after the deadline",
            false,
            result.success ? SideEffectState::Committed
                           : SideEffectState::Unknown);
    }
    if (!result.success) {
        const bool contradictory_failure =
            !result.memory_id.empty() || result.revision != 0 ||
            result.idempotent;
        const auto effect =
            contradictory_failure
                ? SideEffectState::Unknown
                : writeFailureSideEffect(result.error_code);
        const auto code =
            !contradictory_failure &&
                isKnownStorageFailure(result.error_code)
                ? result.error_code
                : std::string{"MEMORY_WRITE_FAILED"};
        return Status::Error(
            "memory", code,
            "memory provider did not confirm the write",
            result.error_code == "storage_commit_failed",
            effect);
    }
    return Status::Ok();
}

std::shared_ptr<master_agent::memory::IMemoryClient>
createJournalMemoryClient(const std::filesystem::path& data_directory) {
    std::error_code directory_error;
    std::filesystem::create_directories(data_directory, directory_error);
    if (directory_error) {
        vehicle_memory::MemoryError error;
        error.code = vehicle_memory::MemoryErrorCode::kConfigurationError;
        error.message = "failed to create memory data directory";
        return std::make_shared<JournalMemoryClient>(nullptr, error);
    }
    auto journal = vehicle_memory::CreateJsonConversationJournal(
        data_directory, "conversation_journal.json",
        vehicle_memory::CreateWindowsAtomicFileWriter());
    const auto loaded = journal->Load();
    return std::make_shared<JournalMemoryClient>(std::move(journal),
                                                  loaded.error);
}

}  // namespace master_agent::memory
