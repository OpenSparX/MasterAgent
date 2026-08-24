/**
 * @file main.cpp
 * @brief Provides the default single-process Master Agent entry point.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/config/model_profile_loader.h"
#include "master_agent/runtime/master_agent_runtime.h"

#ifdef MASTER_AGENT_HAS_LLAMA_CPP
#include "llama_cpp_model_runtime.h"
#endif

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
using master_agent::runtime::MasterAgentRuntimeOptions;

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

std::string inferenceState(
    master_agent::inference::InferenceJobState state) {
    using master_agent::inference::InferenceJobState;
    switch (state) {
        case InferenceJobState::Accepted: return "ACCEPTED";
        case InferenceJobState::Ready: return "READY";
        case InferenceJobState::Running: return "RUNNING";
        case InferenceJobState::Suspended: return "SUSPENDED";
        case InferenceJobState::Completed: return "COMPLETED";
        case InferenceJobState::Failed: return "FAILED";
        case InferenceJobState::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

nlohmann::json encodeInferenceEvents(
    const std::vector<master_agent::inference::InferenceEvent>& events,
    const std::string& trace_id) {
    auto encoded = nlohmann::json::array();
    for (const auto& event : events) {
        if (event.trace_id != trace_id) continue;
        nlohmann::json item{
            {"event_type", event.event_type},
            {"job_id", event.job_id},
            {"state", inferenceState(event.state)},
            {"stage", event.stage},
            {"trace_id", event.trace_id}};
        if (event.result) {
            item["result"] = {
                {"runtime", event.result->runtime_backend},
                {"model_id", event.result->model_id},
                {"reality", event.result->reality},
                {"prompt_digest", event.result->prompt_digest},
                {"output_digest", event.result->output_digest},
                {"prompt_tokens", event.result->prompt_token_count},
                {"generated_tokens", event.result->generated_token_count},
                {"total_latency_ms", event.result->total_latency_ms},
                {"streamed_chunks", event.result->streamed_chunk_count}};
        }
        if (event.last_error) {
            item["error"] = {
                {"code", event.last_error->code},
                {"module", event.last_error->source_module}};
        }
        encoded.push_back(std::move(item));
    }
    return encoded;
}

}  // namespace

int main(int argc, char** argv) {
    std::string text = u8"请把前排自动风速设置为高";
    const auto arguments = userArguments(argc, argv);
    std::optional<std::filesystem::path> configured_runtime;
    std::optional<std::filesystem::path> configured_model;
    std::optional<std::filesystem::path> configured_bundle;
    std::optional<std::string> configured_model_profile;
    std::optional<std::string> configured_cloud_profile;
    std::optional<std::string> configured_endpoint;
    std::uint64_t configured_deadline_ms = 30'000;
    bool enable_fake_cloud = false;
    bool enable_local_debug_trace = false;
    std::vector<std::string> text_arguments;
    for (const auto& argument : arguments) {
        constexpr const char* kRuntimePrefix = "--runtime=";
        constexpr const char* kModelPrefix = "--model=";
        constexpr const char* kConfigPrefix = "--config=";
        constexpr const char* kModelProfilePrefix = "--model-profile=";
        constexpr const char* kCloudProfilePrefix = "--cloud-profile=";
        constexpr const char* kEndpointPrefix = "--endpoint=";
        constexpr const char* kDeadlinePrefix = "--deadline-ms=";
        if (argument.rfind(kRuntimePrefix, 0) == 0) {
            configured_runtime =
                std::filesystem::path(argument.substr(
                    std::char_traits<char>::length(kRuntimePrefix)));
        } else if (argument.rfind(kModelPrefix, 0) == 0) {
            configured_model = std::filesystem::path(argument.substr(
                std::char_traits<char>::length(kModelPrefix)));
        } else if (argument.rfind(kConfigPrefix, 0) == 0) {
            configured_bundle = std::filesystem::path(argument.substr(
                std::char_traits<char>::length(kConfigPrefix)));
        } else if (argument.rfind(kModelProfilePrefix, 0) == 0) {
            configured_model_profile = argument.substr(
                std::char_traits<char>::length(kModelProfilePrefix));
        } else if (argument.rfind(kCloudProfilePrefix, 0) == 0) {
            configured_cloud_profile = argument.substr(
                std::char_traits<char>::length(kCloudProfilePrefix));
        } else if (argument.rfind(kEndpointPrefix, 0) == 0) {
            configured_endpoint = argument.substr(
                std::char_traits<char>::length(kEndpointPrefix));
        } else if (argument.rfind(kDeadlinePrefix, 0) == 0) {
            try {
                configured_deadline_ms = std::stoull(argument.substr(
                    std::char_traits<char>::length(kDeadlinePrefix)));
            } catch (...) {
                std::cerr << "invalid --deadline-ms value\n";
                return 1;
            }
        } else if (argument == "--fake-cloud") {
            enable_fake_cloud = true;
        } else if (argument == "--local-debug-trace") {
            enable_local_debug_trace = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: master_agent [--runtime=<directory>] "
                   "[--config=<bundle>] [--model-profile=<id>] "
                   "[--cloud-profile=<id>] "
                   "[--model=<gguf>] [--endpoint=<host:port>] "
                   "[--deadline-ms=<1..300000>] "
                   "[--fake-cloud] "
                   "[--local-debug-trace] "
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
    MasterAgentRuntimeOptions runtime_options;
    runtime_options.simulated_work_units = 2;
    runtime_options.enable_fake_cloud_fallback = enable_fake_cloud;
    if (enable_local_debug_trace) {
        runtime_options.local_debug_artifact_directory =
            runtime_directory / "local_debug";
    }
    std::optional<master_agent::config::ModelSelection> model_selection;
    if (configured_model_profile && !configured_bundle) {
        std::cerr << nlohmann::json{
            {"success", false},
            {"error_code", "CONFIG_BUNDLE_REQUIRED_FOR_MODEL_PROFILE"},
            {"error_message", "--model-profile requires --config"}}.dump(2)
                  << std::endl;
        return 1;
    }
    if (configured_cloud_profile && !configured_bundle) {
        std::cerr << nlohmann::json{
            {"success", false},
            {"error_code", "CONFIG_BUNDLE_REQUIRED_FOR_CLOUD_PROFILE"},
            {"error_message", "--cloud-profile requires --config"}}.dump(2)
                  << std::endl;
        return 1;
    }
    if (configured_bundle) {
        const auto loaded = master_agent::config::loadModelSelection(
            *configured_bundle, configured_model_profile,
            configured_cloud_profile);
        if (!loaded.status.ok || !loaded.value) {
            std::cerr << nlohmann::json{
                {"success", false},
                {"error_code", loaded.status.error.code},
                {"error_message", loaded.status.error.message}}.dump(2)
                      << std::endl;
            return 1;
        }
        model_selection = *loaded.value;
        runtime_options.config_snapshot_id = master_agent::secureDigest(
            model_selection->bundle_name + "|" +
            model_selection->bundle_version + "|" +
            model_selection->routing_policy_id);
        const auto& intent_profile = model_selection->intent;
        if (intent_profile.runtime_type == "deterministic_mock") {
            if (configured_model) {
                std::cerr << nlohmann::json{
                    {"success", false},
                    {"error_code", "MODEL_PATH_INCOMPATIBLE_WITH_PROFILE"},
                    {"error_message", intent_profile.id}}.dump(2)
                          << std::endl;
                return 1;
            }
        } else if (intent_profile.runtime_type == "llama_cpp") {
            if (!configured_model && !intent_profile.artifact_path_ref.empty()) {
                if (const char* resolved =
                        std::getenv(intent_profile.artifact_path_ref.c_str())) {
                    if (*resolved != '\0') configured_model = resolved;
                }
            }
            if (!configured_model) {
                std::cerr << nlohmann::json{
                    {"success", false},
                    {"error_code", "CONFIG_MODEL_ARTIFACT_UNRESOLVED"},
                    {"error_message", intent_profile.artifact_path_ref}}.dump(2)
                          << std::endl;
                return 1;
            }
            if (!configured_endpoint && !intent_profile.endpoint_ref.empty()) {
                if (const char* resolved =
                        std::getenv(intent_profile.endpoint_ref.c_str())) {
                    if (*resolved != '\0') configured_endpoint = resolved;
                }
            }
            if (!configured_endpoint && !intent_profile.default_endpoint.empty()) {
                configured_endpoint = intent_profile.default_endpoint;
            }
        } else {
            std::cerr << nlohmann::json{
                {"success", false},
                {"error_code", "CONFIG_MODEL_RUNTIME_UNSUPPORTED"},
                {"error_message", intent_profile.runtime_type}}.dump(2)
                      << std::endl;
            return 1;
        }

        if (model_selection->classifier.runtime_type !=
            "deterministic_mock_bge") {
            std::cerr << nlohmann::json{
                {"success", false},
                {"error_code", "CONFIG_CLASSIFIER_RUNTIME_UNSUPPORTED"},
                {"error_message", model_selection->classifier.runtime_type}}.dump(2)
                      << std::endl;
            return 1;
        }
        if (model_selection->cloud_fallback_enabled) {
            if (!model_selection->cloud_intent ||
                model_selection->cloud_intent->runtime_type != "fake_cloud") {
                std::cerr << nlohmann::json{
                    {"success", false},
                    {"error_code", "CONFIG_CLOUD_RUNTIME_UNSUPPORTED"},
                    {"error_message", model_selection->cloud_intent
                        ? model_selection->cloud_intent->runtime_type
                        : std::string("no enabled cloud model")}}.dump(2)
                          << std::endl;
                return 1;
            }
            runtime_options.enable_fake_cloud_fallback = true;
        }
        runtime_options.intent_model_profile_id =
            model_selection->intent.id;
    }
    if (configured_model) {
#ifdef MASTER_AGENT_HAS_LLAMA_CPP
        if (!std::filesystem::exists(*configured_model)) {
            std::cerr << nlohmann::json{
                {"success", false},
                {"error_code", "MODEL_FILE_NOT_FOUND"},
                {"error_message", configured_model->string()}}.dump(2)
                      << std::endl;
            return 1;
        }
        master_agent::inference::LlamaCppConfig model_config;
        model_config.model_path = configured_model->string();
        model_config.endpoint = configured_endpoint.value_or("127.0.0.1:8080");
        model_config.context_length = model_selection
            ? model_selection->intent.context_length : 4096;
        runtime_options.model_runtime =
            std::make_shared<master_agent::inference::LlamaCppModelRuntime>(
                std::move(model_config));
#else
        std::cerr << "this build has no llama.cpp runtime adapter\n";
        return 1;
#endif
    }
    const auto created = MasterAgentRuntime::create(
        runtime_directory, std::move(runtime_options));
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
    input.deadline_ms = configured_deadline_ms;
    const auto result = (*created.value)->submitText(input);
    auto encoded = encode(result);
    if (model_selection) {
        encoded["config_bundle"] = {
            {"path", model_selection->bundle_root.string()},
            {"name", model_selection->bundle_name},
            {"version", model_selection->bundle_version},
            {"model_routing", model_selection->routing_policy_id},
            {"intent_model", model_selection->intent.id},
            {"classifier_model", model_selection->classifier.id},
            {"cloud_model", model_selection->cloud_intent
                ? model_selection->cloud_intent->id : std::string{}},
            {"loading_scope", "MODEL_SELECTION_ONLY"}};
    }
    encoded["runtime_directory"] =
        runtime_directory.string();
    encoded["model_runtime"] =
        (*created.value)->modelRuntime()->runtimeTag();
    if (const auto mock = (*created.value)->mockModelRuntime()) {
        encoded["mock_model_trace"] =
            encodeModelInvocations(mock->invocations());
    }
    encoded["inference_trace"] = encodeInferenceEvents(
        (*created.value)->inference()->events(), result.trace_id);
    encoded["cloud_trace"] = nlohmann::json::array();
    for (const auto& event : (*created.value)->intent()->cloudEvents()) {
        encoded["cloud_trace"].push_back({
            {"event", event.event_type},
            {"request_id", event.request_id},
            {"trace_id", event.trace_id},
            {"reason_code", event.reason_code},
            {"outcome", event.outcome},
            {"payload_digest", event.payload_digest},
            {"output_digest", event.output_digest},
            {"runtime", event.runtime_tag}});
    }
    const auto shutdown_status = (*created.value)->shutdown();
    encoded["shutdown"] =
        shutdown_status.ok ? "FLUSHED" : "FLUSH_FAILED";
    std::cout << encoded.dump(2) << std::endl;
    return result.success && shutdown_status.ok ? 0 : 2;
}
