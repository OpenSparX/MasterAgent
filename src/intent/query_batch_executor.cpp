/**
 * @file query_batch_executor.cpp
 * @brief Routes read-only intent queries to the four documented module owners.
 *
 * Design trace: Intent Recognition Engine Detailed Design, section 2.6.2. No query is
 * answered by an independent simulator: preprocessing, Memory, Skill, and
 * Atomic Service remain the only evidence sources.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

namespace master_agent::intent {
namespace {

QueryResultRecord failedRecord(
    const QuerySpec& query, const std::string& source,
    const std::string& code, std::int64_t observed_at) {
    QueryResultRecord record;
    record.query_id = query.query_id;
    record.capability = query.capability;
    record.normalized_arguments = query.arguments;
    record.status =
        code.find("PERMISSION") != std::string::npos
            ? "PERMISSION_DENIED"
            : (code.find("TIMEOUT") != std::string::npos ||
                       code.find("EXPIRED") != std::string::npos
                   ? "TIMEOUT"
                   : "ERROR");
    record.source = source;
    record.observed_at_utc_ms = observed_at;
    record.error_code = code;
    return record;
}

nlohmann::json toolDefinitionJson(
    const atomic_service::McpToolDefinition& tool) {
    nlohmann::json annotations{
        {"readOnlyHint", tool.annotations.read_only_hint},
        {"destructiveHint", tool.annotations.destructive_hint},
        {"idempotentHint", tool.annotations.idempotent_hint},
        {"openWorldHint", tool.annotations.open_world_hint}};
    if (!tool.annotations.title.empty()) {
        annotations["title"] = tool.annotations.title;
    }
    nlohmann::json definition{
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", tool.input_schema},
        {"annotations", std::move(annotations)}};
    if (!tool.title.empty()) definition["title"] = tool.title;
    if (!tool.output_schema.empty()) {
        definition["outputSchema"] = tool.output_schema;
    }
    return definition;
}

class ModuleIntentQueryExecutor final
    : public IIntentQueryExecutor {
public:
    ModuleIntentQueryExecutor(
        std::shared_ptr<preprocess::IStateQuery> preprocess,
        std::shared_ptr<memory::IMemoryService> memory,
        std::shared_ptr<
            intent_support::IIntentSkillResolver> skill,
        std::shared_ptr<
            atomic_service::IAtomicServiceManager> atomic,
        std::shared_ptr<IRuntimeClock> clock)
        : preprocess_(std::move(preprocess)),
          memory_(std::move(memory)),
          skill_(std::move(skill)),
          atomic_(std::move(atomic)),
          clock_(std::move(clock)) {}

    QueryResultRecord execute(
        const QuerySpec& query,
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call) const override {
        const auto observed =
            clock_ ? clock_->utcNowMs() : 0;
        if (!clock_ || !query.read_only ||
            query.query_id.empty() ||
            query.capability.empty() ||
            !query.arguments.is_object() ||
            query.deadline_mono_ns <= 0 ||
            deadlineExpired(
                query.deadline_mono_ns, *clock_) ||
            !hasHostModuleIdentity(
                call,
                CallerModuleId::IntentRecognitionEngine)) {
            return failedRecord(
                query, "IntentRecognitionEngine",
                "INTENT_QUERY_INVALID", observed);
        }

        auto query_call = call;
        query_call.deadline_mono_ns =
            query.deadline_mono_ns;
        if (query.capability ==
                "preprocess.queryRuntimeState" ||
            query.capability ==
                "preprocess.getVehicleState") {
            if (!preprocess_) {
                return failedRecord(
                    query, "PreprocessingEngine",
                    "PREPROCESS_QUERY_NOT_CONFIGURED",
                    observed);
            }
            preprocess::StateQuery state;
            state.request_id = request.request_id;
            state.session_id = request.session_id;
            state.turn_id = request.turn_id;
            const auto domain = query.arguments.value(
                "state_type", std::string{"VEHICLE"});
            state.state_type =
                domain == "ENVIRONMENT"
                    ? preprocess::StateDomain::Environment
                    : preprocess::StateDomain::Vehicle;
            if (query.arguments.contains("fields") &&
                query.arguments.at("fields").is_array()) {
                state.fields =
                    query.arguments.at("fields")
                        .get<std::vector<std::string>>();
            }
            const auto result =
                preprocess_->queryRuntimeState(
                    state, query_call);
            if (!result.status.ok || !result.value) {
                return failedRecord(
                    query, "PreprocessingEngine",
                    result.status.error.code, observed);
            }
            QueryResultRecord record;
            record.query_id = query.query_id;
            record.capability = query.capability;
            record.normalized_arguments =
                query.arguments;
            record.status =
                result.value->values.empty()
                    ? "EMPTY"
                    : "OK";
            record.result = {
                {"values", result.value->values},
                {"missing_fields",
                 result.value->missing_fields},
                {"timestamp_utc_ms",
                 result.value->timestamp_utc_ms}};
            record.source = "PreprocessingEngine";
            record.source_version = 1;
            record.observed_at_utc_ms =
                result.value->timestamp_utc_ms;
            return record;
        }

        if (query.capability == "memory.getContext" ||
            query.capability == "memory.recallMemory") {
            if (!memory_) {
                return failedRecord(
                    query, "MemoryService",
                    "MEMORY_QUERY_NOT_CONFIGURED",
                    observed);
            }
            const auto result = memory_->getContext(
                request,
                context.preprocess_result
                    .normalized_request.text,
                query_call);
            if (!result.status.ok || !result.value) {
                return failedRecord(
                    query, "MemoryService",
                    result.status.error.code, observed);
            }
            QueryResultRecord record;
            record.query_id = query.query_id;
            record.capability = query.capability;
            record.normalized_arguments =
                query.arguments;
            record.status =
                result.value->flattened_context.empty()
                    ? "EMPTY"
                    : "OK";
            record.result = {
                {"flattened_context",
                 result.value->flattened_context},
                {"block_count",
                 result.value->blocks.size()}};
            record.source = "MemoryService";
            record.source_version =
                context.context_version;
            record.observed_at_utc_ms = observed;
            return record;
        }

        if (query.capability ==
            "skill.getToolDefinitions") {
            if (!skill_) {
                return failedRecord(
                    query, "SkillEngine",
                    "SKILL_QUERY_NOT_CONFIGURED",
                    observed);
            }
            const auto tools =
                skill_->listAvailableTools(query_call);
            if (!tools.status.ok || !tools.value) {
                return failedRecord(
                    query, "SkillEngine",
                    tools.status.error.code, observed);
            }
            nlohmann::json definitions =
                nlohmann::json::array();
            for (const auto& tool : *tools.value) {
                definitions.push_back(
                    toolDefinitionJson(tool));
            }
            QueryResultRecord record;
            record.query_id = query.query_id;
            record.capability = query.capability;
            record.normalized_arguments =
                query.arguments;
            record.status =
                definitions.empty() ? "EMPTY" : "OK";
            record.result =
                {{"tools", definitions}};
            record.source = "SkillEngine";
            record.source_version = 1;
            record.observed_at_utc_ms = observed;
            return record;
        }

        if (query.capability == "skill.matchSkill") {
            if (!skill_) {
                return failedRecord(
                    query, "SkillEngine",
                    "SKILL_QUERY_NOT_CONFIGURED",
                    observed);
            }
            const auto matches =
                skill_->querySkillCandidates(
                    query.arguments.value(
                        "query",
                        context.preprocess_result
                            .normalized_request.text),
                    query.arguments.value("top_k", 3U),
                    query_call);
            if (!matches.status.ok || !matches.value) {
                return failedRecord(
                    query, "SkillEngine",
                    matches.status.error.code, observed);
            }
            QueryResultRecord record;
            record.query_id = query.query_id;
            record.capability = query.capability;
            record.normalized_arguments =
                query.arguments;
            record.status =
                matches.value->value(
                    "matches",
                    nlohmann::json::array())
                        .empty()
                    ? "EMPTY"
                    : "OK";
            record.result = *matches.value;
            record.source = "SkillEngine";
            record.source_version = 1;
            record.observed_at_utc_ms = observed;
            return record;
        }

        if (query.capability == "atomic.queryReadOnly") {
            if (!atomic_) {
                return failedRecord(
                    query, "AtomicServiceManager",
                    "ATOMIC_QUERY_NOT_CONFIGURED",
                    observed);
            }
            atomic_service::AtomicReadOnlyMcpRequest read_only;
            read_only.mcp_request.jsonrpc = "2.0";
            read_only.mcp_request.id = query.query_id;
            read_only.mcp_request.method = "tools/call";
            read_only.mcp_request.name = query.arguments.value(
                "tool_name", std::string{});
            read_only.mcp_request.arguments = query.arguments.value(
                "arguments", nlohmann::json::object());
            read_only.expected_catalog_digest =
                context.expected_capability_digest;
            const auto result = atomic_->queryReadOnly(
                read_only,
                makeChildCallContext(
                    query_call,
                    CallerModuleId::IntentRecognitionEngine));
            if (!result.status.ok || !result.value) {
                return failedRecord(
                    query, "AtomicServiceManager",
                    result.status.error.code, observed);
            }
            QueryResultRecord record;
            record.query_id = query.query_id;
            record.capability = query.capability;
            record.normalized_arguments =
                query.arguments;
            record.status =
                result.value->result.structured_content.empty()
                    ? "EMPTY"
                    : "OK";
            record.result = {
                {"structured_content",
                 result.value->result.structured_content},
                {"text_content",
                 result.value->result.text_content},
                {"catalog_snapshot_id",
                 result.value->catalog_snapshot_id},
                {"tool_digest", result.value->tool_digest},
                {"policy_digest", result.value->policy_digest}};
            record.source = "AtomicServiceManager";
            record.source_version =
                result.value->provider_epoch;
            record.observed_at_utc_ms =
                result.value->observed_at_utc_ms;
            return record;
        }

        return failedRecord(
            query, "IntentRecognitionEngine",
            "INTENT_QUERY_CAPABILITY_NOT_ALLOWED",
            observed);
    }

private:
    std::shared_ptr<preprocess::IStateQuery> preprocess_;
    std::shared_ptr<memory::IMemoryService> memory_;
    std::shared_ptr<
        intent_support::IIntentSkillResolver> skill_;
    std::shared_ptr<
        atomic_service::IAtomicServiceManager> atomic_;
    std::shared_ptr<IRuntimeClock> clock_;
};

}  // namespace

std::shared_ptr<IIntentQueryExecutor>
createModuleIntentQueryExecutor(
    std::shared_ptr<preprocess::IStateQuery> preprocess,
    std::shared_ptr<memory::IMemoryService> memory,
    std::shared_ptr<intent_support::IIntentSkillResolver> skill,
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<IRuntimeClock> clock) {
    return std::make_shared<ModuleIntentQueryExecutor>(
        std::move(preprocess), std::move(memory),
        std::move(skill), std::move(atomic),
        std::move(clock));
}

}  // namespace master_agent::intent
