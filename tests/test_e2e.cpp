/**
 * @file test_e2e.cpp
 * @brief Verifies complete ingress, intent, execution, response, and memory flows.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "master_agent/runtime/master_agent_runtime.h"
#include "master_agent/common/types.h"
#include "master_agent/data_log/data_log_service.h"
#include "test_support.h"

using master_agent::CallerModuleId;
using master_agent::CallContext;
using master_agent::ManualRuntimeClock;
using master_agent::TaskPriority;
using master_agent::runtime::MasterAgentRuntime;
using master_agent::interaction::TextInput;
using master_agent::test_support::ScopedTempDirectory;
using master_agent::test_support::expect;

namespace {

/// Scriptable SDK double used to verify the Memory Service trust boundary.
class ScriptedMemoryClient final
    : public master_agent::memory::IMemoryClient {
public:
    master_agent::memory::WriteMemoryResult writeMemory(
        const master_agent::memory::WriteMemoryRequest&) override {
        ++write_calls;
        if (throw_write) {
            throw std::runtime_error("raw-write-secret");
        }
        if (advance_write_ms != 0 && clock) {
            clock->advanceMs(advance_write_ms);
        }
        return write_result;
    }

    master_agent::memory::GetContextResult getContext(
        const master_agent::memory::GetContextRequest&) override {
        ++read_calls;
        if (throw_get) {
            throw std::runtime_error("raw-read-secret");
        }
        return get_result;
    }

    std::shared_ptr<ManualRuntimeClock> clock;
    master_agent::memory::WriteMemoryResult write_result;
    master_agent::memory::GetContextResult get_result;
    std::int64_t advance_write_ms = 0;
    bool throw_get = false;
    bool throw_write = false;
    std::size_t read_calls = 0;
    std::size_t write_calls = 0;
};

master_agent::interaction::StandardRequest memoryRequest(
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    master_agent::interaction::StandardRequest request;
    request.request_id = "memory-request";
    request.trace_id = "memory-trace";
    request.user_id = "driver-memory";
    request.session_id = "session-memory";
    request.turn_id = 1;
    request.timestamp_utc_ms = clock->utcNowMs();
    request.priority = TaskPriority::P1;
    request.deadline_mono_ns =
        clock->monotonicNowNs() + 10'000'000'000LL;
    return request;
}

CallContext memoryCall(
    const master_agent::interaction::StandardRequest& request) {
    return {CallerModuleId::AgentService, request.request_id,
            request.trace_id, "memory-principal", request.priority,
            request.deadline_mono_ns};
}

master_agent::memory::ContextBlock validMemoryBlock(
    std::uint64_t turn_id) {
    master_agent::memory::ContextBlock block;
    block.memory_type = "short_term";
    block.content = "remembered turn " + std::to_string(turn_id);
    block.source_memory_id =
        "short-term:driver-memory:session-memory:" +
        std::to_string(turn_id) + ":1";
    block.relevance_score = 1.0;
    block.session_id = "session-memory";
    block.turn_id = turn_id;
    block.timestamp_ms = 1'785'200'000'000ULL + turn_id;
    return block;
}

std::filesystem::path findCounter(
    const std::filesystem::path& directory) {
    for (const auto& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".counter") {
            return entry.path();
        }
    }
    throw std::runtime_error("turn counter was not created");
}

/// Verifies missing-counter migration, sealed/legacy authority, strict legacy
/// parsing, delimiter-safe identity keys and the SDK identity limit.
void testDurableTurnMigrationAndIngressIdentityBounds() {
    ScopedTempDirectory temp("master-agent-turn-migration");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<master_agent::IdGenerator>(
        "turn-migration");
    const auto state = temp.path() / "turns";
    std::size_t floor_calls = 0;
    master_agent::interaction::TurnFloorLookup floor =
        [&floor_calls](const std::string&, const std::string&) {
            ++floor_calls;
            return master_agent::Result<std::uint64_t>::Success(7);
        };
    master_agent::interaction::InteractionLayer first_layer(
        clock, ids, state, floor);
    TextInput input;
    input.text = "hello";
    input.user_id = "driver-turn";
    input.session_id = "session-turn";
    input.params["locale"] = "zh-CN";
    const auto first = first_layer.submitText(input);
    const auto second = first_layer.submitText(input);
    expect(first.value && first.value->turn_id == 8 &&
               first.value->params.at("locale") == "zh-CN" &&
               second.value && second.value->turn_id == 9 &&
               floor_calls == 1,
           "memory floor must run once only while the counter is missing");

    master_agent::interaction::TurnFloorLookup forbidden_floor =
        [&floor_calls](const std::string&,
                       const std::string&)
            -> master_agent::Result<std::uint64_t> {
            ++floor_calls;
            throw std::runtime_error("floor must not run");
        };
    master_agent::interaction::InteractionLayer sealed_restart(
        clock, ids, state, forbidden_floor);
    const auto sealed_next = sealed_restart.submitText(input);
    expect(sealed_next.value && sealed_next.value->turn_id == 10 &&
               floor_calls == 1,
           "a sealed counter must remain authoritative after restart");

    const auto counter = findCounter(state);
    {
        std::ofstream legacy(counter, std::ios::binary | std::ios::trunc);
        legacy << "41 \r\n";
    }
    master_agent::interaction::InteractionLayer legacy_restart(
        clock, ids, state, forbidden_floor);
    const auto legacy_next = legacy_restart.submitText(input);
    expect(legacy_next.value && legacy_next.value->turn_id == 42 &&
               floor_calls == 1,
           "a valid legacy counter must migrate without consulting memory");
    {
        std::ofstream corrupt(counter, std::ios::binary | std::ios::trunc);
        corrupt << "42garbage";
    }
    const auto rejected_legacy = legacy_restart.submitText(input);
    expect(!rejected_legacy.status.ok &&
               rejected_legacy.status.error.code ==
                   "INTERACTION_TURN_STATE_CORRUPT",
           "legacy counter trailing non-whitespace must fail closed");

    ScopedTempDirectory invalid_floor_temp(
        "master-agent-invalid-turn-floor");
    master_agent::interaction::InteractionLayer invalid_floor(
        clock, ids, invalid_floor_temp.path(),
        [](const std::string&, const std::string&) {
            return master_agent::Result<std::uint64_t>::Success(
                std::numeric_limits<std::uint64_t>::max());
        });
    const auto invalid_floor_result = invalid_floor.submitText(input);
    expect(!invalid_floor_result.status.ok &&
               invalid_floor_result.status.error.code ==
                   "INTERACTION_TURN_MIGRATION_FAILED",
           "UINT64_MAX memory floor must be rejected as invalid migration");

    ScopedTempDirectory missing_floor_temp(
        "master-agent-missing-turn-floor-value");
    master_agent::interaction::InteractionLayer missing_floor(
        clock, ids, missing_floor_temp.path(),
        [](const std::string&,
           const std::string&)
            -> master_agent::Result<std::uint64_t> {
            return {master_agent::Status::Ok(), std::nullopt};
        });
    const auto missing_floor_result = missing_floor.submitText(input);
    expect(!missing_floor_result.status.ok &&
               missing_floor_result.status.error.code ==
                   "INTERACTION_TURN_MIGRATION_FAILED",
           "successful floor lookup without a value must fail migration");

    ScopedTempDirectory throwing_floor_temp(
        "master-agent-throwing-turn-floor");
    master_agent::interaction::InteractionLayer throwing_floor(
        clock, ids, throwing_floor_temp.path(),
        [](const std::string&,
           const std::string&)
            -> master_agent::Result<std::uint64_t> {
            throw std::runtime_error("raw-floor-secret");
        });
    const auto throwing_floor_result = throwing_floor.submitText(input);
    expect(!throwing_floor_result.status.ok &&
               throwing_floor_result.status.error.code ==
                   "INTERACTION_TURN_MIGRATION_FAILED" &&
               throwing_floor_result.status.error.message.find(
                   "raw-floor-secret") == std::string::npos,
           "turn-floor exceptions must become a safe migration failure");

    ScopedTempDirectory delimiter_temp(
        "master-agent-delimiter-safe-turns");
    master_agent::interaction::InteractionLayer delimiter_layer(
        clock, ids, delimiter_temp.path());
    TextInput left = input;
    left.user_id = "a";
    left.session_id = "bc";
    TextInput right = input;
    right.user_id = "ab";
    right.session_id = "c";
    const auto left_result = delimiter_layer.submitText(left);
    const auto right_result = delimiter_layer.submitText(right);
    expect(left_result.value && left_result.value->turn_id == 1 &&
               right_result.value && right_result.value->turn_id == 1,
           "length-prefixed turn keys must separate delimiter-like identities");

    TextInput oversized = input;
    oversized.user_id.assign(257, 'u');
    const auto oversized_user = delimiter_layer.submitText(oversized);
    oversized.user_id = "driver";
    oversized.session_id.assign(257, 's');
    const auto oversized_session = delimiter_layer.submitText(oversized);
    expect(!oversized_user.status.ok &&
               oversized_user.status.error.code ==
                   "INTERACTION_USER_ID_TOO_LARGE" &&
               !oversized_session.status.ok &&
               oversized_session.status.error.code ==
                   "INTERACTION_SESSION_ID_TOO_LARGE",
           "ingress must enforce the short-term SDK's 256-byte IDs");

    TextInput delimited = input;
    delimited.user_id = "driver:one";
    const auto delimited_user = delimiter_layer.submitText(delimited);
    delimited.user_id = "driver";
    delimited.session_id = "session:one";
    const auto delimited_session =
        delimiter_layer.submitText(delimited);
    expect(!delimited_user.status.ok &&
               delimited_user.status.error.code ==
                   "INTERACTION_IDENTITY_DELIMITER_INVALID" &&
               !delimited_session.status.ok &&
               delimited_session.status.error.code ==
                   "INTERACTION_IDENTITY_DELIMITER_INVALID",
           "ingress must reject identities ambiguous in SDK memory_id");
}

/// Verifies context response sealing, safe exception summaries, write ACK
/// binding, side-effect classification and post-I/O deadline fencing.
void testMemorySdkBoundarySealsAndFailureSemantics() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto client = std::make_shared<ScriptedMemoryClient>();
    client->clock = clock;
    master_agent::memory::MemoryService service(client, clock);
    auto request = memoryRequest(clock);
    auto call = memoryCall(request);

    client->get_result.success = true;
    client->get_result.context_blocks = {
        validMemoryBlock(1), validMemoryBlock(2)};
    const auto valid = service.getContext(request, "query", call);
    expect(valid.status.ok && valid.value &&
               valid.value->blocks.size() == 2 &&
               valid.value->flattened_context.find("\n---\n") !=
                   std::string::npos,
           "validated SDK context must return a populated Result value");

    const auto expect_invalid_context =
        [&](std::vector<master_agent::memory::ContextBlock> blocks,
            const std::string& message) {
            client->get_result.success = true;
            client->get_result.context_blocks = std::move(blocks);
            const auto result =
                service.getContext(request, "query", call);
            expect(!result.status.ok &&
                       result.status.error.code ==
                           "MEMORY_CONTEXT_RESULT_INVALID",
                   message);
        };
    expect_invalid_context(
        {validMemoryBlock(1), validMemoryBlock(2),
         validMemoryBlock(3), validMemoryBlock(4)},
        "memory response must obey max_turns");
    auto malformed = validMemoryBlock(1);
    malformed.session_id = "other-session";
    expect_invalid_context({malformed},
                           "context session must bind the request");
    malformed = validMemoryBlock(1);
    malformed.turn_id = 0;
    expect_invalid_context({malformed},
                           "context turn must be nonzero");
    malformed = validMemoryBlock(1);
    malformed.timestamp_ms = 0;
    expect_invalid_context({malformed},
                           "context timestamp must be nonzero");
    malformed = validMemoryBlock(1);
    malformed.memory_type = "long_term";
    expect_invalid_context({malformed},
                           "context type must be short_term");
    malformed = validMemoryBlock(1);
    malformed.source_memory_id = "unbound-source";
    expect_invalid_context({malformed},
                           "context source must bind identity");
    malformed = validMemoryBlock(1);
    malformed.content.clear();
    expect_invalid_context({malformed},
                           "context content must not be empty");
    malformed = validMemoryBlock(1);
    malformed.relevance_score =
        std::numeric_limits<double>::quiet_NaN();
    expect_invalid_context({malformed},
                           "context relevance must be finite");
    malformed = validMemoryBlock(1);
    malformed.relevance_score = 1.1;
    expect_invalid_context({malformed},
                           "context relevance must stay within [0,1]");
    expect_invalid_context(
        {validMemoryBlock(1), validMemoryBlock(1)},
        "duplicate context blocks must fail closed");
    malformed = validMemoryBlock(1);
    malformed.content.assign(1025, 'x');
    expect_invalid_context({malformed},
                           "context bytes must obey token budget");
    client->get_result.success = true;
    client->get_result.context_blocks = {validMemoryBlock(1)};
    client->get_result.error_code = "invalid_request";
    const auto contradictory_get =
        service.getContext(request, "query", call);
    client->get_result.error_code.clear();
    expect(!contradictory_get.status.ok &&
               contradictory_get.status.error.code ==
                   "MEMORY_CONTEXT_RESULT_INVALID",
           "success and error fields must not coexist in a sealed response");

    client->throw_get = true;
    const auto thrown_get =
        service.getContext(request, "query", call);
    client->throw_get = false;
    expect(!thrown_get.status.ok &&
               thrown_get.status.error.code ==
                   "MEMORY_CONTEXT_CLIENT_EXCEPTION" &&
               thrown_get.status.error.message.find(
                   "raw-read-secret") == std::string::npos,
           "memory read exception details must not cross the boundary");

    auto oversized_request = request;
    oversized_request.user_id.assign(257, 'u');
    const auto reads_before = client->read_calls;
    const auto oversized_context = service.getContext(
        oversized_request, "query", memoryCall(oversized_request));
    expect(!oversized_context.status.ok &&
               oversized_context.status.error.code ==
                   "MEMORY_IDENTITY_TOO_LARGE" &&
               client->read_calls == reads_before,
           "oversized memory identity must be rejected before SDK I/O");

    auto delimited_request = request;
    delimited_request.session_id = "session:memory";
    const auto reads_before_delimited = client->read_calls;
    const auto delimited_context = service.getContext(
        delimited_request, "query", memoryCall(delimited_request));
    expect(!delimited_context.status.ok &&
               delimited_context.status.error.code ==
                   "MEMORY_IDENTITY_INVALID" &&
               client->read_calls == reads_before_delimited,
           "Memory recall must reject colon-delimited identity before I/O");

    master_agent::memory::CompletedTurn turn;
    turn.request = request;
    turn.normalized_user_input = "hello";
    turn.assistant_output = "world";
    const auto expected_id =
        request.user_id + ":" + request.session_id + ":1:1";
    client->write_result = {};
    client->write_result.success = true;
    client->write_result.memory_id = expected_id;
    client->write_result.revision = 1;
    expect(service.writeTurn(turn, call).ok,
           "sealed memory write acknowledgement must succeed");

    auto delimited_turn = turn;
    delimited_turn.request.user_id = "driver:memory";
    const auto writes_before_delimited = client->write_calls;
    const auto rejected_delimited_write = service.writeTurn(
        delimited_turn, memoryCall(delimited_turn.request));
    expect(!rejected_delimited_write.ok &&
               rejected_delimited_write.error.code ==
                   "MEMORY_IDENTITY_INVALID" &&
               client->write_calls == writes_before_delimited,
           "Memory write must reject ambiguous identity before I/O");

    client->write_result.memory_id = "other-memory";
    const auto wrong_id = service.writeTurn(turn, call);
    expect(!wrong_id.ok &&
               wrong_id.error.code ==
                   "MEMORY_WRITE_RESULT_INVALID" &&
               wrong_id.error.side_effect_state ==
                   master_agent::SideEffectState::Unknown,
           "memory write ACK must bind the expected identity");
    client->write_result.memory_id = expected_id;
    client->write_result.revision = 0;
    const auto zero_revision = service.writeTurn(turn, call);
    expect(!zero_revision.ok &&
               zero_revision.error.code ==
                   "MEMORY_WRITE_RESULT_INVALID" &&
               zero_revision.error.side_effect_state ==
                   master_agent::SideEffectState::Unknown,
           "memory write ACK revision must be positive");

    client->write_result = {};
    client->write_result.error_code = "storage_commit_failed";
    const auto commit_failed = service.writeTurn(turn, call);
    expect(!commit_failed.ok &&
               commit_failed.error.side_effect_state ==
                   master_agent::SideEffectState::Unknown,
           "storage_commit_failed must retain UNKNOWN side effects");
    client->write_result.error_code = "invalid_request";
    const auto invalid_request = service.writeTurn(turn, call);
    expect(!invalid_request.ok &&
               invalid_request.error.side_effect_state ==
                   master_agent::SideEffectState::NotStarted,
           "definite request rejection must be NOT_STARTED");
    client->write_result.error_code = "provider-private-code";
    const auto unknown_failure = service.writeTurn(turn, call);
    expect(!unknown_failure.ok &&
               unknown_failure.error.code ==
                   "MEMORY_WRITE_FAILED" &&
               unknown_failure.error.side_effect_state ==
                   master_agent::SideEffectState::Unknown,
           "unknown provider failures must be sanitized and ambiguous");

    client->throw_write = true;
    const auto thrown_write = service.writeTurn(turn, call);
    client->throw_write = false;
    expect(!thrown_write.ok &&
               thrown_write.error.code ==
                   "MEMORY_WRITE_CLIENT_EXCEPTION" &&
               thrown_write.error.message.find(
                   "raw-write-secret") == std::string::npos &&
               thrown_write.error.side_effect_state ==
                   master_agent::SideEffectState::Unknown,
           "memory write exception details must stay private");

    client->write_result = {};
    client->write_result.success = true;
    client->write_result.memory_id = expected_id;
    client->write_result.revision = 2;
    client->advance_write_ms = 2;
    auto late_request = request;
    late_request.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000LL;
    turn.request = late_request;
    const auto late_write =
        service.writeTurn(turn, memoryCall(late_request));
    expect(!late_write.ok &&
               late_write.error.code ==
                   "MEMORY_WRITE_COMMITTED_AFTER_DEADLINE" &&
               late_write.error.side_effect_state ==
                   master_agent::SideEffectState::Committed,
           "late validated success must report COMMITTED, not UNKNOWN");
}

/// Scriptable state Provider for response-contract and exception containment
/// tests.
class ScriptedStateProvider final
    : public master_agent::preprocess::
          IRuntimeStateProvider {
public:
    explicit ScriptedStateProvider(
        std::shared_ptr<ManualRuntimeClock> runtime_clock)
        : clock(std::move(runtime_clock)) {
        master_agent::preprocess::StateCapability capability;
        capability.state_type =
            master_agent::preprocess::StateDomain::Vehicle;
        capability.fields = {"speed_kmh"};
        capability_result =
            master_agent::Result<
                master_agent::preprocess::StateCapability>::
                Success(std::move(capability));
        master_agent::preprocess::StateQueryResult query;
        query.success = true;
        query.values["speed_kmh"] = "12";
        query.timestamp_utc_ms = clock->utcNowMs();
        query_result =
            master_agent::Result<
                master_agent::preprocess::StateQueryResult>::
                Success(std::move(query));
    }

    master_agent::Result<
        master_agent::preprocess::StateCapability>
    getCapability() const override {
        ++capability_calls;
        if (advance_capability_ms != 0) {
            clock->advanceMs(advance_capability_ms);
        }
        if (throw_capability) {
            throw std::runtime_error("raw-capability-secret");
        }
        return capability_result;
    }

    master_agent::Result<
        master_agent::preprocess::StateQueryResult>
    query(
        const master_agent::preprocess::StateQuery&)
        const override {
        ++query_calls;
        if (advance_query_ms != 0) {
            clock->advanceMs(advance_query_ms);
        }
        if (throw_query) {
            throw std::runtime_error("raw-query-secret");
        }
        return query_result;
    }

    std::shared_ptr<ManualRuntimeClock> clock;
    master_agent::Result<
        master_agent::preprocess::StateCapability>
        capability_result;
    master_agent::Result<
        master_agent::preprocess::StateQueryResult>
        query_result;
    std::int64_t advance_capability_ms = 0;
    std::int64_t advance_query_ms = 0;
    bool throw_capability = false;
    bool throw_query = false;
    mutable std::size_t capability_calls = 0;
    mutable std::size_t query_calls = 0;
};

master_agent::interaction::StandardRequest
preprocessRequest(
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    master_agent::interaction::StandardRequest request;
    request.request_id = "preprocess-request";
    request.trace_id = "preprocess-trace";
    request.text = "hello";
    request.timestamp_utc_ms = clock->utcNowMs();
    request.deadline_mono_ns =
        clock->monotonicNowNs() + 10'000'000'000LL;
    request.user_id = "driver-preprocess";
    request.session_id = "session-preprocess";
    request.turn_id = 1;
    request.priority = TaskPriority::P1;
    request.trigger_type = "TEXT_INPUT";
    return request;
}

CallContext preprocessCall(
    const master_agent::interaction::StandardRequest&
        request) {
    return {CallerModuleId::AgentService, request.request_id,
            request.trace_id, "preprocess-principal",
            request.priority, request.deadline_mono_ns};
}

void testPreprocessUtf8MetadataAndTimeContracts() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    master_agent::preprocess::PreprocessEngine engine(
        clock);
    auto request = preprocessRequest(clock);
    request.text = std::string("  ") + u8"打开" + "\t\n" +
                   u8"空调" + std::string(1, '\x01') + "  ";
    request.params = {
        {"TargetDevice", " ac "},
        {" Source-Page ", " front\t seat "},
        {"request-id", "attacker"},
        {"", "ignored"},
        {"empty", ""}};
    const auto result =
        engine.process(request, preprocessCall(request));
    expect(result.status.ok && result.value &&
               result.value->valid &&
               result.value->normalized_request.text ==
                   u8"打开 空调" &&
               result.value->normalized_request.params.at(
                   "target_device") == "ac" &&
               result.value->normalized_request.params.at(
                   "source_page") == "front seat" &&
               result.value->normalized_request.params.count(
                   "request_id") == 0 &&
               result.value->event_schema.at("is_fresh") ==
                   "true" &&
               result.value->event_schema.at("trigger_type") ==
                   "text_input",
           "preprocess must clean controls, normalize params and protect "
           "reserved metadata");

    auto truncated_request = preprocessRequest(clock);
    truncated_request.text =
        std::string(2047, 'a') + u8"中";
    const auto truncated = engine.process(
        truncated_request, preprocessCall(truncated_request));
    expect(truncated.value && truncated.value->valid &&
               truncated.value->normalized_request.text.size() ==
                   2047 &&
               truncated.value->normalized_request.text ==
                   std::string(2047, 'a'),
           "text truncation must stop before a complete UTF-8 scalar");

    auto invalid_utf8 = preprocessRequest(clock);
    invalid_utf8.text =
        std::string("\xF0\x28\x8C\x28", 4);
    const auto invalid_text = engine.process(
        invalid_utf8, preprocessCall(invalid_utf8));
    expect(invalid_text.status.ok && invalid_text.value &&
               !invalid_text.value->valid &&
               invalid_text.value->error_message ==
                   "input text is not valid UTF-8",
           "invalid UTF-8 must return a bounded recognized failure");

    auto invalid_suffix = preprocessRequest(clock);
    invalid_suffix.text =
        std::string(3000, 'a') + std::string("\xC0\xAF", 2);
    const auto suffix_result = engine.process(
        invalid_suffix, preprocessCall(invalid_suffix));
    expect(suffix_result.value && !suffix_result.value->valid &&
               suffix_result.value->error_message ==
                   "input text is not valid UTF-8",
           "invalid bytes after the 2048-byte prefix must not be hidden");

    auto too_large = preprocessRequest(clock);
    too_large.text.assign(16U * 1024U + 1U, 'a');
    const auto too_large_result = engine.process(
        too_large, preprocessCall(too_large));
    expect(too_large_result.value &&
               !too_large_result.value->valid,
           "direct callers must not bypass the ingress text ceiling");

    auto collision = preprocessRequest(clock);
    collision.params = {{"FooBar", "one"},
                        {"foo-bar", "two"}};
    const auto collision_result = engine.process(
        collision, preprocessCall(collision));
    expect(collision_result.value &&
               !collision_result.value->valid &&
               collision_result.value->error_message.find(
                   "collide") != std::string::npos,
           "normalized parameter key collisions must fail closed");

    auto invalid_param = preprocessRequest(clock);
    invalid_param.params["value"] =
        std::string("\xED\xA0\x80", 3);
    const auto invalid_param_result = engine.process(
        invalid_param, preprocessCall(invalid_param));
    expect(invalid_param_result.value &&
               !invalid_param_result.value->valid &&
               invalid_param_result.value->error_message.find(
                   "UTF-8") != std::string::npos,
           "parameter values must pass strict UTF-8 validation");

    auto too_many_params = preprocessRequest(clock);
    for (std::size_t index = 0; index < 65; ++index) {
        too_many_params.params.emplace(
            "p" + std::to_string(index), "v");
    }
    const auto too_many_result = engine.process(
        too_many_params, preprocessCall(too_many_params));
    expect(too_many_result.value &&
               !too_many_result.value->valid,
           "parameter count must have a hard boundary");

    auto long_param = preprocessRequest(clock);
    long_param.params["value"].assign(2049, 'v');
    const auto long_param_result = engine.process(
        long_param, preprocessCall(long_param));
    expect(long_param_result.value &&
               !long_param_result.value->valid,
           "parameter values must have a byte boundary");

    auto long_identity = preprocessRequest(clock);
    long_identity.session_id.assign(257, 's');
    const auto long_identity_result = engine.process(
        long_identity, preprocessCall(long_identity));
    expect(long_identity_result.status.ok &&
               long_identity_result.value &&
               !long_identity_result.value->valid,
           "preprocess must independently enforce metadata ID limits");

    auto event = preprocessRequest(clock);
    event.trigger_type = "PERCEPTION_EVENT";
    event.text.clear();
    event.params = {{"EventName", "driver_alert"}};
    const auto valid_event =
        engine.process(event, preprocessCall(event));
    expect(valid_event.value && valid_event.value->valid &&
               valid_event.value->event_schema.at(
                   "event_name") == "driver_alert",
           "event input may omit text when a valid parameter remains");
    event.params.clear();
    const auto empty_event =
        engine.process(event, preprocessCall(event));
    expect(empty_event.value && !empty_event.value->valid,
           "event input without text or params must be rejected");

    auto stale = preprocessRequest(clock);
    stale.timestamp_utc_ms = clock->utcNowMs() - 6000;
    const auto stale_result =
        engine.process(stale, preprocessCall(stale));
    expect(stale_result.value && stale_result.value->valid &&
               stale_result.value->event_schema.at(
                   "is_fresh") == "false" &&
               stale_result.value->normalized_request
                       .timestamp_utc_ms ==
                   clock->utcNowMs() - 1000,
           "stale timestamps must be marked and safely aligned");
    auto future = preprocessRequest(clock);
    future.timestamp_utc_ms = clock->utcNowMs() + 2000;
    const auto future_result =
        engine.process(future, preprocessCall(future));
    expect(future_result.value && future_result.value->valid &&
               future_result.value->event_schema.at(
                   "is_fresh") == "false" &&
               future_result.value->normalized_request
                       .timestamp_utc_ms == clock->utcNowMs(),
           "far-future timestamps must be clamped to processing time");

    auto bad_metadata = preprocessRequest(clock);
    bad_metadata.turn_id = 0;
    const auto metadata_result = engine.process(
        bad_metadata, preprocessCall(bad_metadata));
    expect(metadata_result.status.ok && metadata_result.value &&
               !metadata_result.value->valid,
           "recognized request metadata failures must still carry a value");

    auto mismatch_call = preprocessCall(request);
    mismatch_call.request_id = "different-request";
    const auto mismatch = engine.process(request, mismatch_call);
    expect(!mismatch.status.ok &&
               mismatch.status.error.code ==
                   "PREPROCESS_CALL_IDENTITY_INVALID",
           "process must bind the complete active request identity");

    auto missing_principal = preprocessCall(request);
    missing_principal.principal_id_hash.clear();
    const auto missing_principal_result =
        engine.process(request, missing_principal);
    expect(!missing_principal_result.status.ok &&
               missing_principal_result.status.error.code ==
                   "PREPROCESS_CALL_CONTEXT_INVALID",
           "preprocess call context requires a principal");

    auto p0_request = preprocessRequest(clock);
    p0_request.priority = TaskPriority::P0;
    auto p0_call = preprocessCall(p0_request);
    const auto unauthorized_p0 =
        engine.process(p0_request, p0_call);
    expect(!unauthorized_p0.status.ok &&
               unauthorized_p0.status.error.code ==
                   "PREPROCESS_CALL_CONTEXT_INVALID",
           "P0 preprocessing requires an authorization reference");

    CallContext forged{
        CallerModuleId::AgentService, request.request_id,
        request.trace_id, "preprocess-principal",
        request.priority, request.deadline_mono_ns,
        "forged:endpoint",
        master_agent::hostModuleProcessEpoch(
            CallerModuleId::AgentService)};
    const auto forged_result = engine.process(request, forged);
    expect(!forged_result.status.ok &&
               forged_result.status.error.code ==
                   "PREPROCESS_CALLER_NOT_ALLOWED",
           "caller enum without the host endpoint identity must fail");

    auto expired_request = preprocessRequest(clock);
    expired_request.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000LL;
    const auto expired_call = preprocessCall(expired_request);
    clock->advanceMs(2);
    const auto expired =
        engine.process(expired_request, expired_call);
    expect(!expired.status.ok &&
               expired.status.error.code ==
                   "PREPROCESS_CALL_EXPIRED",
           "expired preprocessing work must be fenced before cleaning");
}

void testPreprocessStateProviderBoundaryAndSeals() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    master_agent::preprocess::PreprocessEngine host_engine(
        clock);
    auto request = preprocessRequest(clock);
    auto call = preprocessCall(request);
    const auto capabilities = host_engine.getCapabilities(call);
    expect(capabilities.status.ok && capabilities.value &&
               capabilities.value->size() == 2 &&
               capabilities.value->at(0).state_type ==
                   master_agent::preprocess::StateDomain::Vehicle &&
               std::find(
                   capabilities.value->at(0).fields.begin(),
                   capabilities.value->at(0).fields.end(),
                   "battery_soc") !=
                   capabilities.value->at(0).fields.end(),
           "reference runtime must expose bounded capability metadata only");

    master_agent::preprocess::StateQuery query;
    query.request_id = request.request_id;
    query.session_id = request.session_id;
    query.turn_id = request.turn_id;
    query.state_type =
        master_agent::preprocess::StateDomain::Vehicle;
    query.fields = {"speed_kmh", "is_parked"};
    const auto state =
        host_engine.queryRuntimeState(query, call);
    expect(state.status.ok && state.value &&
               state.value->success &&
               state.value->values.size() == 2,
           "reference runtime Provider must execute a validated field query");

    query.fields = {"missing_field"};
    const auto missing =
        host_engine.queryRuntimeState(query, call);
    expect(missing.status.ok && missing.value &&
               !missing.value->success &&
               missing.value->missing_fields ==
                   std::vector<std::string>{"missing_field"},
           "known missing fields must return a value-level query failure");
    query.fields = {"speed_kmh", "speed_kmh"};
    const auto duplicate =
        host_engine.queryRuntimeState(query, call);
    expect(!duplicate.status.ok &&
               duplicate.status.error.code ==
                   "PREPROCESS_STATE_QUERY_INVALID",
           "duplicate state fields must be rejected before Provider I/O");
    query.fields = {"speed_kmh"};
    query.state_type =
        static_cast<
            master_agent::preprocess::StateDomain>(255);
    const auto unsupported =
        host_engine.queryRuntimeState(query, call);
    expect(!unsupported.status.ok &&
               unsupported.status.error.code ==
                   "PREPROCESS_STATE_QUERY_INVALID",
           "unsupported state domain must fail the closed-enum boundary");

    auto denied = call;
    denied.caller = CallerModuleId::IntentRecognitionEngine;
    denied.caller_endpoint_id =
        master_agent::hostModuleEndpoint(
            CallerModuleId::IntentRecognitionEngine);
    denied.caller_process_epoch =
        master_agent::hostModuleProcessEpoch(
            CallerModuleId::IntentRecognitionEngine);
    const auto denied_capabilities =
        host_engine.getCapabilities(denied);
    expect(!denied_capabilities.status.ok &&
               denied_capabilities.status.error.code ==
                   "PREPROCESS_CALLER_NOT_ALLOWED",
           "getCapabilities must enforce the AgentService boundary");

    auto null_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    null_provider->capability_result = {
        master_agent::Status::Ok(), std::nullopt};
    master_agent::preprocess::PreprocessEngine
        null_capability_engine(clock, {null_provider});
    const auto null_capability =
        null_capability_engine.getCapabilities(call);
    expect(!null_capability.status.ok &&
               null_capability.status.error.code ==
                   "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
           "Provider OK without capability value must fail closed");

    auto throwing_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    throwing_provider->throw_capability = true;
    master_agent::preprocess::PreprocessEngine
        throwing_capability_engine(clock, {throwing_provider});
    const auto thrown_capability =
        throwing_capability_engine.getCapabilities(call);
    expect(!thrown_capability.status.ok &&
               thrown_capability.status.error.code ==
                   "PREPROCESS_STATE_PROVIDER_EXCEPTION" &&
               thrown_capability.status.error.message.find(
                   "raw-capability-secret") == std::string::npos &&
               throwing_provider->query_calls == 0,
           "capability discovery must not query state or leak exceptions");

    auto failed_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    failed_provider->capability_result =
        master_agent::Result<
            master_agent::preprocess::StateCapability>::
            Failure(master_agent::Status::Error(
                "provider", "PRIVATE", "raw-provider-secret"));
    master_agent::preprocess::PreprocessEngine
        failed_capability_engine(clock, {failed_provider});
    const auto failed_capability =
        failed_capability_engine.getCapabilities(call);
    expect(!failed_capability.status.ok &&
               failed_capability.status.error.message.find(
                   "raw-provider-secret") == std::string::npos,
           "Provider status text must be replaced by a safe summary");

    auto null_query_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    null_query_provider->query_result = {
        master_agent::Status::Ok(), std::nullopt};
    master_agent::preprocess::PreprocessEngine
        null_query_engine(clock, {null_query_provider});
    query.state_type =
        master_agent::preprocess::StateDomain::Vehicle;
    query.fields = {"speed_kmh"};
    const auto null_query =
        null_query_engine.queryRuntimeState(query, call);
    expect(!null_query.status.ok &&
               null_query.status.error.code ==
                   "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
           "Provider query OK without value must fail closed");

    auto throwing_query_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    throwing_query_provider->throw_query = true;
    master_agent::preprocess::PreprocessEngine
        throwing_query_engine(clock, {throwing_query_provider});
    const auto thrown_query =
        throwing_query_engine.queryRuntimeState(query, call);
    expect(!thrown_query.status.ok &&
               thrown_query.status.error.code ==
                   "PREPROCESS_STATE_PROVIDER_EXCEPTION" &&
               thrown_query.status.error.message.find(
                   "raw-query-secret") == std::string::npos,
           "Provider query exception must be contained and sanitized");

    auto malformed_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    master_agent::preprocess::StateQueryResult malformed;
    malformed.success = true;
    malformed.timestamp_utc_ms = clock->utcNowMs();
    malformed_provider->query_result =
        master_agent::Result<
            master_agent::preprocess::StateQueryResult>::
            Success(std::move(malformed));
    master_agent::preprocess::PreprocessEngine
        malformed_engine(clock, {malformed_provider});
    const auto malformed_result =
        malformed_engine.queryRuntimeState(query, call);
    expect(!malformed_result.status.ok &&
               malformed_result.status.error.code ==
                   "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
           "incomplete successful Provider output must fail its seal");

    auto late_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    late_provider->advance_capability_ms = 2;
    master_agent::preprocess::PreprocessEngine
        late_engine(clock, {late_provider});
    auto late_request = preprocessRequest(clock);
    late_request.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000LL;
    const auto late_capability =
        late_engine.getCapabilities(preprocessCall(late_request));
    expect(!late_capability.status.ok &&
               late_capability.status.error.code ==
                   "PREPROCESS_CAPABILITY_RESULT_AFTER_DEADLINE",
           "capability result crossing deadline must not be consumed");

    auto untouched_provider =
        std::make_shared<ScriptedStateProvider>(clock);
    master_agent::preprocess::PreprocessEngine
        expired_engine(clock, {untouched_provider});
    auto expired_request = preprocessRequest(clock);
    expired_request.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000LL;
    const auto expired_call = preprocessCall(expired_request);
    clock->advanceMs(2);
    const auto expired_capability =
        expired_engine.getCapabilities(expired_call);
    expect(!expired_capability.status.ok &&
               expired_capability.status.error.code ==
                   "PREPROCESS_CALL_EXPIRED" &&
               untouched_provider->capability_calls == 1 &&
               untouched_provider->query_calls == 0,
           "expired getCapabilities must be fenced before additional "
           "Provider metadata I/O or any live-state query");
}

void testAtomicEndToEndAndMemory() {
    ScopedTempDirectory temp("master-agent-e2e");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 2);
    expect(created.status.ok && created.value,
           "runtime creation must succeed");
    auto runtime = *created.value;

    TextInput first;
    first.text = u8"请把空调切换到内循环";
    first.user_id = "driver-001";
    first.session_id = "session-e2e";
    const auto result = runtime->submitText(first);
    expect(result.success, "atomic climate turn must succeed");
    expect(!result.plan_id.empty(), "atomic turn must commit a plan");
    expect(result.reply.find(u8"成功") != std::string::npos,
           "atomic turn must return verified completion reply");
    expect(std::filesystem::exists(
               temp.path() / "memory" / "conversation_journal.json"),
           "completed turn must be persisted to short-term memory");

    clock->advanceMs(10);
    TextInput second;
    second.text = u8"你好";
    second.user_id = first.user_id;
    second.session_id = first.session_id;
    const auto direct = runtime->submitText(second);
    expect(direct.success && direct.plan_id.empty(),
           "direct reply must not create a plan");

    CallContext log_call{CallerModuleId::AgentService, result.request_id,
                         result.trace_id, "reviewer", TaskPriority::P1,
                         clock->monotonicNowNs() + 1000000000LL};
    master_agent::data_log::TraceQuery query;
    query.request_id = result.request_id;
    const auto trace = runtime->dataLog()->queryTrace(query, log_call);
    expect(trace.status.ok && trace.value &&
               trace.value->events.size() >= 4,
           "end-to-end turn must have a causal event trail");
}

void testMockModelAndSubAgentEndToEnd() {
    ScopedTempDirectory temp("master-agent-agent-e2e");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 2);
    expect(created.status.ok && created.value,
           "runtime creation must succeed");
    auto runtime = *created.value;

    TextInput input;
    input.text = u8"请帮我规划明天的行程";
    input.text =
        "plan tomorrow's trip using memory preferences";
    input.user_id = "driver-002";
    input.session_id = "session-trip";
    const auto result = runtime->submitText(input);
    expect(result.success,
           "mock inference/sub-agent turn must succeed; error=" +
               result.error_code + "; reply=" + result.reply);
    expect(result.reply.find(u8"行程规划") != std::string::npos,
           "sub-agent output must reach interaction response");
    const auto dispatch_events = (*created.value)->dispatch()->events();
    expect(!dispatch_events.empty(),
           "Agent Dispatch must emit lifecycle events");
    const auto invocations =
        runtime->modelRuntime()->invocations();
    expect(invocations.size() == 2,
           "QUERY_BATCH path must invoke the model exactly twice");
    expect(invocations.at(0).inference_phase ==
               "FIRST_INFERENCE" &&
               invocations.at(1).inference_phase ==
                   "SECOND_INFERENCE",
           "model phases must be ordered and explicit");
    const auto first_output = nlohmann::json::parse(
        invocations.at(0).raw_output);
    const auto second_output = nlohmann::json::parse(
        invocations.at(1).raw_output);
    expect(first_output.value("branch", std::string{}) ==
               "QUERY_BATCH" &&
               first_output.at("queries").size() == 1,
           "first inference must return one complete query batch");
    expect(invocations.at(1).prompt.find(
               "EVIDENCE_BUNDLE:") != std::string::npos &&
               second_output.value("outcome", std::string{}) ==
                   "PLAN" &&
               !second_output.contains("branch") &&
               !second_output.contains("queries"),
           "second inference must consume frozen evidence and return only "
           "a final plan");
    expect(invocations.at(0).prompt_digest !=
               invocations.at(1).prompt_digest &&
               invocations.at(0).output_digest !=
                   invocations.at(1).output_digest &&
               invocations.at(0).reality == "SIMULATED" &&
               invocations.at(1).reality == "SIMULATED",
           "two model boundaries must be separately sealed and simulated");
}

void testBgeAcceptedPathBypassesModel() {
    ScopedTempDirectory temp("master-agent-bge-e2e");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "BGE runtime creation must succeed");
    auto runtime = *created.value;

    TextInput input;
    input.text = u8"请帮我给车内换气";
    input.user_id = "driver-bge";
    input.session_id = "session-bge";
    const auto result = runtime->submitText(input);
    expect(result.success && !result.plan_id.empty(),
           "an accepted BGE class must create a deterministic plan");
    expect(runtime->modelRuntime()->invocations().empty(),
           "an accepted BGE class must not invoke the language model");
}

void testVersionedRuleArtifactFastPathAndSlotBinding() {
    ScopedTempDirectory temp("master-agent-rule-artifact");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "rule-artifact runtime creation must succeed");
    auto runtime = *created.value;

    const nlohmann::json artifact_json = {
        {"rules",
         {{{"rule_id", "WL001"},
           {"level1_category", "vehicle_control"},
           {"level2_category", "air_circulation"},
           {"example_utterances", {"rule controlled ventilation"}},
           {"patterns", {"rule controlled ventilation"}},
           {"match_mode", "EXACT"},
           {"effective_stage", "TOOL"},
           {"action",
            "com_sgm_service_climate_setAirCirculationMode"},
           {"call_llm", false},
           {"tool_template_id",
            "com_sgm_service_climate_setAirCirculationMode"},
           {"required_slots", {"mode"}},
           {"rule_priority", 100},
           {"enabled", true},
           {"version", 1}}}}};
    const auto bytes = artifact_json.dump();
    const auto artifact_path = temp.path() / "intent-rules.json";
    {
        std::ofstream output(artifact_path, std::ios::binary);
        output << bytes;
    }
    master_agent::intent::RuleSetArtifactRef artifact;
    artifact.artifact_id = "customer-intent-rules";
    artifact.version = 1;
    artifact.content_digest = master_agent::secureDigest(bytes);
    artifact.schema_version = 1;
    artifact.read_only_uri = artifact_path;
    const CallContext management_call{
        CallerModuleId::AgentService, "rule-management",
        "rule-management-trace", "system", TaskPriority::P1,
        clock->monotonicNowNs() + 30'000'000'000LL};
    expect(runtime->intent()
               ->reloadRules(artifact, 0, management_call)
               .ok,
           "digest-sealed rule artifact must switch atomically");

    TextInput complete;
    complete.text = "rule controlled ventilation";
    complete.user_id = "driver-rule";
    complete.session_id = "session-rule";
    complete.params["mode"] = "AUTO";
    const auto planned = runtime->submitText(complete);
    expect(planned.success && !planned.plan_id.empty() &&
               runtime->modelRuntime()->invocations().empty(),
           "complete rule slots must produce a deterministic plan without BGE or LLM");

    TextInput incomplete = complete;
    incomplete.session_id = "session-rule-missing";
    incomplete.params.clear();
    const auto clarified = runtime->submitText(incomplete);
    expect(clarified.success && clarified.plan_id.empty() &&
               clarified.turn_summary == "clarification" &&
               runtime->modelRuntime()->invocations().empty(),
           "a rule with a missing required slot must clarify instead of executing");
}

void testFullReadOnlyQueryMatrixIsJoinedBeforeSecondInference() {
    ScopedTempDirectory temp("master-agent-query-matrix");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "query-matrix runtime creation must succeed");
    auto runtime = *created.value;

    TextInput input;
    input.text = "evaluate the full query matrix";
    input.user_id = "driver-query-matrix";
    input.session_id = "session-query-matrix";
    const auto result = runtime->submitText(input);
    expect(result.success && result.plan_id.empty(),
           "the query-matrix mock must finish with a direct reply");

    const auto invocations = runtime->modelRuntime()->invocations();
    expect(invocations.size() == 2,
           "QUERY_BATCH must use exactly two model phases");
    const auto first = nlohmann::json::parse(invocations[0].raw_output);
    expect(first.value("branch", std::string{}) == "QUERY_BATCH" &&
               first.at("queries").size() == 4,
           "the first phase must submit the complete four-module batch");
    const auto& evidence_prompt = invocations[1].prompt;
    expect(evidence_prompt.find("PreprocessingEngine") !=
                   std::string::npos &&
               evidence_prompt.find("MemoryService") !=
                   std::string::npos &&
               evidence_prompt.find("SkillEngine") !=
                   std::string::npos &&
               evidence_prompt.find("AtomicServiceManager") !=
                   std::string::npos,
           "the second phase must receive terminal evidence from every "
           "documented query owner");
}

void testAsynchronousIntentAdmissionAndIdempotentResultQuery() {
    ScopedTempDirectory temp("master-agent-intent-async");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created = MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "asynchronous intent runtime creation must succeed");
    auto runtime = *created.value;

    TextInput input;
    input.text = u8"你好";
    input.user_id = "driver-intent-async";
    input.session_id = "session-intent-async";
    const auto ingress = runtime->interaction()->submitText(input);
    expect(ingress.status.ok && ingress.value,
           "interaction must create the asynchronous intent request");
    const auto request = *ingress.value;
    CallContext call{
        CallerModuleId::AgentService, request.request_id,
        request.trace_id, "principal-intent-async",
        request.priority, request.deadline_mono_ns};
    const auto preprocessed = runtime->preprocess()->process(request, call);
    expect(preprocessed.status.ok && preprocessed.value,
           "preprocessing must produce the frozen intent input");
    const auto recalled = runtime->memory()->getContext(
        preprocessed.value->normalized_request,
        preprocessed.value->normalized_request.text, call);
    const auto catalog = runtime->atomic()->getToolCatalogSnapshot(call);
    expect(catalog.status.ok && catalog.value,
           "intent admission requires a frozen capability catalog");

    master_agent::intent::IntentContext context;
    context.preprocess_result = *preprocessed.value;
    if (recalled.status.ok && recalled.value) {
        context.memory_context = *recalled.value;
    }
    context.session_id = request.session_id;
    context.turn_id = request.turn_id;
    context.context_version = request.turn_id;
    context.priority = request.priority;
    context.deadline_mono_ns = request.deadline_mono_ns;
    context.expected_capability_digest = catalog.value->catalog_digest;

    const auto admitted = runtime->intent()->submit(
        preprocessed.value->normalized_request, context,
        "intent-async-idempotency", call);
    const auto replay = runtime->intent()->submit(
        preprocessed.value->normalized_request, context,
        "intent-async-idempotency", call);
    expect(admitted.accepted && !admitted.existing &&
               replay.accepted && replay.existing &&
               replay.job_id == admitted.job_id,
           "intent submit must admit once and replay the same job");

    std::optional<master_agent::intent::IntentJob> terminal;
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        const auto snapshot = runtime->intent()->getResult(
            admitted.job_id, call);
        expect(snapshot.status.ok && snapshot.value,
               "getResult must return the authoritative job snapshot");
        if (snapshot.value->state ==
                master_agent::intent::IntentJobState::Completed ||
            snapshot.value->state ==
                master_agent::intent::IntentJobState::Failed ||
            snapshot.value->state ==
                master_agent::intent::IntentJobState::Cancelled) {
            terminal = *snapshot.value;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(terminal && terminal->result &&
               terminal->result->outcome_type ==
                   master_agent::intent::IntentOutcomeType::DirectReply,
           "the asynchronous intent job must publish one terminal result");
}

/// Final REPLY/ASK/FAIL decisions are deterministic Intent terminals. They
/// must reach Interaction through Agent Service without manufacturing an
/// Orchestrator or Agent Dispatch job. ASK/FAIL are covered on both the
/// first-inference NO_QUERY path and the second-inference evidence path.
void testModelReplyAskFailBypassOrchestrator() {
    ScopedTempDirectory temp(
        "master-agent-model-terminal-routing");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created =
        MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "terminal-routing runtime must initialize");
    auto runtime = *created.value;

    const auto submit =
        [&runtime](const std::string& text,
                   const std::string& session) {
            TextInput input;
            input.text = text;
            input.user_id = "driver-model-terminals";
            input.session_id = session;
            return runtime->submitText(input);
        };

    const auto reply = submit(
        "explain the current request", "session-model-reply");
    expect(reply.success && reply.plan_id.empty() &&
               reply.turn_summary == "direct_reply" &&
               !reply.reply.empty(),
           "model REPLY must return directly without a plan");

    const auto first_ask = submit(
        "clarify destination", "session-model-first-ask");
    expect(first_ask.success && first_ask.plan_id.empty() &&
               first_ask.turn_summary == "clarification" &&
               first_ask.reply ==
                   u8"请确认你希望采用哪一种偏好。",
           "first-inference ASK must return a clarification directly");

    const auto second_ask = submit(
        "clarify destination using memory",
        "session-model-second-ask");
    expect(second_ask.success && second_ask.plan_id.empty() &&
               second_ask.turn_summary == "clarification",
           "second-inference ASK must return directly after evidence");

    const auto first_fail = submit(
        "unsupported request", "session-model-first-fail");
    expect(!first_fail.success && first_fail.plan_id.empty() &&
               first_fail.turn_summary == "intent_failed" &&
               first_fail.error_code ==
                   "INTENT_MODEL_UNSUPPORTED" &&
               first_fail.reply ==
                   u8"当前能力暂不支持该请求。",
           "first-inference FAIL must be a bounded business failure");

    const auto second_fail = submit(
        "unsupported request using memory",
        "session-model-second-fail");
    expect(!second_fail.success && second_fail.plan_id.empty() &&
               second_fail.turn_summary == "intent_failed" &&
               second_fail.error_code ==
                   "INTENT_MODEL_UNSUPPORTED",
           "second-inference FAIL must terminate without a third model call");

    const auto invocations =
        runtime->modelRuntime()->invocations();
    expect(invocations.size() == 7,
           "REPLY/ASK/FAIL matrix must use 1+1+2+1+2 model calls");
    const auto first_ask_envelope = nlohmann::json::parse(
        invocations.at(1).raw_output);
    const auto second_ask_output = nlohmann::json::parse(
        invocations.at(3).raw_output);
    const auto first_fail_envelope = nlohmann::json::parse(
        invocations.at(4).raw_output);
    const auto second_fail_output = nlohmann::json::parse(
        invocations.at(6).raw_output);
    expect(first_ask_envelope.at("final").value(
               "outcome", std::string{}) == "ASK" &&
               second_ask_output.value(
                   "outcome", std::string{}) == "ASK" &&
               first_fail_envelope.at("final").value(
                   "outcome", std::string{}) == "FAIL" &&
               second_fail_output.value(
                   "outcome", std::string{}) == "FAIL",
           "both inference paths must preserve ASK/FAIL final outcomes");
    expect(runtime->orchestrator()->events().empty() &&
               runtime->dispatch()->events().empty(),
           "REPLY/ASK/FAIL must not create plan or dispatch events");
}

void testRuntimeRestartRecoversTraceAndAdvancesProducerEpoch() {
    ScopedTempDirectory temp("master-agent-runtime-restart");
    auto clock = std::make_shared<ManualRuntimeClock>();
    std::string first_request_id;
    std::string first_trace_id;
    {
        const auto created =
            MasterAgentRuntime::create(temp.path(), clock, 1);
        expect(created.status.ok && created.value,
               "first runtime process must initialize");
        TextInput input;
        input.text = u8"你好";
        input.user_id = "driver-restart";
        input.session_id = "session-restart";
        const auto result = (*created.value)->submitText(input);
        expect(result.success,
               "first runtime process must complete a turn");
        first_request_id = result.request_id;
        first_trace_id = result.trace_id;
    }

    const auto restarted =
        MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(restarted.status.ok && restarted.value,
           "second runtime process must recover the same directory");
    CallContext query_call{
        CallerModuleId::AgentService, first_request_id,
        first_trace_id, "reviewer", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};
    master_agent::data_log::TraceQuery query;
    query.request_id = first_request_id;
    const auto recovered_trace =
        (*restarted.value)->dataLog()->queryTrace(query, query_call);
    expect(recovered_trace.value &&
               !recovered_trace.value->events.empty(),
           "restart must rebuild the prior request trace");

    TextInput next;
    next.text = u8"你好";
    next.user_id = "driver-restart";
    next.session_id = "session-restart";
    const auto next_result = (*restarted.value)->submitText(next);
    expect(next_result.success &&
               next_result.request_id != first_request_id &&
               next_result.turn_id == 2,
           "restart must retain the producer epoch separation and allocate "
           "the next durable turn");
}

void testStillUnknownReturnsPendingPlanReceipt() {
    ScopedTempDirectory temp(
        "master-agent-pending-reconciliation");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto created =
        MasterAgentRuntime::create(temp.path(), clock, 1);
    expect(created.status.ok && created.value,
           "pending-plan runtime must initialize");
    auto runtime = *created.value;
    runtime->climateProvider()->setNextInvocationState(
        master_agent::atomic_service::
            ProviderInvocationState::Unknown);
    runtime->climateProvider()->setUnknownReconcileStatus(
        master_agent::atomic_service::
            ReconcileStatus::StillUnknown);

    TextInput input;
    input.text = u8"请把空调切换到内循环";
    input.user_id = "driver-pending";
    input.session_id = "session-pending";
    const auto result = runtime->submitText(input);
    expect(result.success && result.pending &&
               !result.plan_id.empty() &&
               result.plan_state ==
                   master_agent::orchestrator::
                       PlanState::Running &&
               result.turn_summary == "plan_reconciling" &&
               result.error_code.empty(),
           "STILL_UNKNOWN must return the committed plan receipt as "
           "pending, not manufacture TURN_FAILED");

    CallContext query_call{
        CallerModuleId::AgentService, result.request_id,
        result.trace_id, "reviewer", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};
    master_agent::data_log::TraceQuery query;
    query.request_id = result.request_id;
    const auto trace =
        runtime->dataLog()->queryTrace(query, query_call);
    expect(trace.value &&
               std::any_of(
                   trace.value->events.begin(),
                   trace.value->events.end(),
                   [](const auto& event) {
                       return event.event_type ==
                              "TURN_PENDING";
                   }) &&
               std::none_of(
                   trace.value->events.begin(),
                   trace.value->events.end(),
                   [](const auto& event) {
                       return event.event_type ==
                              "TURN_FAILED";
                   }),
           "pending reconciliation must be observable without a false "
           "failure terminal");
}

void testInvalidIngress() {
    ScopedTempDirectory temp("master-agent-invalid");
    const auto created = MasterAgentRuntime::create(
        temp.path(), std::make_shared<ManualRuntimeClock>(), 1);
    expect(created.status.ok && created.value,
           "runtime creation must succeed");
    TextInput input;
    input.user_id = "driver-003";
    const auto result = (*created.value)->submitText(input);
    expect(!result.success && result.error_code == "INTERACTION_EMPTY_TEXT",
           "empty input must fail at ingress");
}

void testRuntimeShutdownIsIdempotentAndClosesIngress() {
    ScopedTempDirectory temp("master-agent-runtime-shutdown");
    const auto created = MasterAgentRuntime::create(
        temp.path(), std::make_shared<ManualRuntimeClock>(), 1);
    expect(created.status.ok && created.value,
           "runtime creation must succeed");
    const auto runtime = *created.value;
    expect(runtime->shutdown().ok && runtime->shutdown().ok,
           "runtime shutdown must be idempotent");

    TextInput input;
    input.text = u8"你好";
    input.user_id = "driver-shutdown";
    input.session_id = "session-shutdown";
    const auto rejected = runtime->submitText(input);
    expect(!rejected.success &&
               rejected.error_code == "RUNTIME_SHUTTING_DOWN",
           "shutdown must reject new ingress without entering modules");
}

}  // namespace

int main() {
    try {
        testDurableTurnMigrationAndIngressIdentityBounds();
        testMemorySdkBoundarySealsAndFailureSemantics();
        testPreprocessUtf8MetadataAndTimeContracts();
        testPreprocessStateProviderBoundaryAndSeals();
        testAtomicEndToEndAndMemory();
        testMockModelAndSubAgentEndToEnd();
        testBgeAcceptedPathBypassesModel();
        testVersionedRuleArtifactFastPathAndSlotBinding();
        testFullReadOnlyQueryMatrixIsJoinedBeforeSecondInference();
        testAsynchronousIntentAdmissionAndIdempotentResultQuery();
        testModelReplyAskFailBypassOrchestrator();
        testRuntimeRestartRecoversTraceAndAdvancesProducerEpoch();
        testStillUnknownReturnsPendingPlanReceipt();
        testInvalidIngress();
        testRuntimeShutdownIsIdempotentAndClosesIngress();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_e2e failure: " << error.what() << '\n';
        return 1;
    }
}
