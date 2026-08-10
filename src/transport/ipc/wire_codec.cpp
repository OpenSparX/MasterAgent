/**
 * @file wire_codec.cpp
 * @brief Implements strict JSON codecs for cross-process domain contracts.
 */

#include "master_agent/transport/ipc/wire_codec.h"

#include <stdexcept>
#include <type_traits>

namespace master_agent::ipc::wire {
namespace {

template <typename Enum>
int enumValue(Enum value) {
    return static_cast<int>(value);
}

template <typename Enum>
Enum closedEnum(const nlohmann::json& value, int minimum,
                int maximum) {

    const auto encoded = value.get<int>();
    if (encoded < minimum || encoded > maximum) {
        throw std::runtime_error("closed enum is outside range");
    }
    return static_cast<Enum>(encoded);
}

template <typename T>
nlohmann::json optionalValue(const std::optional<T>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

template <typename T>
std::optional<T> decodeOptional(
    const nlohmann::json& value) {
    return value.is_null() ? std::optional<T>{}
                           : std::optional<T>{value.get<T>()};
}

nlohmann::json encodeObservation(
    const ObservationContext& value) {

    return {
        {"request_id", value.request_id},
        {"trace_id", value.trace_id},
        {"span_id", value.span_id},
        {"causal_parent_event_id",
         optionalValue(value.causal_parent_event_id)},
        {"session_id", optionalValue(value.session_id)},
        {"plan_id", optionalValue(value.plan_id)},
        {"pid", optionalValue(value.pid)},
        {"activation_id", optionalValue(value.activation_id)},
        {"execution_id", optionalValue(value.execution_id)},
        {"producer_endpoint_id", value.producer_endpoint_id},
        {"producer_epoch", value.producer_epoch},
        {"producer_sequence", value.producer_sequence},
        {"boot_id", value.boot_id},
        {"task_priority", enumValue(value.task_priority)},
        {"deadline_mono_ns", value.deadline_mono_ns}};
}

ObservationContext decodeObservation(
    const nlohmann::json& value) {
    ObservationContext result;
    result.request_id = value.at("request_id").get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    result.span_id = value.at("span_id").get<std::string>();
    result.causal_parent_event_id =
        decodeOptional<std::string>(
            value.at("causal_parent_event_id"));
    result.session_id =
        decodeOptional<std::string>(value.at("session_id"));
    result.plan_id =
        decodeOptional<std::string>(value.at("plan_id"));
    result.pid = decodeOptional<std::string>(value.at("pid"));
    result.activation_id =
        decodeOptional<std::string>(value.at("activation_id"));
    result.execution_id =
        decodeOptional<std::string>(value.at("execution_id"));
    result.producer_endpoint_id =
        value.at("producer_endpoint_id").get<std::string>();
    result.producer_epoch =
        value.at("producer_epoch").get<std::uint64_t>();
    result.producer_sequence =
        value.at("producer_sequence").get<std::uint64_t>();
    result.boot_id = value.at("boot_id").get<std::uint64_t>();
    result.task_priority = closedEnum<TaskPriority>(
        value.at("task_priority"), 0, 2);
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    return result;
}

nlohmann::json encodeError(const StructuredError& value) {
    return {
        {"domain", value.domain},
        {"code", value.code},
        {"message", value.message},
        {"retryable", value.retryable},
        {"side_effect_state", enumValue(value.side_effect_state)},
        {"severity_hint", enumValue(value.severity_hint)},
        {"source_module", value.source_module},
        {"source_interface", value.source_interface},
        {"operation", value.operation},
        {"context", encodeObservation(value.context)},
        {"recoverable_hint", value.recoverable_hint},
        {"retry_scope_hint", value.retry_scope_hint},
        {"safe_detail_code", value.safe_detail_code},
        {"safe_detail_summary", value.safe_detail_summary},
        {"evidence_event_ids", value.evidence_event_ids},
        {"evidence_object_refs", value.evidence_object_refs},
        {"privacy_labels", value.privacy_labels}};
}

StructuredError decodeError(const nlohmann::json& value) {
    StructuredError result;
    result.domain = value.at("domain").get<std::string>();
    result.code = value.at("code").get<std::string>();
    result.message = value.at("message").get<std::string>();
    result.retryable = value.at("retryable").get<bool>();
    result.side_effect_state = closedEnum<SideEffectState>(
        value.at("side_effect_state"), 0, 5);
    result.severity_hint = closedEnum<ErrorSeverityHint>(
        value.at("severity_hint"), 0, 3);
    result.source_module =
        value.at("source_module").get<std::string>();
    result.source_interface =
        value.at("source_interface").get<std::string>();
    result.operation = value.at("operation").get<std::string>();
    result.context = decodeObservation(value.at("context"));
    result.recoverable_hint =
        value.at("recoverable_hint").get<bool>();
    result.retry_scope_hint =
        value.at("retry_scope_hint").get<std::string>();
    result.safe_detail_code =
        value.at("safe_detail_code").get<std::string>();
    result.safe_detail_summary =
        value.at("safe_detail_summary").get<std::string>();
    result.evidence_event_ids =
        value.at("evidence_event_ids")
            .get<std::vector<std::string>>();
    result.evidence_object_refs =
        value.at("evidence_object_refs")
            .get<std::vector<std::string>>();
    result.privacy_labels =
        value.at("privacy_labels").get<std::vector<std::string>>();
    return result;
}

nlohmann::json encodeToolResult(
    const atomic_service::CallToolResult& value) {
    return {{"text_content", value.text_content},
            {"structured_content", value.structured_content},
            {"is_error", value.is_error}};
}

atomic_service::CallToolResult decodeToolResult(
    const nlohmann::json& value) {
    atomic_service::CallToolResult result;
    result.text_content =
        value.at("text_content").get<std::vector<std::string>>();
    result.structured_content = value.at("structured_content");
    result.is_error = value.at("is_error").get<bool>();
    return result;
}

nlohmann::json encodeProviderSeal(
    const atomic_service::AtomicProviderInvocationSeal& value) {
    return {
        {"invocation_id", value.invocation_id},
        {"provider_id", value.provider_id},
        {"provider_epoch", value.provider_epoch},
        {"operation_id", value.operation_id},
        {"execution_id", value.execution_id},
        {"attempt_no", value.attempt_no},
        {"tool_name", value.tool_name},
        {"tool_catalog_snapshot_id",
         value.tool_catalog_snapshot_id},
        {"tool_digest", value.tool_digest},
        {"policy_digest", value.policy_digest},
        {"fencing_token", value.fencing_token},
        {"request_digest", value.request_digest}};
}

atomic_service::AtomicProviderInvocationSeal decodeProviderSeal(
    const nlohmann::json& value) {
    atomic_service::AtomicProviderInvocationSeal result;
    result.invocation_id =
        value.at("invocation_id").get<std::string>();
    result.provider_id =
        value.at("provider_id").get<std::string>();
    result.provider_epoch =
        value.at("provider_epoch").get<std::uint64_t>();
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.execution_id =
        value.at("execution_id").get<std::string>();
    result.attempt_no =
        value.at("attempt_no").get<std::uint32_t>();
    result.tool_name = value.at("tool_name").get<std::string>();
    result.tool_catalog_snapshot_id =
        value.at("tool_catalog_snapshot_id").get<std::string>();
    result.tool_digest =
        value.at("tool_digest").get<std::string>();
    result.policy_digest =
        value.at("policy_digest").get<std::string>();
    result.fencing_token =
        value.at("fencing_token").get<std::uint64_t>();
    result.request_digest =
        value.at("request_digest").get<std::string>();
    return result;
}

}  // namespace

nlohmann::json encodeStatus(const Status& value) {
    return {{"ok", value.ok},
            {"error", encodeError(value.error)}};
}

Status decodeStatus(const nlohmann::json& value) {
    Status result;
    result.ok = value.at("ok").get<bool>();
    result.error = decodeError(value.at("error"));
    return result;
}

nlohmann::json encodeCallContext(const CallContext& value) {
    return {
        {"caller", enumValue(value.caller)},
        {"request_id", value.request_id},
        {"trace_id", value.trace_id},
        {"principal_id_hash", value.principal_id_hash},
        {"priority", enumValue(value.priority)},
        {"deadline_mono_ns", value.deadline_mono_ns},
        {"caller_endpoint_id", value.caller_endpoint_id},
        {"caller_process_epoch", value.caller_process_epoch},
        {"authorization_ref", value.authorization_ref}};
}

CallContext decodeCallContext(const nlohmann::json& value) {
    const auto caller = closedEnum<CallerModuleId>(
        value.at("caller"), 0, 16);
    return {
        caller,
        value.at("request_id").get<std::string>(),
        value.at("trace_id").get<std::string>(),
        value.at("principal_id_hash").get<std::string>(),
        closedEnum<TaskPriority>(value.at("priority"), 0, 2),
        value.at("deadline_mono_ns").get<std::int64_t>(),
        value.at("caller_endpoint_id").get<std::string>(),
        value.at("caller_process_epoch").get<std::uint64_t>(),
        value.at("authorization_ref").get<std::string>()};
}

nlohmann::json encode(const interaction::TextInput& value) {
    return {{"text", value.text},
            {"user_id", value.user_id},
            {"session_id", value.session_id},
            {"source", value.source},
            {"params", value.params}};
}

interaction::TextInput decodeTextInput(
    const nlohmann::json& value) {
    interaction::TextInput result;
    result.text = value.at("text").get<std::string>();
    result.user_id = value.at("user_id").get<std::string>();
    result.session_id = value.at("session_id").get<std::string>();
    result.source = value.at("source").get<std::string>();
    result.params =
        value.at("params").get<std::map<std::string, std::string>>();
    return result;
}

nlohmann::json encode(
    const interaction::StandardRequest& value) {
    return {
        {"request_id", value.request_id},
        {"trace_id", value.trace_id},
        {"trigger_type", value.trigger_type},
        {"text", value.text},
        {"params", value.params},
        {"timestamp_utc_ms", value.timestamp_utc_ms},
        {"deadline_mono_ns", value.deadline_mono_ns},
        {"user_id", value.user_id},
        {"session_id", value.session_id},
        {"turn_id", value.turn_id},
        {"priority", enumValue(value.priority)},
        {"resume_task_id", value.resume_task_id}};
}

interaction::StandardRequest decodeStandardRequest(
    const nlohmann::json& value) {
    interaction::StandardRequest result;
    result.request_id =
        value.at("request_id").get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    result.trigger_type =
        value.at("trigger_type").get<std::string>();
    result.text = value.at("text").get<std::string>();
    result.params =
        value.at("params").get<std::map<std::string, std::string>>();
    result.timestamp_utc_ms =
        value.at("timestamp_utc_ms").get<std::int64_t>();
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    result.user_id = value.at("user_id").get<std::string>();
    result.session_id = value.at("session_id").get<std::string>();
    result.turn_id = value.at("turn_id").get<std::uint64_t>();
    result.priority =
        closedEnum<TaskPriority>(value.at("priority"), 0, 2);
    result.resume_task_id =
        value.at("resume_task_id").get<std::string>();
    return result;
}

nlohmann::json encode(const agent_service::TurnResult& value) {
    return {
        {"request_id", value.request_id},
        {"trace_id", value.trace_id},
        {"session_id", value.session_id},
        {"turn_id", value.turn_id},
        {"reply", value.reply},
        {"success", value.success},
        {"pending", value.pending},
        {"error_code", value.error_code},
        {"error_message", value.error_message},
        {"plan_id", value.plan_id},
        {"plan_state",
         value.plan_state
             ? nlohmann::json(enumValue(*value.plan_state))
             : nlohmann::json(nullptr)},
        {"turn_summary", value.turn_summary}};
}

agent_service::TurnResult decodeTurnResult(
    const nlohmann::json& value) {
    agent_service::TurnResult result;
    result.request_id =
        value.at("request_id").get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    result.session_id = value.at("session_id").get<std::string>();
    result.turn_id = value.at("turn_id").get<std::uint64_t>();
    result.reply = value.at("reply").get<std::string>();
    result.success = value.at("success").get<bool>();
    result.pending = value.at("pending").get<bool>();
    result.error_code = value.at("error_code").get<std::string>();
    result.error_message =
        value.at("error_message").get<std::string>();
    result.plan_id = value.at("plan_id").get<std::string>();
    if (!value.at("plan_state").is_null()) {
        result.plan_state = closedEnum<orchestrator::PlanState>(
            value.at("plan_state"), 0, 5);
    }
    result.turn_summary =
        value.at("turn_summary").get<std::string>();
    return result;
}

nlohmann::json encode(const memory::MemoryContext& value) {
    auto blocks = nlohmann::json::array();
    for (const auto& block : value.blocks) {
        blocks.push_back(
            {{"memory_type", block.memory_type},
             {"content", block.content},
             {"source_memory_id", block.source_memory_id},
             {"relevance_score", block.relevance_score},
             {"session_id", block.session_id},
             {"turn_id", block.turn_id},
             {"timestamp_ms", block.timestamp_ms}});
    }
    return {{"blocks", std::move(blocks)},
            {"flattened_context", value.flattened_context}};
}

memory::MemoryContext decodeMemoryContext(
    const nlohmann::json& value) {
    memory::MemoryContext result;
    for (const auto& encoded : value.at("blocks")) {
        master_agent::memory::ContextBlock block;
        block.memory_type =
            encoded.at("memory_type").get<std::string>();
        block.content = encoded.at("content").get<std::string>();
        block.source_memory_id =
            encoded.at("source_memory_id").get<std::string>();
        block.relevance_score =
            encoded.at("relevance_score").get<double>();
        block.session_id =
            encoded.at("session_id").get<std::string>();
        block.turn_id =
            encoded.at("turn_id").get<std::uint64_t>();
        block.timestamp_ms =
            encoded.at("timestamp_ms").get<std::uint64_t>();
        result.blocks.push_back(std::move(block));
    }
    result.flattened_context =
        value.at("flattened_context").get<std::string>();
    return result;
}

nlohmann::json encode(const memory::CompletedTurn& value) {
    return {{"request", encode(value.request)},
            {"normalized_user_input",
             value.normalized_user_input},
            {"assistant_output", value.assistant_output},
            {"scene", value.scene},
            {"record_version", value.record_version}};
}

memory::CompletedTurn decodeCompletedTurn(
    const nlohmann::json& value) {
    memory::CompletedTurn result;
    result.request =
        decodeStandardRequest(value.at("request"));
    result.normalized_user_input =
        value.at("normalized_user_input").get<std::string>();
    result.assistant_output =
        value.at("assistant_output").get<std::string>();
    result.scene = value.at("scene").get<std::string>();
    result.record_version =
        value.at("record_version").get<std::uint32_t>();
    return result;
}

nlohmann::json encode(
    const atomic_service::McpToolDefinition& value) {
    return {
        {"name", value.name},
        {"title", value.title},
        {"description", value.description},
        {"input_schema", value.input_schema},
        {"output_schema", value.output_schema},
        {"annotations",
         {{"title", value.annotations.title},
          {"read_only_hint", value.annotations.read_only_hint},
          {"destructive_hint", value.annotations.destructive_hint},
          {"idempotent_hint", value.annotations.idempotent_hint},
          {"open_world_hint", value.annotations.open_world_hint}}}};
}

atomic_service::McpToolDefinition decodeMcpToolDefinition(
    const nlohmann::json& value) {
    atomic_service::McpToolDefinition result;
    result.name = value.at("name").get<std::string>();
    result.title = value.at("title").get<std::string>();
    result.description =
        value.at("description").get<std::string>();
    result.input_schema = value.at("input_schema");
    result.output_schema = value.at("output_schema");
    const auto& annotations = value.at("annotations");
    result.annotations.title =
        annotations.at("title").get<std::string>();
    result.annotations.read_only_hint =
        annotations.at("read_only_hint").get<bool>();
    result.annotations.destructive_hint =
        annotations.at("destructive_hint").get<bool>();
    result.annotations.idempotent_hint =
        annotations.at("idempotent_hint").get<bool>();
    result.annotations.open_world_hint =
        annotations.at("open_world_hint").get<bool>();
    return result;
}

nlohmann::json encode(
    const atomic_service::McpToolCatalogSnapshot& value) {
    auto tools = nlohmann::json::array();
    for (const auto& tool : value.tools) {
        tools.push_back(encode(tool));
    }
    return {
        {"snapshot_id", value.snapshot_id},
        {"mcp_protocol_version", value.mcp_protocol_version},
        {"catalog_generation", value.catalog_generation},
        {"catalog_digest", value.catalog_digest},
        {"created_at_utc_ms", value.created_at_utc_ms},
        {"tools", std::move(tools)},
        {"tool_digests", value.tool_digests},
        {"policy_digests", value.policy_digests},
        {"idempotency_policies", value.idempotency_policies},
        {"retryable_errors", value.retryable_errors},
        {"provider_ids", value.provider_ids},
        {"provider_epochs", value.provider_epochs}};
}

atomic_service::McpToolCatalogSnapshot decodeToolCatalog(
    const nlohmann::json& value) {
    atomic_service::McpToolCatalogSnapshot result;
    result.snapshot_id =
        value.at("snapshot_id").get<std::string>();
    result.mcp_protocol_version =
        value.at("mcp_protocol_version").get<std::string>();
    result.catalog_generation =
        value.at("catalog_generation").get<std::uint64_t>();
    result.catalog_digest =
        value.at("catalog_digest").get<std::string>();
    result.created_at_utc_ms =
        value.at("created_at_utc_ms").get<std::int64_t>();
    for (const auto& tool : value.at("tools")) {
        result.tools.push_back(decodeMcpToolDefinition(tool));
    }
    result.tool_digests =
        value.at("tool_digests")
            .get<std::map<std::string, std::string>>();
    result.policy_digests =
        value.at("policy_digests")
            .get<std::map<std::string, std::string>>();
    result.idempotency_policies =
        value.at("idempotency_policies")
            .get<std::map<std::string, std::string>>();
    result.retryable_errors =
        value.at("retryable_errors")
            .get<std::map<std::string,
                          std::vector<std::string>>>();
    result.provider_ids =
        value.at("provider_ids")
            .get<std::map<std::string, std::string>>();
    result.provider_epochs =
        value.at("provider_epochs")
            .get<std::map<std::string, std::uint64_t>>();
    return result;
}

nlohmann::json encode(
    const atomic_service::AtomicMcpCallEnvelope& value) {
    const auto& request = value.mcp_request;
    const auto& runtime = value.runtime;
    return {
        {"mcp_request",
         {{"jsonrpc", request.jsonrpc},
          {"id", request.id},
          {"method", request.method},
          {"name", request.name},
          {"arguments", request.arguments}}},
        {"runtime",
         {{"caller_module_id",
           enumValue(runtime.caller_module_id)},
          {"request_id", runtime.request_id},
          {"trace_id", runtime.trace_id},
          {"plan_id", runtime.plan_id},
          {"pid", runtime.pid},
          {"activation_id", runtime.activation_id},
          {"execution_id", runtime.execution_id},
          {"attempt_no", runtime.attempt_no},
          {"operation_id", runtime.operation_id},
          {"priority", enumValue(runtime.priority)},
          {"deadline_mono_ns", runtime.deadline_mono_ns},
          {"idempotency_key", runtime.idempotency_key},
          {"fencing_token", runtime.fencing_token},
          {"tool_catalog_snapshot_id",
           runtime.tool_catalog_snapshot_id},
          {"tool_digest", runtime.tool_digest},
          {"policy_digest", runtime.policy_digest},
          {"granted_permissions", runtime.granted_permissions},
          {"resource_lease_refs", runtime.resource_lease_refs},
          {"principal_id_hash", runtime.principal_id_hash},
          {"authorization_ref", runtime.authorization_ref},
          {"parent_operation_id",
           optionalValue(runtime.parent_operation_id)},
          {"parent_dispatch_id", runtime.parent_dispatch_id},
          {"parent_agent_id", runtime.parent_agent_id},
          {"parent_agent_epoch", runtime.parent_agent_epoch},
          {"parent_lease_id", runtime.parent_lease_id},
          {"parent_fencing_token",
           runtime.parent_fencing_token}}}};
}

atomic_service::AtomicMcpCallEnvelope decodeAtomicCall(
    const nlohmann::json& value) {
    atomic_service::AtomicMcpCallEnvelope result;
    const auto& request = value.at("mcp_request");
    result.mcp_request.jsonrpc =
        request.at("jsonrpc").get<std::string>();
    result.mcp_request.id = request.at("id").get<std::string>();
    result.mcp_request.method =
        request.at("method").get<std::string>();
    result.mcp_request.name =
        request.at("name").get<std::string>();
    result.mcp_request.arguments = request.at("arguments");
    const auto& runtime = value.at("runtime");
    auto& target = result.runtime;
    target.caller_module_id = closedEnum<CallerModuleId>(
        runtime.at("caller_module_id"), 0, 16);
    target.request_id =
        runtime.at("request_id").get<std::string>();
    target.trace_id =
        runtime.at("trace_id").get<std::string>();
    target.plan_id = runtime.at("plan_id").get<std::string>();
    target.pid = runtime.at("pid").get<std::string>();
    target.activation_id =
        runtime.at("activation_id").get<std::string>();
    target.execution_id =
        runtime.at("execution_id").get<std::string>();
    target.attempt_no =
        runtime.at("attempt_no").get<std::uint32_t>();
    target.operation_id =
        runtime.at("operation_id").get<std::string>();
    target.priority = closedEnum<TaskPriority>(
        runtime.at("priority"), 0, 2);
    target.deadline_mono_ns =
        runtime.at("deadline_mono_ns").get<std::int64_t>();
    target.idempotency_key =
        runtime.at("idempotency_key").get<std::string>();
    target.fencing_token =
        runtime.at("fencing_token").get<std::uint64_t>();
    target.tool_catalog_snapshot_id =
        runtime.at("tool_catalog_snapshot_id").get<std::string>();
    target.tool_digest =
        runtime.at("tool_digest").get<std::string>();
    target.policy_digest =
        runtime.at("policy_digest").get<std::string>();
    target.granted_permissions =
        runtime.at("granted_permissions")
            .get<std::vector<std::string>>();
    target.resource_lease_refs =
        runtime.at("resource_lease_refs")
            .get<std::vector<std::string>>();
    target.principal_id_hash =
        runtime.at("principal_id_hash").get<std::string>();
    target.authorization_ref =
        runtime.at("authorization_ref").get<std::string>();
    target.parent_operation_id =
        decodeOptional<std::string>(
            runtime.at("parent_operation_id"));
    target.parent_dispatch_id =
        runtime.at("parent_dispatch_id").get<std::string>();
    target.parent_agent_id =
        runtime.at("parent_agent_id").get<std::string>();
    target.parent_agent_epoch =
        runtime.at("parent_agent_epoch").get<std::uint64_t>();
    target.parent_lease_id =
        runtime.at("parent_lease_id").get<std::string>();
    target.parent_fencing_token =
        runtime.at("parent_fencing_token")
            .get<std::uint64_t>();
    return result;
}

nlohmann::json encode(
    const atomic_service::DispatchAcceptance& value) {
    return {{"accepted", value.accepted},
            {"existing", value.existing},
            {"operation_id", value.operation_id},
            {"execution_id", value.execution_id},
            {"reject_code", value.reject_code},
            {"executor_id", value.executor_id},
            {"executor_epoch", value.executor_epoch}};
}

atomic_service::DispatchAcceptance decodeAtomicAcceptance(
    const nlohmann::json& value) {
    atomic_service::DispatchAcceptance result;
    result.accepted = value.at("accepted").get<bool>();
    result.existing = value.at("existing").get<bool>();
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.execution_id =
        value.at("execution_id").get<std::string>();
    result.reject_code =
        value.at("reject_code").get<std::string>();
    result.executor_id =
        value.at("executor_id").get<std::string>();
    result.executor_epoch =
        value.at("executor_epoch").get<std::uint64_t>();
    return result;
}

nlohmann::json encode(
    const atomic_service::AtomicExecutionSnapshot& value) {
    return {
        {"envelope", encode(value.envelope)},
        {"state", enumValue(value.state)},
        {"result",
         value.result ? encodeToolResult(*value.result)
                      : nlohmann::json(nullptr)},
        {"side_effect_state", enumValue(value.side_effect_state)},
        {"completion_evidence",
         enumValue(value.completion_evidence)},
        {"error_code", value.error_code},
        {"resource_key", value.resource_key},
        {"remaining_work_units", value.remaining_work_units},
        {"control_epoch", value.control_epoch},
        {"provider_invocation",
         value.provider_invocation
             ? encodeProviderSeal(*value.provider_invocation)
             : nlohmann::json(nullptr)},
        {"retryable_hint", value.retryable_hint}};
}

atomic_service::AtomicExecutionSnapshot decodeAtomicSnapshot(
    const nlohmann::json& value) {
    atomic_service::AtomicExecutionSnapshot result;
    result.envelope = decodeAtomicCall(value.at("envelope"));
    result.state = closedEnum<
        atomic_service::AtomicExecutionState>(
        value.at("state"), 0, 7);
    if (!value.at("result").is_null()) {
        result.result = decodeToolResult(value.at("result"));
    }
    result.side_effect_state = closedEnum<SideEffectState>(
        value.at("side_effect_state"), 0, 5);
    result.completion_evidence =
        closedEnum<atomic_service::CompletionEvidence>(
            value.at("completion_evidence"), 0, 5);
    result.error_code =
        value.at("error_code").get<std::string>();
    result.resource_key =
        value.at("resource_key").get<std::string>();
    result.remaining_work_units =
        value.at("remaining_work_units").get<std::uint32_t>();
    result.control_epoch =
        value.at("control_epoch").get<std::uint64_t>();
    if (!value.at("provider_invocation").is_null()) {
        result.provider_invocation =
            decodeProviderSeal(value.at("provider_invocation"));
    }
    result.retryable_hint =
        value.at("retryable_hint").get<bool>();
    return result;
}

nlohmann::json encode(
    const atomic_service::AtomicReconcileResult& value) {
    return {
        {"operation_id", value.operation_id},
        {"execution_id", value.execution_id},
        {"tool_name", value.tool_name},
        {"status", enumValue(value.status)},
        {"observed_state", value.observed_state},
        {"fencing_token", value.fencing_token},
        {"call_tool_result",
         value.call_tool_result
             ? encodeToolResult(*value.call_tool_result)
             : nlohmann::json(nullptr)},
        {"completion_evidence",
         enumValue(value.completion_evidence)},
        {"side_effect_state", enumValue(value.side_effect_state)},
        {"invocation_seal",
         encodeProviderSeal(value.invocation_seal)},
        {"retryable_hint", value.retryable_hint}};
}

atomic_service::AtomicReconcileResult decodeAtomicReconcile(
    const nlohmann::json& value) {
    atomic_service::AtomicReconcileResult result;
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.execution_id =
        value.at("execution_id").get<std::string>();
    result.tool_name =
        value.at("tool_name").get<std::string>();
    result.status =
        closedEnum<atomic_service::ReconcileStatus>(
            value.at("status"), 0, 3);
    result.observed_state = value.at("observed_state");
    result.fencing_token =
        value.at("fencing_token").get<std::uint64_t>();
    if (!value.at("call_tool_result").is_null()) {
        result.call_tool_result =
            decodeToolResult(value.at("call_tool_result"));
    }
    result.completion_evidence =
        closedEnum<atomic_service::CompletionEvidence>(
            value.at("completion_evidence"), 0, 5);
    result.side_effect_state = closedEnum<SideEffectState>(
        value.at("side_effect_state"), 0, 5);
    result.invocation_seal =
        decodeProviderSeal(value.at("invocation_seal"));
    result.retryable_hint =
        value.at("retryable_hint").get<bool>();
    return result;
}

nlohmann::json encode(
    const agent_dispatch::DispatchTask& value) {
    return {
        {"caller_module_id", enumValue(value.caller_module_id)},
        {"request_id", value.request_id},
        {"plan_id", value.plan_id},
        {"pid", value.pid},
        {"activation_id", value.activation_id},
        {"execution_id", value.execution_id},
        {"attempt_no", value.attempt_no},
        {"operation_id", value.operation_id},
        {"task_id", value.task_id},
        {"action", value.action},
        {"target_agent", value.target_agent},
        {"allow_agent_fallback", value.allow_agent_fallback},
        {"params", value.params},
        {"input_schema_version", value.input_schema_version},
        {"expected_output_schema_version",
         value.expected_output_schema_version},
        {"priority", enumValue(value.priority)},
        {"deadline_mono_ns", value.deadline_mono_ns},
        {"idempotency_key", value.idempotency_key},
        {"fencing_token", value.fencing_token},
        {"capability_digest", value.capability_digest},
        {"capacity_epoch", value.capacity_epoch},
        {"resource_lease_refs", value.resource_lease_refs},
        {"granted_permissions", value.granted_permissions},
        {"allowed_child_capabilities",
         value.allowed_child_capabilities},
        {"child_authorization_digest",
         value.child_authorization_digest},
        {"principal_id_hash", value.principal_id_hash},
        {"authorization_ref", value.authorization_ref},
        {"trace_id", value.trace_id}};
}

agent_dispatch::DispatchTask decodeDispatchTask(
    const nlohmann::json& value) {
    agent_dispatch::DispatchTask result;
    result.caller_module_id = closedEnum<CallerModuleId>(
        value.at("caller_module_id"), 0, 16);
    result.request_id =
        value.at("request_id").get<std::string>();
    result.plan_id = value.at("plan_id").get<std::string>();
    result.pid = value.at("pid").get<std::string>();
    result.activation_id =
        value.at("activation_id").get<std::string>();
    result.execution_id =
        value.at("execution_id").get<std::string>();
    result.attempt_no =
        value.at("attempt_no").get<std::uint32_t>();
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.task_id = value.at("task_id").get<std::string>();
    result.action = value.at("action").get<std::string>();
    result.target_agent =
        value.at("target_agent").get<std::string>();
    result.allow_agent_fallback =
        value.at("allow_agent_fallback").get<bool>();
    result.params = value.at("params");
    result.input_schema_version =
        value.at("input_schema_version").get<std::uint32_t>();
    result.expected_output_schema_version =
        value.at("expected_output_schema_version")
            .get<std::uint32_t>();
    result.priority =
        closedEnum<TaskPriority>(value.at("priority"), 0, 2);
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    result.idempotency_key =
        value.at("idempotency_key").get<std::string>();
    result.fencing_token =
        value.at("fencing_token").get<std::uint64_t>();
    result.capability_digest =
        value.at("capability_digest").get<std::string>();
    result.capacity_epoch =
        value.at("capacity_epoch").get<std::uint64_t>();
    result.resource_lease_refs =
        value.at("resource_lease_refs")
            .get<std::vector<std::string>>();
    result.granted_permissions =
        value.at("granted_permissions")
            .get<std::vector<std::string>>();
    result.allowed_child_capabilities =
        value.at("allowed_child_capabilities")
            .get<std::vector<std::string>>();
    result.child_authorization_digest =
        value.at("child_authorization_digest").get<std::string>();
    result.principal_id_hash =
        value.at("principal_id_hash").get<std::string>();
    result.authorization_ref =
        value.at("authorization_ref").get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    return result;
}

nlohmann::json encode(
    const agent_dispatch::DispatchAcceptance& value) {
    return {{"accepted", value.accepted},
            {"existing", value.existing},
            {"dispatch_id", value.dispatch_id},
            {"operation_id", value.operation_id},
            {"reject_code", value.reject_code}};
}

agent_dispatch::DispatchAcceptance decodeDispatchAcceptance(
    const nlohmann::json& value) {
    agent_dispatch::DispatchAcceptance result;
    result.accepted = value.at("accepted").get<bool>();
    result.existing = value.at("existing").get<bool>();
    result.dispatch_id =
        value.at("dispatch_id").get<std::string>();
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.reject_code =
        value.at("reject_code").get<std::string>();
    return result;
}

nlohmann::json encode(
    const agent_dispatch::DispatchSnapshot& value) {
    const auto& route = value.route;
    return {
        {"dispatch_id", value.dispatch_id},
        {"task", encode(value.task)},
        {"route",
         {{"routed", route.routed},
          {"agent_id", route.agent_id},
          {"agent_epoch", route.agent_epoch},
          {"manifest_digest", route.manifest_digest},
          {"capability_version", route.capability_version},
          {"lease_id", route.lease_id},
          {"reason_code", route.reason_code},
          {"route_decided_at_utc_ms",
           route.route_decided_at_utc_ms}}},
        {"state", enumValue(value.state)},
        {"result", value.result},
        {"error_code", value.error_code},
        {"side_effect_state", enumValue(value.side_effect_state)},
        {"checkpoint_ref", value.checkpoint_ref},
        {"control_epoch", value.control_epoch},
        {"enqueue_sequence", value.enqueue_sequence},
        {"retryable_hint", value.retryable_hint}};
}

agent_dispatch::DispatchSnapshot decodeDispatchSnapshot(
    const nlohmann::json& value) {
    agent_dispatch::DispatchSnapshot result;
    result.dispatch_id =
        value.at("dispatch_id").get<std::string>();
    result.task = decodeDispatchTask(value.at("task"));
    const auto& route = value.at("route");
    result.route.routed = route.at("routed").get<bool>();
    result.route.agent_id =
        route.at("agent_id").get<std::string>();
    result.route.agent_epoch =
        route.at("agent_epoch").get<std::uint64_t>();
    result.route.manifest_digest =
        route.at("manifest_digest").get<std::string>();
    result.route.capability_version =
        route.at("capability_version").get<std::string>();
    result.route.lease_id =
        route.at("lease_id").get<std::string>();
    result.route.reason_code =
        route.at("reason_code").get<std::string>();
    result.route.route_decided_at_utc_ms =
        route.at("route_decided_at_utc_ms").get<std::int64_t>();
    result.state = closedEnum<agent_dispatch::DispatchState>(
        value.at("state"), 0, 7);
    result.result = value.at("result");
    result.error_code =
        value.at("error_code").get<std::string>();
    result.side_effect_state = closedEnum<SideEffectState>(
        value.at("side_effect_state"), 0, 5);
    result.checkpoint_ref =
        value.at("checkpoint_ref").get<std::string>();
    result.control_epoch =
        value.at("control_epoch").get<std::uint64_t>();
    result.enqueue_sequence =
        value.at("enqueue_sequence").get<std::uint64_t>();
    result.retryable_hint =
        value.at("retryable_hint").get<bool>();
    return result;
}

nlohmann::json encode(
    const agent_dispatch::AgentDispatchCapacity& value) {
    nlohmann::json depth = nlohmann::json::object();
    for (const auto& item : value.queue_depth_by_priority) {
        depth[std::to_string(enumValue(item.first))] =
            item.second;
    }
    return {
        {"target_id", value.target_id},
        {"capacity_epoch", value.capacity_epoch},
        {"available_credits", value.available_credits},
        {"max_inflight", value.max_inflight},
        {"reserved_p0_credits", value.reserved_p0_credits},
        {"queue_depth_by_priority", std::move(depth)},
        {"health_state", value.health_state}};
}

agent_dispatch::AgentDispatchCapacity decodeDispatchCapacity(
    const nlohmann::json& value) {
    agent_dispatch::AgentDispatchCapacity result;
    result.target_id =
        value.at("target_id").get<std::string>();
    result.capacity_epoch =
        value.at("capacity_epoch").get<std::uint64_t>();
    result.available_credits =
        value.at("available_credits").get<std::uint32_t>();
    result.max_inflight =
        value.at("max_inflight").get<std::uint32_t>();
    result.reserved_p0_credits =
        value.at("reserved_p0_credits").get<std::uint32_t>();
    for (const auto& item :
         value.at("queue_depth_by_priority").items()) {
        const int encoded = std::stoi(item.key());
        if (encoded < 0 || encoded > 2) {
            throw std::runtime_error("invalid queue priority");
        }
        result.queue_depth_by_priority[
            static_cast<TaskPriority>(encoded)] =
            item.value().get<std::size_t>();
    }
    result.health_state =
        value.at("health_state").get<std::string>();
    return result;
}

nlohmann::json encode(
    const inference::InferenceRequest& value) {
    auto segments = nlohmann::json::array();
    for (const auto& segment : value.prompt_segments) {
        segments.push_back(
            {{"segment_id", segment.segment_id},
             {"digest", segment.digest},
             {"token_count", segment.token_count}});
    }
    const auto& admission = value.admission;
    return {
        {"job_id", value.job_id},
        {"request_id", value.request_id},
        {"parent_operation_id", value.parent_operation_id},
        {"session_id", value.session_id},
        {"prompt", value.prompt},
        {"prompt_digest", value.prompt_digest},
        {"prompt_segments", std::move(segments)},
        {"prompt_protocol_version", value.prompt_protocol_version},
        {"inference_phase", value.inference_phase},
        {"model", value.model},
        {"adapter", value.adapter},
        {"kv_reuse_policy", value.kv_reuse_policy},
        {"priority", enumValue(value.priority)},
        {"deadline_mono_ns", value.deadline_mono_ns},
        {"idempotency_key", value.idempotency_key},
        {"admission",
         {{"principal_id", admission.principal_id},
          {"caller_module_id",
           enumValue(admission.caller_module_id)},
          {"source_request_id", admission.source_request_id},
          {"granted_priority",
           enumValue(admission.granted_priority)},
          {"p0_authorization", admission.p0_authorization},
          {"policy_snapshot_id",
           admission.policy_snapshot_id},
          {"allowed_model_profiles",
           admission.allowed_model_profiles},
          {"max_input_tokens", admission.max_input_tokens},
          {"max_output_tokens", admission.max_output_tokens},
          {"deadline_mono_ns", admission.deadline_mono_ns},
          {"signature_ref", admission.signature_ref}}},
        {"parent_dispatch_id", value.parent_dispatch_id},
        {"parent_agent_id", value.parent_agent_id},
        {"parent_agent_epoch", value.parent_agent_epoch},
        {"parent_lease_id", value.parent_lease_id},
        {"parent_fencing_token", value.parent_fencing_token},
        {"reality", value.reality},
        {"trace_id", value.trace_id}};
}

inference::InferenceRequest decodeInferenceRequest(
    const nlohmann::json& value) {
    inference::InferenceRequest result;
    result.job_id = value.at("job_id").get<std::string>();
    result.request_id =
        value.at("request_id").get<std::string>();
    result.parent_operation_id =
        value.at("parent_operation_id").get<std::string>();
    result.session_id =
        value.at("session_id").get<std::string>();
    result.prompt = value.at("prompt").get<std::string>();
    result.prompt_digest =
        value.at("prompt_digest").get<std::string>();
    for (const auto& encoded : value.at("prompt_segments")) {
        kv_cache::PromptSegment segment;
        segment.segment_id =
            encoded.at("segment_id").get<std::string>();
        segment.digest =
            encoded.at("digest").get<std::string>();
        segment.token_count =
            encoded.at("token_count").get<std::uint32_t>();
        result.prompt_segments.push_back(std::move(segment));
    }
    result.prompt_protocol_version =
        value.at("prompt_protocol_version").get<std::string>();
    result.inference_phase =
        value.at("inference_phase").get<std::string>();
    result.model = value.at("model").get<std::string>();
    result.adapter = value.at("adapter").get<std::string>();
    result.kv_reuse_policy =
        value.at("kv_reuse_policy").get<std::string>();
    result.priority =
        closedEnum<TaskPriority>(value.at("priority"), 0, 2);
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    result.idempotency_key =
        value.at("idempotency_key").get<std::string>();
    const auto& admission = value.at("admission");
    result.admission.principal_id =
        admission.at("principal_id").get<std::string>();
    result.admission.caller_module_id =
        closedEnum<CallerModuleId>(
            admission.at("caller_module_id"), 0, 16);
    result.admission.source_request_id =
        admission.at("source_request_id").get<std::string>();
    result.admission.granted_priority =
        closedEnum<TaskPriority>(
            admission.at("granted_priority"), 0, 2);
    result.admission.p0_authorization =
        admission.at("p0_authorization").get<bool>();
    result.admission.policy_snapshot_id =
        admission.at("policy_snapshot_id").get<std::string>();
    result.admission.allowed_model_profiles =
        admission.at("allowed_model_profiles")
            .get<std::vector<std::string>>();
    result.admission.max_input_tokens =
        admission.at("max_input_tokens").get<std::uint32_t>();
    result.admission.max_output_tokens =
        admission.at("max_output_tokens").get<std::uint32_t>();
    result.admission.deadline_mono_ns =
        admission.at("deadline_mono_ns").get<std::int64_t>();
    result.admission.signature_ref =
        admission.at("signature_ref").get<std::string>();
    result.parent_dispatch_id =
        value.at("parent_dispatch_id").get<std::string>();
    result.parent_agent_id =
        value.at("parent_agent_id").get<std::string>();
    result.parent_agent_epoch =
        value.at("parent_agent_epoch").get<std::uint64_t>();
    result.parent_lease_id =
        value.at("parent_lease_id").get<std::string>();
    result.parent_fencing_token =
        value.at("parent_fencing_token").get<std::uint64_t>();
    result.reality = value.at("reality").get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    return result;
}

nlohmann::json encode(
    const inference::InferenceAcceptance& value) {
    return {{"accepted", value.accepted},
            {"existing", value.existing},
            {"job_id", value.job_id},
            {"reject_code", value.reject_code}};
}

inference::InferenceAcceptance decodeInferenceAcceptance(
    const nlohmann::json& value) {
    inference::InferenceAcceptance result;
    result.accepted = value.at("accepted").get<bool>();
    result.existing = value.at("existing").get<bool>();
    result.job_id = value.at("job_id").get<std::string>();
    result.reject_code =
        value.at("reject_code").get<std::string>();
    return result;
}

nlohmann::json encode(
    const inference::InferenceJobSnapshot& value) {
    nlohmann::json output = nullptr;
    if (value.result) {
        const auto& item = *value.result;
        output = {
            {"raw_output", item.raw_output},
            {"finish_reason", item.finish_reason},
            {"model_id", item.model_id},
            {"model_digest", item.model_digest},
            {"job_id", item.job_id},
            {"operation_id", item.operation_id},
            {"replica_id", item.replica_id},
            {"replica_epoch", item.replica_epoch},
            {"lease_id", item.lease_id},
            {"fencing_token", item.fencing_token},
            {"control_epoch", item.control_epoch},
            {"attempt_id", item.attempt_id},
            {"prompt_digest", item.prompt_digest},
            {"invocation_id", item.invocation_id},
            {"prompt_token_count", item.prompt_token_count},
            {"generated_token_count",
             item.generated_token_count},
            {"total_latency_ms", item.total_latency_ms},
            {"runtime_backend", item.runtime_backend},
            {"reality", item.reality},
            {"output_digest", item.output_digest}};
    }
    return {
        {"job_id", value.job_id},
        {"attempt_id", value.attempt_id},
        {"operation_id", value.operation_id},
        {"state", enumValue(value.state)},
        {"base_priority", enumValue(value.base_priority)},
        {"effective_priority",
         enumValue(value.effective_priority)},
        {"enqueue_sequence", value.enqueue_sequence},
        {"queued_at_mono_ns", value.queued_at_mono_ns},
        {"started_at_mono_ns", value.started_at_mono_ns},
        {"deadline_mono_ns", value.deadline_mono_ns},
        {"replica_id", value.replica_id},
        {"replica_epoch", value.replica_epoch},
        {"lease_id", value.lease_id},
        {"fencing_token", value.fencing_token},
        {"stage", value.stage},
        {"checkpoint_ref", value.checkpoint_ref},
        {"result", std::move(output)},
        {"last_error",
         value.last_error ? encodeError(*value.last_error)
                          : nlohmann::json(nullptr)},
        {"control_epoch", value.control_epoch},
        {"version", value.version}};
}

inference::InferenceJobSnapshot decodeInferenceSnapshot(
    const nlohmann::json& value) {
    inference::InferenceJobSnapshot result;
    result.job_id = value.at("job_id").get<std::string>();
    result.attempt_id =
        value.at("attempt_id").get<std::string>();
    result.operation_id =
        value.at("operation_id").get<std::string>();
    result.state = closedEnum<inference::InferenceJobState>(
        value.at("state"), 0, 6);
    result.base_priority = closedEnum<TaskPriority>(
        value.at("base_priority"), 0, 2);
    result.effective_priority = closedEnum<TaskPriority>(
        value.at("effective_priority"), 0, 2);
    result.enqueue_sequence =
        value.at("enqueue_sequence").get<std::uint64_t>();
    result.queued_at_mono_ns =
        value.at("queued_at_mono_ns").get<std::int64_t>();
    result.started_at_mono_ns =
        value.at("started_at_mono_ns").get<std::int64_t>();
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    result.replica_id =
        value.at("replica_id").get<std::string>();
    result.replica_epoch =
        value.at("replica_epoch").get<std::uint64_t>();
    result.lease_id = value.at("lease_id").get<std::string>();
    result.fencing_token =
        value.at("fencing_token").get<std::uint64_t>();
    result.stage = value.at("stage").get<std::string>();
    result.checkpoint_ref =
        value.at("checkpoint_ref").get<std::string>();
    if (!value.at("result").is_null()) {
        const auto& encoded = value.at("result");
        inference::InferenceOutput output;
        output.raw_output =
            encoded.at("raw_output").get<std::string>();
        output.finish_reason =
            encoded.at("finish_reason").get<std::string>();
        output.model_id =
            encoded.at("model_id").get<std::string>();
        output.model_digest =
            encoded.at("model_digest").get<std::string>();
        output.job_id =
            encoded.at("job_id").get<std::string>();
        output.operation_id =
            encoded.at("operation_id").get<std::string>();
        output.replica_id =
            encoded.at("replica_id").get<std::string>();
        output.replica_epoch =
            encoded.at("replica_epoch").get<std::uint64_t>();
        output.lease_id =
            encoded.at("lease_id").get<std::string>();
        output.fencing_token =
            encoded.at("fencing_token").get<std::uint64_t>();
        output.control_epoch =
            encoded.at("control_epoch").get<std::uint64_t>();
        output.attempt_id =
            encoded.at("attempt_id").get<std::string>();
        output.prompt_digest =
            encoded.at("prompt_digest").get<std::string>();
        output.invocation_id =
            encoded.at("invocation_id").get<std::string>();
        output.prompt_token_count =
            encoded.at("prompt_token_count").get<std::uint32_t>();
        output.generated_token_count =
            encoded.at("generated_token_count")
                .get<std::uint32_t>();
        output.total_latency_ms =
            encoded.at("total_latency_ms").get<std::uint64_t>();
        output.runtime_backend =
            encoded.at("runtime_backend").get<std::string>();
        output.reality =
            encoded.at("reality").get<std::string>();
        output.output_digest =
            encoded.at("output_digest").get<std::string>();
        result.result = std::move(output);
    }
    if (!value.at("last_error").is_null()) {
        result.last_error =
            decodeError(value.at("last_error"));
    }
    result.control_epoch =
        value.at("control_epoch").get<std::uint64_t>();
    result.version = value.at("version").get<std::uint64_t>();
    return result;
}

nlohmann::json encode(
    const orchestrator::IntentDAG& value) {
    auto nodes = nlohmann::json::array();
    for (const auto& node : value.nodes) {
        nodes.push_back({
            {"node_id", node.node_id},
            {"node_type", node.node_type},
            {"executor", node.executor},
            {"action", node.action},
            {"target_agent", node.target_agent},
            {"allow_agent_fallback", node.allow_agent_fallback},
            {"params", node.params},
            {"dependencies", node.dependencies},
            {"input_schema_version",
             node.input_schema_version},
            {"expected_output_schema_version",
             node.expected_output_schema_version},
            {"base_priority", enumValue(node.base_priority)},
            {"deadline_mono_ns", node.deadline_mono_ns},
            {"max_attempts", node.max_attempts},
            {"resource_requirements",
             node.resource_requirements}});
    }
    auto edges = nlohmann::json::array();
    for (const auto& edge : value.edges) {
        edges.push_back({
            {"edge_id", edge.edge_id},
            {"from_node_id", edge.from_node_id},
            {"to_node_id", edge.to_node_id},
            {"edge_type", edge.edge_type},
            {"required", edge.required}});
    }
    return {{"dag_id", value.dag_id},
            {"request_id", value.request_id},
            {"nodes", std::move(nodes)},
            {"edges", std::move(edges)},
            {"priority", enumValue(value.priority)},
            {"deadline_mono_ns", value.deadline_mono_ns},
            {"schema_version", value.schema_version},
            {"idempotency_key", value.idempotency_key}};
}

orchestrator::IntentDAG decodeIntentDAG(
    const nlohmann::json& value) {
    orchestrator::IntentDAG result;
    result.dag_id = value.at("dag_id").get<std::string>();
    result.request_id =
        value.at("request_id").get<std::string>();
    for (const auto& encoded : value.at("nodes")) {
        orchestrator::DAGNode node;
        node.node_id =
            encoded.at("node_id").get<std::string>();
        node.node_type =
            encoded.at("node_type").get<std::string>();
        node.executor =
            encoded.at("executor").get<std::string>();
        node.action = encoded.at("action").get<std::string>();
        node.target_agent =
            encoded.at("target_agent").get<std::string>();
        node.allow_agent_fallback =
            encoded.at("allow_agent_fallback").get<bool>();
        node.params = encoded.at("params");
        node.dependencies =
            encoded.at("dependencies")
                .get<std::vector<std::string>>();
        node.input_schema_version =
            encoded.at("input_schema_version")
                .get<std::uint32_t>();
        node.expected_output_schema_version =
            encoded.at("expected_output_schema_version")
                .get<std::uint32_t>();
        node.base_priority = closedEnum<TaskPriority>(
            encoded.at("base_priority"), 0, 2);
        node.deadline_mono_ns =
            encoded.at("deadline_mono_ns").get<std::int64_t>();
        node.max_attempts =
            encoded.at("max_attempts").get<std::uint32_t>();
        node.resource_requirements =
            encoded.at("resource_requirements")
                .get<std::vector<std::string>>();
        result.nodes.push_back(std::move(node));
    }
    for (const auto& encoded : value.at("edges")) {
        orchestrator::DAGEdge edge;
        edge.edge_id =
            encoded.at("edge_id").get<std::string>();
        edge.from_node_id =
            encoded.at("from_node_id").get<std::string>();
        edge.to_node_id =
            encoded.at("to_node_id").get<std::string>();
        edge.edge_type =
            encoded.at("edge_type").get<std::string>();
        edge.required = encoded.at("required").get<bool>();
        result.edges.push_back(std::move(edge));
    }
    result.priority =
        closedEnum<TaskPriority>(value.at("priority"), 0, 2);
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    result.schema_version =
        value.at("schema_version").get<std::uint32_t>();
    result.idempotency_key =
        value.at("idempotency_key").get<std::string>();
    return result;
}

nlohmann::json encode(
    const orchestrator::AdmissionContext& value) {
    nlohmann::json policies = nlohmann::json::object();
    for (const auto& item : value.retry_policies) {
        policies[item.first] = {
            {"idempotency_policy",
             item.second.idempotency_policy},
            {"retryable_errors",
             item.second.retryable_errors},
            {"base_backoff_ns", item.second.base_backoff_ns},
            {"max_backoff_ns", item.second.max_backoff_ns}};
    }
    return {
        {"principal_id_hash", value.principal_id_hash},
        {"source_type", value.source_type},
        {"granted_priority", enumValue(value.granted_priority)},
        {"p0_authorization", value.p0_authorization},
        {"p0_allowed_capabilities",
         value.p0_allowed_capabilities},
        {"policy_snapshot_id", value.policy_snapshot_id},
        {"policy_digest", value.policy_digest},
        {"authorization_ref", value.authorization_ref},
        {"max_nodes", value.max_nodes},
        {"allowed_capabilities", value.allowed_capabilities},
        {"granted_permissions", value.granted_permissions},
        {"retry_policies", std::move(policies)},
        {"retry_policy_digest", value.retry_policy_digest},
        {"deadline_mono_ns", value.deadline_mono_ns}};
}

orchestrator::AdmissionContext decodeAdmission(
    const nlohmann::json& value) {
    orchestrator::AdmissionContext result;
    result.principal_id_hash =
        value.at("principal_id_hash").get<std::string>();
    result.source_type =
        value.at("source_type").get<std::string>();
    result.granted_priority = closedEnum<TaskPriority>(
        value.at("granted_priority"), 0, 2);
    result.p0_authorization =
        value.at("p0_authorization").get<bool>();
    result.p0_allowed_capabilities =
        value.at("p0_allowed_capabilities")
            .get<std::set<std::string>>();
    result.policy_snapshot_id =
        value.at("policy_snapshot_id").get<std::string>();
    result.policy_digest =
        value.at("policy_digest").get<std::string>();
    result.authorization_ref =
        value.at("authorization_ref").get<std::string>();
    result.max_nodes = value.at("max_nodes").get<std::size_t>();
    result.allowed_capabilities =
        value.at("allowed_capabilities")
            .get<std::set<std::string>>();
    result.granted_permissions =
        value.at("granted_permissions")
            .get<std::set<std::string>>();
    for (const auto& item :
         value.at("retry_policies").items()) {
        orchestrator::CapabilityRetryPolicy policy;
        policy.idempotency_policy =
            item.value()
                .at("idempotency_policy")
                .get<std::string>();
        policy.retryable_errors =
            item.value()
                .at("retryable_errors")
                .get<std::set<std::string>>();
        policy.base_backoff_ns =
            item.value()
                .at("base_backoff_ns")
                .get<std::int64_t>();
        policy.max_backoff_ns =
            item.value()
                .at("max_backoff_ns")
                .get<std::int64_t>();
        result.retry_policies[item.key()] = std::move(policy);
    }
    result.retry_policy_digest =
        value.at("retry_policy_digest").get<std::string>();
    result.deadline_mono_ns =
        value.at("deadline_mono_ns").get<std::int64_t>();
    return result;
}

nlohmann::json encode(
    const orchestrator::OrchestratorSubmitRequest& value) {
    return {{"dag", encode(value.dag)},
            {"admission", encode(value.admission)},
            {"idempotency_key", value.idempotency_key},
            {"expected_capability_digest",
             value.expected_capability_digest},
            {"trace_id", value.trace_id},
            {"submitted_at_utc_ms", value.submitted_at_utc_ms}};
}

orchestrator::OrchestratorSubmitRequest
decodeOrchestratorSubmit(const nlohmann::json& value) {
    orchestrator::OrchestratorSubmitRequest result;
    result.dag = decodeIntentDAG(value.at("dag"));
    result.admission = decodeAdmission(value.at("admission"));
    result.idempotency_key =
        value.at("idempotency_key").get<std::string>();
    result.expected_capability_digest =
        value.at("expected_capability_digest")
            .get<std::string>();
    result.trace_id = value.at("trace_id").get<std::string>();
    result.submitted_at_utc_ms =
        value.at("submitted_at_utc_ms").get<std::int64_t>();
    return result;
}

nlohmann::json encode(
    const orchestrator::ValidationResult& value) {
    return {{"valid", value.valid},
            {"reject_code", value.reject_code},
            {"details", value.details}};
}

orchestrator::ValidationResult decodeValidationResult(
    const nlohmann::json& value) {
    orchestrator::ValidationResult result;
    result.valid = value.at("valid").get<bool>();
    result.reject_code =
        value.at("reject_code").get<std::string>();
    result.details =
        value.at("details").get<std::vector<std::string>>();
    return result;
}

nlohmann::json encode(
    const orchestrator::PlanCommitResult& value) {
    return {{"accepted", value.accepted},
            {"existing", value.existing},
            {"plan_id", value.plan_id},
            {"node_id_to_pid", value.node_id_to_pid},
            {"summary_priority", enumValue(value.summary_priority)},
            {"committed_at_utc_ms", value.committed_at_utc_ms},
            {"reject_code", value.reject_code}};
}

orchestrator::PlanCommitResult decodePlanCommit(
    const nlohmann::json& value) {
    orchestrator::PlanCommitResult result;
    result.accepted = value.at("accepted").get<bool>();
    result.existing = value.at("existing").get<bool>();
    result.plan_id = value.at("plan_id").get<std::string>();
    result.node_id_to_pid =
        value.at("node_id_to_pid")
            .get<std::map<std::string, std::string>>();
    result.summary_priority = closedEnum<TaskPriority>(
        value.at("summary_priority"), 0, 2);
    result.committed_at_utc_ms =
        value.at("committed_at_utc_ms").get<std::int64_t>();
    result.reject_code =
        value.at("reject_code").get<std::string>();
    return result;
}

nlohmann::json encode(
    const orchestrator::TaskPlanSnapshot& value) {
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& item : value.nodes) {
        const auto& node = item.second;
        nodes[item.first] = {
            {"definition",
             encode(orchestrator::IntentDAG{
                 "node-codec", value.request_id,
                 {node.definition}, {}, node.effective_priority,
                 value.effective_deadline_mono_ns, 2,
                 "node-codec"})
                 .at("nodes")
                 .at(0)},
            {"pid", node.pid},
            {"activation_id", node.activation_id},
            {"state", enumValue(node.state)},
            {"effective_priority",
             enumValue(node.effective_priority)},
            {"attempt_count", node.attempt_count},
            {"execution_id", node.execution_id},
            {"operation_id", node.operation_id},
            {"fencing_token", node.fencing_token},
            {"result", node.result},
            {"error_code", node.error_code},
            {"side_effect_state",
             enumValue(node.side_effect_state)},
            {"retryable_hint", node.retryable_hint},
            {"retry_at_mono_ns", node.retry_at_mono_ns}};
    }
    return {
        {"plan_id", value.plan_id},
        {"request_id", value.request_id},
        {"state", enumValue(value.state)},
        {"summary_priority", enumValue(value.summary_priority)},
        {"active_priority", enumValue(value.active_priority)},
        {"effective_deadline_mono_ns",
         value.effective_deadline_mono_ns},
        {"capability_snapshot_id",
         value.capability_snapshot_id},
        {"policy_snapshot_id", value.policy_snapshot_id},
        {"control_epoch", value.control_epoch},
        {"version", value.version},
        {"nodes", std::move(nodes)},
        {"trace_id", value.trace_id}};
}

orchestrator::TaskPlanSnapshot decodePlanSnapshot(
    const nlohmann::json& value) {
    orchestrator::TaskPlanSnapshot result;
    result.plan_id = value.at("plan_id").get<std::string>();
    result.request_id =
        value.at("request_id").get<std::string>();
    result.state = closedEnum<orchestrator::PlanState>(
        value.at("state"), 0, 5);
    result.summary_priority = closedEnum<TaskPriority>(
        value.at("summary_priority"), 0, 2);
    result.active_priority = closedEnum<TaskPriority>(
        value.at("active_priority"), 0, 2);
    result.effective_deadline_mono_ns =
        value.at("effective_deadline_mono_ns")
            .get<std::int64_t>();
    result.capability_snapshot_id =
        value.at("capability_snapshot_id").get<std::string>();
    result.policy_snapshot_id =
        value.at("policy_snapshot_id").get<std::string>();
    result.control_epoch =
        value.at("control_epoch").get<std::uint64_t>();
    result.version = value.at("version").get<std::uint64_t>();
    for (const auto& item : value.at("nodes").items()) {
        orchestrator::NodeRuntimeSnapshot node;
        const auto& encoded = item.value();
        const auto definition_container =
            nlohmann::json{
                {"dag_id", "node-codec"},
                {"request_id", result.request_id},
                {"nodes",
                 nlohmann::json::array(
                     {encoded.at("definition")})},
                {"edges", nlohmann::json::array()},
                {"priority",
                 encoded.at("effective_priority")},
                {"deadline_mono_ns",
                 result.effective_deadline_mono_ns},
                {"schema_version", 2},
                {"idempotency_key", "node-codec"}};
        node.definition =
            decodeIntentDAG(definition_container).nodes.at(0);
        node.pid = encoded.at("pid").get<std::string>();
        node.activation_id =
            encoded.at("activation_id").get<std::string>();
        node.state =
            closedEnum<orchestrator::ActivationState>(
                encoded.at("state"), 0, 10);
        node.effective_priority = closedEnum<TaskPriority>(
            encoded.at("effective_priority"), 0, 2);
        node.attempt_count =
            encoded.at("attempt_count").get<std::uint32_t>();
        node.execution_id =
            encoded.at("execution_id").get<std::string>();
        node.operation_id =
            encoded.at("operation_id").get<std::string>();
        node.fencing_token =
            encoded.at("fencing_token").get<std::uint64_t>();
        node.result = encoded.at("result");
        node.error_code =
            encoded.at("error_code").get<std::string>();
        node.side_effect_state = closedEnum<SideEffectState>(
            encoded.at("side_effect_state"), 0, 5);
        node.retryable_hint =
            encoded.at("retryable_hint").get<bool>();
        node.retry_at_mono_ns =
            encoded.at("retry_at_mono_ns").get<std::int64_t>();
        result.nodes[item.key()] = std::move(node);
    }
    result.trace_id = value.at("trace_id").get<std::string>();
    return result;
}

nlohmann::json encode(
    const data_log::LogEventBatch& value) {
    auto records = nlohmann::json::array();
    for (const auto& event : value.records) {
        records.push_back({
            {"event_id", event.event_id},
            {"schema_version", event.schema_version},
            {"event_type", event.event_type},
            {"module", event.module},
            {"interface_name", event.interface_name},
            {"operation", event.operation},
            {"context", encodeObservation(event.context)},
            {"old_state", optionalValue(event.old_state)},
            {"new_state", optionalValue(event.new_state)},
            {"outcome", event.outcome},
            {"error_ref", optionalValue(event.error_ref)},
            {"occurred_at_utc_ms", event.occurred_at_utc_ms},
            {"occurred_at_mono_ns", event.occurred_at_mono_ns},
            {"severity", enumValue(event.severity)},
            {"requested_durability",
             enumValue(event.requested_durability)},
            {"privacy_labels", event.privacy_labels},
            {"payload_summary_json",
             event.payload_summary_json},
            {"redaction_policy_version",
             event.redaction_policy_version}});
    }
    return {
        {"batch_id", value.batch_id},
        {"producer_endpoint_id", value.producer_endpoint_id},
        {"producer_epoch", value.producer_epoch},
        {"first_sequence", value.first_sequence},
        {"last_sequence", value.last_sequence},
        {"checksum", value.checksum},
        {"redaction_proof", value.redaction_proof},
        {"records", std::move(records)}};
}

data_log::LogEventBatch decodeLogEventBatch(
    const nlohmann::json& value) {
    data_log::LogEventBatch result;
    result.batch_id = value.at("batch_id").get<std::string>();
    result.producer_endpoint_id =
        value.at("producer_endpoint_id").get<std::string>();
    result.producer_epoch =
        value.at("producer_epoch").get<std::uint64_t>();
    result.first_sequence =
        value.at("first_sequence").get<std::uint64_t>();
    result.last_sequence =
        value.at("last_sequence").get<std::uint64_t>();
    result.checksum = value.at("checksum").get<std::string>();
    result.redaction_proof =
        value.at("redaction_proof").get<std::string>();
    for (const auto& encoded : value.at("records")) {
        data_log::LogEvent event;
        event.event_id =
            encoded.at("event_id").get<std::string>();
        event.schema_version =
            encoded.at("schema_version").get<std::uint32_t>();
        event.event_type =
            encoded.at("event_type").get<std::string>();
        event.module =
            encoded.at("module").get<std::string>();
        event.interface_name =
            encoded.at("interface_name").get<std::string>();
        event.operation =
            encoded.at("operation").get<std::string>();
        event.context =
            decodeObservation(encoded.at("context"));
        event.old_state =
            decodeOptional<std::string>(
                encoded.at("old_state"));
        event.new_state =
            decodeOptional<std::string>(
                encoded.at("new_state"));
        event.outcome =
            encoded.at("outcome").get<std::string>();
        event.error_ref =
            decodeOptional<std::string>(
                encoded.at("error_ref"));
        event.occurred_at_utc_ms =
            encoded.at("occurred_at_utc_ms")
                .get<std::int64_t>();
        event.occurred_at_mono_ns =
            encoded.at("occurred_at_mono_ns")
                .get<std::int64_t>();
        event.severity =
            closedEnum<data_log::EventSeverity>(
                encoded.at("severity"), 0, 4);
        event.requested_durability =
            closedEnum<data_log::DurabilityClass>(
                encoded.at("requested_durability"), 0, 4);
        event.privacy_labels =
            encoded.at("privacy_labels")
                .get<std::vector<std::string>>();
        event.payload_summary_json =
            encoded.at("payload_summary_json")
                .get<std::string>();
        event.redaction_policy_version =
            encoded.at("redaction_policy_version")
                .get<std::string>();
        result.records.push_back(std::move(event));
    }
    return result;
}

nlohmann::json encode(
    const data_log::LogAppendResult& value) {
    return {
        {"disposition", enumValue(value.disposition)},
        {"batch_id", value.batch_id},
        {"accepted_first_sequence",
         value.accepted_first_sequence},
        {"accepted_last_sequence", value.accepted_last_sequence},
        {"accepted_count", value.accepted_count},
        {"rejected_count", value.rejected_count},
        {"achieved_durability",
         enumValue(value.achieved_durability)},
        {"durability_ack_id",
         optionalValue(value.durability_ack_id)},
        {"retry_after_ms", value.retry_after_ms},
        {"policy_version", value.policy_version}};
}

data_log::LogAppendResult decodeLogAppendResult(
    const nlohmann::json& value) {
    data_log::LogAppendResult result;
    result.disposition =
        closedEnum<data_log::AppendDisposition>(
            value.at("disposition"), 0, 4);
    result.batch_id = value.at("batch_id").get<std::string>();
    result.accepted_first_sequence =
        value.at("accepted_first_sequence").get<std::uint64_t>();
    result.accepted_last_sequence =
        value.at("accepted_last_sequence").get<std::uint64_t>();
    result.accepted_count =
        value.at("accepted_count").get<std::uint32_t>();
    result.rejected_count =
        value.at("rejected_count").get<std::uint32_t>();
    result.achieved_durability =
        closedEnum<data_log::DurabilityClass>(
            value.at("achieved_durability"), 0, 4);
    result.durability_ack_id =
        decodeOptional<std::string>(
            value.at("durability_ack_id"));
    result.retry_after_ms =
        value.at("retry_after_ms").get<std::uint64_t>();
    result.policy_version =
        value.at("policy_version").get<std::string>();
    return result;
}

nlohmann::json encode(
    const data_log::AuditBatch& value) {
    auto records = nlohmann::json::array();
    for (const auto& item : value.records) {
        records.push_back({
            {"audit_id", item.audit_id},
            {"schema_version", item.schema_version},
            {"audit_type", item.audit_type},
            {"context", encodeObservation(item.context)},
            {"actor_id_hash", item.actor_id_hash},
            {"actor_role", item.actor_role},
            {"delegated_by_hash",
             optionalValue(item.delegated_by_hash)},
            {"subject_id_hash", item.subject_id_hash},
            {"action", item.action},
            {"interface_name", item.interface_name},
            {"capability_id",
             optionalValue(item.capability_id)},
            {"object_refs", item.object_refs},
            {"object_versions", item.object_versions},
            {"decision", item.decision},
            {"policy_id", item.policy_id},
            {"policy_version", item.policy_version},
            {"evidence_hashes", item.evidence_hashes},
            {"before_fact_summary",
             item.before_fact_summary},
            {"after_fact_summary", item.after_fact_summary},
            {"side_effect_state",
             enumValue(item.side_effect_state)},
            {"privacy_labels", item.privacy_labels},
            {"redaction_policy_version",
             item.redaction_policy_version},
            {"retention_class", item.retention_class},
            {"legal_hold_id",
             optionalValue(item.legal_hold_id)},
            {"occurred_at_utc_ms", item.occurred_at_utc_ms},
            {"occurred_at_mono_ns",
             item.occurred_at_mono_ns},
            {"requested_durability",
             enumValue(item.requested_durability)}});
    }
    return {
        {"batch_id", value.batch_id},
        {"producer_endpoint_id", value.producer_endpoint_id},
        {"producer_epoch", value.producer_epoch},
        {"first_sequence", value.first_sequence},
        {"last_sequence", value.last_sequence},
        {"checksum", value.checksum},
        {"redaction_proof", value.redaction_proof},
        {"records", std::move(records)}};
}

data_log::AuditBatch decodeAuditBatch(
    const nlohmann::json& value) {
    data_log::AuditBatch result;
    result.batch_id = value.at("batch_id").get<std::string>();
    result.producer_endpoint_id =
        value.at("producer_endpoint_id").get<std::string>();
    result.producer_epoch =
        value.at("producer_epoch").get<std::uint64_t>();
    result.first_sequence =
        value.at("first_sequence").get<std::uint64_t>();
    result.last_sequence =
        value.at("last_sequence").get<std::uint64_t>();
    result.checksum = value.at("checksum").get<std::string>();
    result.redaction_proof =
        value.at("redaction_proof").get<std::string>();
    for (const auto& encoded : value.at("records")) {
        data_log::AuditRecord item;
        item.audit_id =
            encoded.at("audit_id").get<std::string>();
        item.schema_version =
            encoded.at("schema_version").get<std::uint32_t>();
        item.audit_type =
            encoded.at("audit_type").get<std::string>();
        item.context =
            decodeObservation(encoded.at("context"));
        item.actor_id_hash =
            encoded.at("actor_id_hash").get<std::string>();
        item.actor_role =
            encoded.at("actor_role").get<std::string>();
        item.delegated_by_hash =
            decodeOptional<std::string>(
                encoded.at("delegated_by_hash"));
        item.subject_id_hash =
            encoded.at("subject_id_hash").get<std::string>();
        item.action =
            encoded.at("action").get<std::string>();
        item.interface_name =
            encoded.at("interface_name").get<std::string>();
        item.capability_id =
            decodeOptional<std::string>(
                encoded.at("capability_id"));
        item.object_refs =
            encoded.at("object_refs")
                .get<std::vector<std::string>>();
        item.object_versions =
            encoded.at("object_versions")
                .get<std::vector<std::string>>();
        item.decision =
            encoded.at("decision").get<std::string>();
        item.policy_id =
            encoded.at("policy_id").get<std::string>();
        item.policy_version =
            encoded.at("policy_version").get<std::string>();
        item.evidence_hashes =
            encoded.at("evidence_hashes")
                .get<std::vector<std::string>>();
        item.before_fact_summary =
            encoded.at("before_fact_summary").get<std::string>();
        item.after_fact_summary =
            encoded.at("after_fact_summary").get<std::string>();
        item.side_effect_state =
            closedEnum<SideEffectState>(
                encoded.at("side_effect_state"), 0, 5);
        item.privacy_labels =
            encoded.at("privacy_labels")
                .get<std::vector<std::string>>();
        item.redaction_policy_version =
            encoded.at("redaction_policy_version")
                .get<std::string>();
        item.retention_class =
            encoded.at("retention_class").get<std::string>();
        item.legal_hold_id =
            decodeOptional<std::string>(
                encoded.at("legal_hold_id"));
        item.occurred_at_utc_ms =
            encoded.at("occurred_at_utc_ms")
                .get<std::int64_t>();
        item.occurred_at_mono_ns =
            encoded.at("occurred_at_mono_ns")
                .get<std::int64_t>();
        item.requested_durability =
            closedEnum<data_log::DurabilityClass>(
                encoded.at("requested_durability"), 0, 4);
        result.records.push_back(std::move(item));
    }
    return result;
}

nlohmann::json encode(
    const data_log::AuditAppendResult& value) {
    return {
        {"disposition", enumValue(value.disposition)},
        {"batch_id", value.batch_id},
        {"accepted_count", value.accepted_count},
        {"achieved_durability",
         enumValue(value.achieved_durability)},
        {"durability_ack_id", value.durability_ack_id},
        {"hash_chain_head", value.hash_chain_head},
        {"key_generation", value.key_generation}};
}

data_log::AuditAppendResult decodeAuditAppendResult(
    const nlohmann::json& value) {
    data_log::AuditAppendResult result;
    result.disposition =
        closedEnum<data_log::AppendDisposition>(
            value.at("disposition"), 0, 4);
    result.batch_id = value.at("batch_id").get<std::string>();
    result.accepted_count =
        value.at("accepted_count").get<std::uint32_t>();
    result.achieved_durability =
        closedEnum<data_log::DurabilityClass>(
            value.at("achieved_durability"), 0, 4);
    result.durability_ack_id =
        value.at("durability_ack_id").get<std::string>();
    result.hash_chain_head =
        value.at("hash_chain_head").get<std::string>();
    result.key_generation =
        value.at("key_generation").get<std::string>();
    return result;
}

nlohmann::json encode(const data_log::TraceQuery& value) {
    return {{"trace_id", optionalValue(value.trace_id)},
            {"request_id", optionalValue(value.request_id)},
            {"plan_id", optionalValue(value.plan_id)},
            {"execution_id", optionalValue(value.execution_id)},
            {"max_records", value.max_records}};
}

data_log::TraceQuery decodeTraceQuery(
    const nlohmann::json& value) {
    data_log::TraceQuery result;
    result.trace_id =
        decodeOptional<std::string>(value.at("trace_id"));
    result.request_id =
        decodeOptional<std::string>(value.at("request_id"));
    result.plan_id =
        decodeOptional<std::string>(value.at("plan_id"));
    result.execution_id =
        decodeOptional<std::string>(value.at("execution_id"));
    result.max_records =
        value.at("max_records").get<std::size_t>();
    return result;
}

nlohmann::json encode(const data_log::TracePage& value) {
    data_log::LogEventBatch batch;
    batch.records = value.events;
    return {
        {"events", encode(batch).at("records")},
        {"complete_for_requested_range",
         value.complete_for_requested_range}};
}

data_log::TracePage decodeTracePage(
    const nlohmann::json& value) {
    nlohmann::json batch{
        {"batch_id", "trace-codec"},
        {"producer_endpoint_id", "trace-codec"},
        {"producer_epoch", 1},
        {"first_sequence", 0},
        {"last_sequence", 0},
        {"checksum", "trace-codec"},
        {"redaction_proof", "trace-codec"},
        {"records", value.at("events")}};
    data_log::TracePage result;
    result.events = decodeLogEventBatch(batch).records;
    result.complete_for_requested_range =
        value.at("complete_for_requested_range").get<bool>();
    return result;
}

nlohmann::json encode(const data_log::LogHealth& value) {
    return {
        {"ready", value.ready},
        {"buffered_events", value.buffered_events},
        {"persisted_events", value.persisted_events},
        {"audit_records", value.audit_records},
        {"emergency_ring_records",
         value.emergency_ring_records},
        {"hash_chain_head", value.hash_chain_head},
        {"d4_ready", value.d4_ready},
        {"audit_integrity_degraded",
         value.audit_integrity_degraded},
        {"active_key_generation",
         value.active_key_generation},
        {"audit_anchor_generation",
         value.audit_anchor_generation}};
}

data_log::LogHealth decodeLogHealth(
    const nlohmann::json& value) {
    data_log::LogHealth result;
    result.ready = value.at("ready").get<bool>();
    result.buffered_events =
        value.at("buffered_events").get<std::size_t>();
    result.persisted_events =
        value.at("persisted_events").get<std::size_t>();
    result.audit_records =
        value.at("audit_records").get<std::size_t>();
    result.emergency_ring_records =
        value.at("emergency_ring_records").get<std::size_t>();
    result.hash_chain_head =
        value.at("hash_chain_head").get<std::string>();
    result.d4_ready = value.at("d4_ready").get<bool>();
    result.audit_integrity_degraded =
        value.at("audit_integrity_degraded").get<bool>();
    result.active_key_generation =
        value.at("active_key_generation").get<std::string>();
    result.audit_anchor_generation =
        value.at("audit_anchor_generation").get<std::uint64_t>();
    return result;
}

namespace {

nlohmann::json encodeOccurrence(
    const exception::ExceptionOccurrence& value) {
    return {
        {"occurrence_id", value.occurrence_id},
        {"schema_version", value.schema_version},
        {"producer_endpoint_id", value.producer_endpoint_id},
        {"producer_epoch", value.producer_epoch},
        {"producer_sequence", value.producer_sequence},
        {"domain", value.domain},
        {"code", value.code},
        {"reported_severity", enumValue(value.reported_severity)},
        {"impact", enumValue(value.impact)},
        {"source_module", value.source_module},
        {"source_interface", value.source_interface},
        {"operation", value.operation},
        {"context", encodeObservation(value.context)},
        {"object_ref", optionalValue(value.object_ref)},
        {"capability_id", optionalValue(value.capability_id)},
        {"side_effect_state",
         value.side_effect_state
             ? nlohmann::json(
                   enumValue(*value.side_effect_state))
             : nlohmann::json(nullptr)},
        {"recoverable_hint", value.recoverable_hint},
        {"retryable_hint", value.retryable_hint},
        {"retry_scope_hint", value.retry_scope_hint},
        {"bounded_detail_code", value.bounded_detail_code},
        {"bounded_detail_summary",
         value.bounded_detail_summary},
        {"evidence_event_ids", value.evidence_event_ids},
        {"evidence_audit_ids", value.evidence_audit_ids},
        {"privacy_labels", value.privacy_labels},
        {"occurred_at_utc_ms", value.occurred_at_utc_ms},
        {"occurred_at_mono_ns", value.occurred_at_mono_ns},
        {"received_at_utc_ms", value.received_at_utc_ms},
        {"redaction_policy_version",
         value.redaction_policy_version},
        {"normalizer_version", value.normalizer_version}};
}

exception::ExceptionOccurrence decodeOccurrence(
    const nlohmann::json& value) {
    exception::ExceptionOccurrence result;
    result.occurrence_id =
        value.at("occurrence_id").get<std::string>();
    result.schema_version =
        value.at("schema_version").get<std::uint32_t>();
    result.producer_endpoint_id =
        value.at("producer_endpoint_id").get<std::string>();
    result.producer_epoch =
        value.at("producer_epoch").get<std::uint64_t>();
    result.producer_sequence =
        value.at("producer_sequence").get<std::uint64_t>();
    result.domain = value.at("domain").get<std::string>();
    result.code = value.at("code").get<std::string>();
    result.reported_severity =
        closedEnum<exception::ExceptionSeverity>(
            value.at("reported_severity"), 0, 3);
    result.impact = closedEnum<exception::ExceptionImpact>(
        value.at("impact"), 0, 3);
    result.source_module =
        value.at("source_module").get<std::string>();
    result.source_interface =
        value.at("source_interface").get<std::string>();
    result.operation = value.at("operation").get<std::string>();
    result.context = decodeObservation(value.at("context"));
    result.object_ref =
        decodeOptional<std::string>(value.at("object_ref"));
    result.capability_id =
        decodeOptional<std::string>(value.at("capability_id"));
    if (!value.at("side_effect_state").is_null()) {
        result.side_effect_state =
            closedEnum<SideEffectState>(
                value.at("side_effect_state"), 0, 5);
    }
    result.recoverable_hint =
        value.at("recoverable_hint").get<bool>();
    result.retryable_hint =
        value.at("retryable_hint").get<bool>();
    result.retry_scope_hint =
        value.at("retry_scope_hint").get<std::string>();
    result.bounded_detail_code =
        value.at("bounded_detail_code").get<std::string>();
    result.bounded_detail_summary =
        value.at("bounded_detail_summary").get<std::string>();
    result.evidence_event_ids =
        value.at("evidence_event_ids")
            .get<std::vector<std::string>>();
    result.evidence_audit_ids =
        value.at("evidence_audit_ids")
            .get<std::vector<std::string>>();
    result.privacy_labels =
        value.at("privacy_labels").get<std::vector<std::string>>();
    result.occurred_at_utc_ms =
        value.at("occurred_at_utc_ms").get<std::int64_t>();
    result.occurred_at_mono_ns =
        value.at("occurred_at_mono_ns").get<std::int64_t>();
    result.received_at_utc_ms =
        value.at("received_at_utc_ms").get<std::int64_t>();
    result.redaction_policy_version =
        value.at("redaction_policy_version").get<std::string>();
    result.normalizer_version =
        value.at("normalizer_version").get<std::string>();
    return result;
}

nlohmann::json encodeAccepted(
    const exception::ExceptionAccepted& value) {
    return {
        {"occurrence_id", value.occurrence_id},
        {"exception_id", value.exception_id},
        {"fingerprint", value.fingerprint},
        {"disposition", enumValue(value.disposition)},
        {"group_version", value.group_version},
        {"applied_severity", enumValue(value.applied_severity)},
        {"total_count", value.total_count},
        {"lifecycle", enumValue(value.lifecycle)},
        {"escalation", enumValue(value.escalation)},
        {"achieved_durability",
         enumValue(value.achieved_durability)},
        {"durability_ack_id", value.durability_ack_id},
        {"fingerprint_policy_version",
         value.fingerprint_policy_version}};
}

exception::ExceptionAccepted decodeAccepted(
    const nlohmann::json& value) {
    exception::ExceptionAccepted result;
    result.occurrence_id =
        value.at("occurrence_id").get<std::string>();
    result.exception_id =
        value.at("exception_id").get<std::string>();
    result.fingerprint =
        value.at("fingerprint").get<std::string>();
    result.disposition =
        closedEnum<exception::ExceptionDisposition>(
            value.at("disposition"), 0, 4);
    result.group_version =
        value.at("group_version").get<std::uint64_t>();
    result.applied_severity =
        closedEnum<exception::ExceptionSeverity>(
            value.at("applied_severity"), 0, 3);
    result.total_count =
        value.at("total_count").get<std::uint64_t>();
    result.lifecycle =
        closedEnum<exception::ExceptionLifecycle>(
            value.at("lifecycle"), 0, 4);
    result.escalation =
        closedEnum<exception::EscalationKind>(
            value.at("escalation"), 0, 2);
    result.achieved_durability =
        closedEnum<data_log::DurabilityClass>(
            value.at("achieved_durability"), 0, 4);
    result.durability_ack_id =
        value.at("durability_ack_id").get<std::string>();
    result.fingerprint_policy_version =
        value.at("fingerprint_policy_version").get<std::string>();
    return result;
}

}  // namespace

nlohmann::json encode(
    const exception::ExceptionReportRequest& value) {
    auto occurrences = nlohmann::json::array();
    for (const auto& item : value.occurrences) {
        occurrences.push_back(encodeOccurrence(item));
    }
    return {
        {"report_id", value.report_id},
        {"occurrences", std::move(occurrences)},
        {"batch_checksum", value.batch_checksum},
        {"source_redaction_proof",
         value.source_redaction_proof},
        {"requested_durability",
         enumValue(value.requested_durability)}};
}

exception::ExceptionReportRequest decodeExceptionReport(
    const nlohmann::json& value) {
    exception::ExceptionReportRequest result;
    result.report_id =
        value.at("report_id").get<std::string>();
    for (const auto& item : value.at("occurrences")) {
        result.occurrences.push_back(decodeOccurrence(item));
    }
    result.batch_checksum =
        value.at("batch_checksum").get<std::string>();
    result.source_redaction_proof =
        value.at("source_redaction_proof").get<std::string>();
    result.requested_durability =
        closedEnum<data_log::DurabilityClass>(
            value.at("requested_durability"), 0, 4);
    return result;
}

nlohmann::json encode(
    const exception::ExceptionReportResult& value) {
    auto results = nlohmann::json::array();
    for (const auto& item : value.results) {
        results.push_back(encodeAccepted(item));
    }
    return {{"report_id", value.report_id},
            {"results", std::move(results)},
            {"accepted_count", value.accepted_count},
            {"rejected_count", value.rejected_count},
            {"partial", value.partial}};
}

exception::ExceptionReportResult decodeExceptionReportResult(
    const nlohmann::json& value) {
    exception::ExceptionReportResult result;
    result.report_id =
        value.at("report_id").get<std::string>();
    for (const auto& item : value.at("results")) {
        result.results.push_back(decodeAccepted(item));
    }
    result.accepted_count =
        value.at("accepted_count").get<std::uint32_t>();
    result.rejected_count =
        value.at("rejected_count").get<std::uint32_t>();
    result.partial = value.at("partial").get<bool>();
    return result;
}

nlohmann::json encode(
    const exception::ExceptionGroup& value) {
    return {
        {"exception_id", value.exception_id},
        {"fingerprint", value.fingerprint},
        {"version", value.version},
        {"domain", value.domain},
        {"code", value.code},
        {"current_severity", enumValue(value.current_severity)},
        {"aggregate_impact", enumValue(value.aggregate_impact)},
        {"lifecycle", enumValue(value.lifecycle)},
        {"source_module", value.source_module},
        {"source_interface", value.source_interface},
        {"first_seen_at_utc_ms", value.first_seen_at_utc_ms},
        {"last_seen_at_utc_ms", value.last_seen_at_utc_ms},
        {"occurrence_count", value.occurrence_count},
        {"duplicate_replay_count",
         value.duplicate_replay_count},
        {"current_escalation",
         enumValue(value.current_escalation)},
        {"bounded_occurrence_ids",
         value.bounded_occurrence_ids}};
}

exception::ExceptionGroup decodeExceptionGroup(
    const nlohmann::json& value) {
    exception::ExceptionGroup result;
    result.exception_id =
        value.at("exception_id").get<std::string>();
    result.fingerprint =
        value.at("fingerprint").get<std::string>();
    result.version = value.at("version").get<std::uint64_t>();
    result.domain = value.at("domain").get<std::string>();
    result.code = value.at("code").get<std::string>();
    result.current_severity =
        closedEnum<exception::ExceptionSeverity>(
            value.at("current_severity"), 0, 3);
    result.aggregate_impact =
        closedEnum<exception::ExceptionImpact>(
            value.at("aggregate_impact"), 0, 3);
    result.lifecycle =
        closedEnum<exception::ExceptionLifecycle>(
            value.at("lifecycle"), 0, 4);
    result.source_module =
        value.at("source_module").get<std::string>();
    result.source_interface =
        value.at("source_interface").get<std::string>();
    result.first_seen_at_utc_ms =
        value.at("first_seen_at_utc_ms").get<std::int64_t>();
    result.last_seen_at_utc_ms =
        value.at("last_seen_at_utc_ms").get<std::int64_t>();
    result.occurrence_count =
        value.at("occurrence_count").get<std::uint64_t>();
    result.duplicate_replay_count =
        value.at("duplicate_replay_count").get<std::uint64_t>();
    result.current_escalation =
        closedEnum<exception::EscalationKind>(
            value.at("current_escalation"), 0, 2);
    result.bounded_occurrence_ids =
        value.at("bounded_occurrence_ids")
            .get<std::vector<std::string>>();
    return result;
}

nlohmann::json encode(
    const exception::ExceptionMutationRequest& value) {
    return {
        {"mutation_id", value.mutation_id},
        {"exception_id", value.exception_id},
        {"expected_group_version",
         value.expected_group_version},
        {"actor_id_hash", value.actor_id_hash},
        {"actor_role", value.actor_role},
        {"reason_code", value.reason_code},
        {"verification_evidence_refs",
         value.verification_evidence_refs},
        {"resolution_waiver_id",
         value.resolution_waiver_id}};
}

exception::ExceptionMutationRequest decodeExceptionMutation(
    const nlohmann::json& value) {
    exception::ExceptionMutationRequest result;
    result.mutation_id =
        value.at("mutation_id").get<std::string>();
    result.exception_id =
        value.at("exception_id").get<std::string>();
    result.expected_group_version =
        value.at("expected_group_version").get<std::uint64_t>();
    result.actor_id_hash =
        value.at("actor_id_hash").get<std::string>();
    result.actor_role =
        value.at("actor_role").get<std::string>();
    result.reason_code =
        value.at("reason_code").get<std::string>();
    result.verification_evidence_refs =
        value.at("verification_evidence_refs")
            .get<std::vector<std::string>>();
    result.resolution_waiver_id =
        value.at("resolution_waiver_id").get<std::string>();
    return result;
}

nlohmann::json encode(
    const exception::ExceptionMutationResult& value) {
    return {{"changed", value.changed},
            {"group", encode(value.group)}};
}

exception::ExceptionMutationResult
decodeExceptionMutationResult(const nlohmann::json& value) {
    exception::ExceptionMutationResult result;
    result.changed = value.at("changed").get<bool>();
    result.group = decodeExceptionGroup(value.at("group"));
    return result;
}

}  // namespace master_agent::ipc::wire
