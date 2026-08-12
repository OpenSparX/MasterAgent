/**
 * @file main.cpp
 * @brief Provides the default single-process Master Agent entry point.
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/runtime/master_agent_runtime.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

using master_agent::agent_service::TurnResult;
using master_agent::interaction::TextInput;
using master_agent::runtime::MasterAgentRuntime;

#ifdef _WIN32
std::string toUtf8(const wchar_t* value) {
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr,
        nullptr);
    if (required <= 1) return {};
    std::string converted(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                        converted.data(), required, nullptr, nullptr);
    converted.resize(static_cast<std::size_t>(required - 1));
    return converted;
}

std::vector<std::string> userArguments(int, char**) {
    int wide_count = 0;
    wchar_t** wide_arguments =
        CommandLineToArgvW(GetCommandLineW(), &wide_count);
    std::vector<std::string> result;
    if (!wide_arguments) return result;
    for (int index = 1; index < wide_count; ++index) {
        result.push_back(toUtf8(wide_arguments[index]));
    }
    LocalFree(wide_arguments);
    return result;
}
#else
std::vector<std::string> userArguments(int argc, char** argv) {
    std::vector<std::string> result;
    for (int index = 1; index < argc; ++index) {
        result.emplace_back(argv[index]);
    }
    return result;
}
#endif

std::string planState(const TurnResult& result) {

    if (!result.plan_state) return "NOT_APPLICABLE";
    using master_agent::orchestrator::PlanState;
    switch (*result.plan_state) {
        case PlanState::Committed:
            return "COMMITTED";
        case PlanState::Running:
            return "RUNNING";
        case PlanState::Succeeded:
            return "SUCCEEDED";
        case PlanState::Failed:
            return "FAILED";
        case PlanState::Cancelled:
            return "CANCELLED";
        case PlanState::Unknown:
            return "UNKNOWN";
        case PlanState::Suspended:
            return "SUSPENDED";
        case PlanState::Compensating:
            return "COMPENSATING";
    }
    return "UNKNOWN";
}

nlohmann::json encode(const TurnResult& result) {
    return {{"request_id", result.request_id},
            {"trace_id", result.trace_id},
            {"session_id", result.session_id},
            {"turn_id", result.turn_id},
            {"success", result.success},
            {"pending", result.pending},
            {"reply", result.reply},
            {"plan_id", result.plan_id},
            {"plan_state", planState(result)},
            {"error_code", result.error_code},
            {"error_message", result.error_message},
            {"turn_summary", result.turn_summary}};
}

nlohmann::json encodeModelInvocations(
    const std::vector<master_agent::inference::
                          MockModelInvocation>& invocations) {
    auto encoded = nlohmann::json::array();
    for (const auto& invocation : invocations) {
        encoded.push_back(
            {{"sequence", invocation.sequence},
             {"request_id", invocation.request_id},
             {"job_id", invocation.job_id},
             {"phase", invocation.inference_phase},
             {"prompt_protocol_version",
              invocation.prompt_protocol_version},
             {"input",
              {{"prompt", invocation.prompt},
               {"prompt_digest", invocation.prompt_digest}}},
             {"output",
              {{"raw_output", invocation.raw_output},
               {"output_digest", invocation.output_digest}}},
             {"reality", invocation.reality}});
    }
    return encoded;
}

}  // namespace

int main(int argc, char** argv) {
    std::string text = u8"请把前排自动风速设置为高";
    const auto arguments = userArguments(argc, argv);
    std::optional<std::filesystem::path> configured_runtime;
    std::vector<std::string> text_arguments;
    for (const auto& argument : arguments) {
        constexpr const char* kRuntimePrefix = "--runtime=";
        if (argument.rfind(kRuntimePrefix, 0) == 0) {
            configured_runtime =
                std::filesystem::path(argument.substr(
                    std::char_traits<char>::length(kRuntimePrefix)));
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: master_agent [--runtime=<directory>] "
                   "[request text]\n";
            return 0;
        } else {
            text_arguments.push_back(argument);
        }
    }
    if (!text_arguments.empty()) {
        text.clear();
        for (const auto& argument : text_arguments) {
            if (!text.empty()) text += " ";
            text += argument;
        }
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const auto runtime_directory = configured_runtime.value_or(
        std::filesystem::temp_directory_path() /
        ("master-agent-runtime-" +
         std::to_string(
             std::chrono::steady_clock::now()
                 .time_since_epoch()
                 .count())));
    const auto created =
        MasterAgentRuntime::create(runtime_directory, nullptr, 2);
    if (!created.status.ok || !created.value) {
        std::cerr
            << nlohmann::json{
                   {"success", false},
                   {"error_code", created.status.error.code},
                   {"error_message", created.status.error.message}}
                   .dump(2)
            << std::endl;
        return 1;
    }
    TextInput input;
    input.text = text;
    input.user_id = "cli-user";
    input.session_id = "cli-session";
    const auto result = (*created.value)->submitText(input);
    auto encoded = encode(result);
    encoded["runtime_directory"] =
        runtime_directory.string();
    encoded["mock_model_trace"] = encodeModelInvocations(
        (*created.value)->modelRuntime()->invocations());
    const auto shutdown_status = (*created.value)->shutdown();
    encoded["shutdown"] =
        shutdown_status.ok ? "FLUSHED" : "FLUSH_FAILED";
    std::cout << encoded.dump(2) << std::endl;
    return result.success && shutdown_status.ok ? 0 : 2;
}
