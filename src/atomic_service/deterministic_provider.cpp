/**
 * @file deterministic_provider.cpp
 * @brief Implements the deterministic climate provider and default MCP tools.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

ProviderInvocationResult DeterministicClimateProvider::call(
    const AtomicMcpCallEnvelope& envelope,
    const AtomicProviderInvocationSeal& invocation_seal) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sealed =
        [&invocation_seal](ProviderInvocationResult result) {
            result.invocation_seal = invocation_seal;
            return result;
        };
    ++invocation_count_;
    const auto selected = next_state_;
    next_state_ = ProviderInvocationState::Succeeded;
    if (selected == ProviderInvocationState::Unknown) {
        ProviderInvocationResult result;
        result.state = ProviderInvocationState::Unknown;
        result.side_effect_state = SideEffectState::Unknown;
        result.error_code = "PROVIDER_RESPONSE_LOST";
        return sealed(std::move(result));
    }
    if (selected == ProviderInvocationState::Failed) {
        ProviderInvocationResult result{
            ProviderInvocationState::Failed,
            errorResult("PROVIDER_EXECUTION_FAILED",
                        "deterministic provider injected failure"),
            SideEffectState::ConfirmedNotExecuted,
            "PROVIDER_EXECUTION_FAILED",
            CompletionEvidence::None, {}};
        result.retryable_hint = true;
        return sealed(std::move(result));
    }
    CallToolResult result;
    result.is_error = false;
    if (envelope.mcp_request.name ==
        "com_sgm_service_climate_setAirCirculationMode") {
        result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
    } else if (envelope.mcp_request.name ==
               "com_sgm_service_climate_setAutoFanSpeed") {
        result.structured_content =
            nlohmann::json{{"success", true},
                           {"appliedLocation",
                            envelope.mcp_request.arguments.at("location")},
                           {"appliedMode",
                            envelope.mcp_request.arguments.at("mode")},
                           {"errorCode", ""}};
    } else {
        return sealed(
            {ProviderInvocationState::Failed,
             errorResult("PROVIDER_TOOL_NOT_FOUND", "tool is unsupported"),
             SideEffectState::NotStarted, "PROVIDER_TOOL_NOT_FOUND",
             CompletionEvidence::None, {}});
    }
    result.text_content.push_back(result.structured_content.dump());
    operation_results_[envelope.runtime.operation_id] = result;
    return sealed(
         {ProviderInvocationState::Succeeded, result,
         SideEffectState::Committed, {},
         CompletionEvidence::StateVerified, {}});
}

AtomicReconcileResult DeterministicClimateProvider::reconcile(
    const AtomicMcpCallEnvelope& envelope,
    const AtomicProviderInvocationSeal& invocation_seal) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++reconciliation_count_;
    AtomicReconcileResult result;
    result.invocation_seal = invocation_seal;
    result.operation_id = envelope.runtime.operation_id;
    result.execution_id = envelope.runtime.execution_id;
    result.tool_name = envelope.mcp_request.name;
    result.fencing_token = envelope.runtime.fencing_token;
    const auto known = operation_results_.find(envelope.runtime.operation_id);
    if (known != operation_results_.end()) {
        result.status = ReconcileStatus::ConfirmedSuccess;
        result.side_effect_state = SideEffectState::Committed;
        result.call_tool_result = known->second;
        result.observed_state = known->second.structured_content;
        result.completion_evidence = CompletionEvidence::StateVerified;
        return result;
    }
    result.status = unknown_reconcile_status_;
    result.retryable_hint =
        result.status == ReconcileStatus::ConfirmedNotExecuted;
    if (result.status == ReconcileStatus::ConfirmedSuccess) {
        result.side_effect_state = SideEffectState::Committed;
        CallToolResult call_result;
        call_result.is_error = false;
        if (envelope.mcp_request.name ==
            "com_sgm_service_climate_setAirCirculationMode") {
            call_result.structured_content =
                nlohmann::json{{"success", true},
                               {"appliedMode",
                                envelope.mcp_request.arguments.at("mode")},
                               {"errorCode", ""}};
        } else {
            call_result.structured_content =
                nlohmann::json{
                    {"success", true},
                    {"appliedLocation",
                     envelope.mcp_request.arguments.at("location")},
                    {"appliedMode",
                     envelope.mcp_request.arguments.at("mode")},
                    {"errorCode", ""}};
        }
        call_result.text_content.push_back(
            call_result.structured_content.dump());
        result.call_tool_result = call_result;
        result.observed_state = call_result.structured_content;
        result.completion_evidence = CompletionEvidence::StateVerified;
        operation_results_[envelope.runtime.operation_id] = call_result;
    } else if (result.status ==
               ReconcileStatus::ConfirmedNotExecuted) {
        result.side_effect_state =
            SideEffectState::ConfirmedNotExecuted;
    } else if (result.status ==
               ReconcileStatus::ConfirmedFailure) {
        result.side_effect_state = SideEffectState::Compensated;
    } else {
        result.side_effect_state = SideEffectState::Unknown;
    }
    return result;
}

void DeterministicClimateProvider::setNextInvocationState(
    ProviderInvocationState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    next_state_ = state;
}

void DeterministicClimateProvider::setUnknownReconcileStatus(
    ReconcileStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    unknown_reconcile_status_ = status;
}

std::size_t DeterministicClimateProvider::invocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invocation_count_;
}

std::size_t DeterministicClimateProvider::reconciliationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reconciliation_count_;
}


std::vector<McpToolDefinition> defaultClimateMcpTools() {
    McpToolDefinition circulation;
    circulation.name =
        "com_sgm_service_climate_setAirCirculationMode";
    circulation.description =
        u8"设置空调空气循环模式，返回是否成功以及最终应用的模式。";
    circulation.input_schema =
        nlohmann::json{{"type", "object"},
                       {"properties",
                        {{"mode",
                          {{"type", "string"},
                           {"enum", {"INTERNAL", "EXTERNAL", "AUTO"}},
                           {"description", u8"空调的空气循环模式。"}}}}},
                       {"required", {"mode"}},
                       {"additionalProperties", false}};
    circulation.output_schema = nlohmann::json::object();
    circulation.output_schema["type"] = "object";
    circulation.output_schema["properties"] =
        nlohmann::json{{"success", {{"type", "boolean"}}},
                       {"appliedMode",
                        {{"type", "string"},
                         {"enum", {"INTERNAL", "EXTERNAL", "AUTO"}}}},
                       {"errorCode", {{"type", "string"}}}};
    circulation.output_schema["required"] =
        {"success", "appliedMode", "errorCode"};
    circulation.output_schema["additionalProperties"] = false;
    circulation.annotations = {"setAirCirculationMode", false, false, true,
                               false};

    McpToolDefinition fan;
    fan.name = "com_sgm_service_climate_setAutoFanSpeed";
    fan.description =
        u8"设置自动空调风速，返回是否成功以及最终应用的位置和模式。";
    fan.input_schema =
        nlohmann::json{{"type", "object"},
                       {"properties",
                        {{"location",
                          {{"type", "string"},
                           {"enum", {"FRONT", "REAR"}},
                           {"description", u8"调整自动风速的位置。"}}},
                         {"mode",
                          {{"type", "string"},
                           {"enum", {"LOW", "NORMAL", "HIGH"}},
                           {"description", u8"自动风速目标模式。"}}}}},
                       {"required", {"location", "mode"}},
                       {"additionalProperties", false}};
    fan.output_schema = nlohmann::json::object();
    fan.output_schema["type"] = "object";
    fan.output_schema["properties"] =
        nlohmann::json{
            {"success", {{"type", "boolean"}}},
            {"appliedLocation",
             {{"type", "string"}, {"enum", {"FRONT", "REAR"}}}},
            {"appliedMode",
             {{"type", "string"}, {"enum", {"LOW", "NORMAL", "HIGH"}}}},
            {"errorCode", {{"type", "string"}}}};
    fan.output_schema["required"] =
        {"success", "appliedLocation", "appliedMode", "errorCode"};
    fan.output_schema["additionalProperties"] = false;
    fan.annotations = {"setAutoFanSpeed", false, false, true, false};
    return {circulation, fan};
}

std::vector<AtomicToolRuntimePolicy> defaultClimateRuntimePolicies(
    std::uint32_t work_units) {
    AtomicToolRuntimePolicy circulation;
    circulation.tool_name =
        "com_sgm_service_climate_setAirCirculationMode";
    circulation.required_permissions = {"vehicle.climate.write"};
    circulation.retryable_errors = {
        "PROVIDER_EXECUTION_FAILED",
        "CONFIRMED_NOT_EXECUTED"};
    circulation.resource_argument_fields = {};
    circulation.completion_policy = CompletionPolicy::StateVerified;
    circulation.simulated_work_units =
        std::max<std::uint32_t>(1, work_units);

    AtomicToolRuntimePolicy fan;
    fan.tool_name = "com_sgm_service_climate_setAutoFanSpeed";
    fan.required_permissions = {"vehicle.climate.write"};
    fan.retryable_errors = {
        "PROVIDER_EXECUTION_FAILED",
        "CONFIRMED_NOT_EXECUTED"};
    fan.resource_argument_fields = {"location"};
    fan.completion_policy = CompletionPolicy::StateVerified;
    fan.simulated_work_units =
        std::max<std::uint32_t>(1, work_units);
    return {circulation, fan};
}

}  // namespace master_agent::atomic_service
