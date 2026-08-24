/**
 * @file turn_pipeline.cpp
 * @brief Executes the end-to-end interaction, intent, plan, and response pipeline.
 */

#include "include/plan_response.h"
#include "include/capability_policy.h"
#include "include/client_error_sanitization.h"

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

namespace master_agent::agent_service {

TurnResult AgentService::runTurnImpl(
    const interaction::StandardRequest& request,
    const CallContext& call) {

    if (!clock_ || !ids_ || !preprocess_ || !memory_ || !intent_ ||
        !orchestrator_ || !atomic_ || !log_ || !exceptions_) {
        TurnResult unavailable;
        unavailable.request_id = request.request_id;
        unavailable.trace_id = request.trace_id;
        unavailable.session_id = request.session_id;
        unavailable.turn_id = request.turn_id;
        unavailable.reply = u8"系统尚未完成初始化。";
        unavailable.error_code =
            "AGENT_SERVICE_CONFIGURATION_INVALID";
        unavailable.error_message =
            "all required module dependencies must be configured";
        return unavailable;
    }
    if (!hasHostModuleIdentity(
            call, CallerModuleId::InteractionIngress) ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash.empty() ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns ||
        request.priority != TaskPriority::P1 ||
        request.deadline_mono_ns <= 0 ||
        deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return failureResult(
            request,
            Status::Error("agent_service",
                          "AGENT_SERVICE_CALLER_OR_IDENTITY_INVALID",
                          "runTurn requires matching InteractionIngress identity"),
            "agent_service", "runTurn");
    }
    const auto original_deadline_expired =
        [this, &request]() {
            return !clock_ ||
                   deadlineExpired(
                       request.deadline_mono_ns, *clock_);
        };
    const auto deadline_failure =
        [this, &request](
            const std::string& source_module,
            const std::string& operation) {
            return failureResult(
                request,
                Status::Error(
                    "agent_service",
                    "AGENT_SERVICE_RESULT_AFTER_DEADLINE",
                    "module result arrived after the request deadline",
                    false, SideEffectState::NotApplicable),
                source_module, operation);
        };
    logEvent(request, "TURN_ACCEPTED", "runTurn", "accepted",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D2Journaled, {}, {},
             nlohmann::json{{"stage", "ingress"},
                            {"status", "SUCCEEDED"},
                            {"input", {{"type", "text"},
                                       {"length", request.text.size()},
                                       {"digest", secureDigest(request.text)}}},
                            {"privacy", "safe"}}.dump());
    logEvent(request, "CONFIG_SNAPSHOT_BOUND", "bindConfig", "success",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D2Journaled, {}, {},
             nlohmann::json{{"stage", "config"},
                            {"status", "SUCCEEDED"},
                            {"output", {{"config_snapshot_id", config_snapshot_id_}}}}.dump());
    debugEvent(request, "REQUEST_RECEIVED", "ingress",
               nlohmann::json{{"received", {{"text", request.text},
                                             {"params", request.params},
                                             {"session_id", request.session_id},
                                             {"turn_id", request.turn_id}}},
                              {"operation", "接收并分配请求标识"},
                              {"produced", {{"request_id", request.request_id},
                                             {"trace_id", request.trace_id}}}}.dump());
    debugEvent(request, "CONFIG_BOUND", "config",
               nlohmann::json{{"received", {{"config_snapshot_id", config_snapshot_id_}}},
                              {"operation", "为本次请求冻结配置快照"},
                              {"produced", {{"config_snapshot_id", config_snapshot_id_}}}}.dump());
    if (original_deadline_expired()) {
        return deadline_failure("data_log", "TURN_ACCEPTED");
    }
    CallContext internal{
        CallerModuleId::AgentService, request.request_id,
        request.trace_id, call.principal_id_hash, request.priority,
        request.deadline_mono_ns, {}, 0, call.authorization_ref};

    const auto preprocessed = preprocess_->process(request, internal);
    if (original_deadline_expired()) {
        return deadline_failure("preprocess", "process");
    }
    if (!preprocessed.status.ok) {
        return failureResult(
            request, preprocessed.status, "preprocess", "process");
    }
    if (!preprocessed.value) {
        return failureResult(
            request,
            Status::Error(
                "preprocess",
                "PREPROCESS_SUCCESS_WITHOUT_VALUE",
                "preprocess returned success without a result"),
            "preprocess", "process");
    }
    if (!preprocessed.value->valid) {
        return failureResult(
            request,
            Status::Error(
                "preprocess", "PREPROCESS_INVALID",
                preprocessed.value->error_message),
            "preprocess", "process");
    }
    logEvent(request, "PREPROCESS_COMPLETED", "process", "success",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D1Buffered, {}, {},
             nlohmann::json{{"stage", "preprocess"},
                            {"status", "SUCCEEDED"},
                            {"output", {{"type", "normalized_text"},
                                        {"length", preprocessed.value->normalized_request.text.size()},
                                        {"digest", secureDigest(preprocessed.value->normalized_request.text)}}}}.dump());
    debugEvent(request, "PREPROCESS_DETAIL", "preprocess",
               nlohmann::json{{"received", {{"original_text", request.text},
                                             {"params", request.params}}},
                              {"operation", "校验 UTF-8、清洗并标准化输入"},
                              {"produced", {{"normalized_text", preprocessed.value->normalized_request.text},
                                             {"normalized_params", preprocessed.value->normalized_request.params},
                                             {"valid", preprocessed.value->valid}}}}.dump());
    if (original_deadline_expired()) {
        return deadline_failure(
            "data_log", "PREPROCESS_COMPLETED");
    }

    const auto initial_memory_status = memory_->writeTurn(
        {request, preprocessed.value->normalized_request.text, "",
         "cockpit", 1},
        internal);
    if (original_deadline_expired()) {
        return deadline_failure("memory", "writeTurn");
    }
    if (!initial_memory_status.ok) {
        logEvent(request, "MEMORY_WRITE_DEGRADED", "writeTurn",
                 "degraded", data_log::EventSeverity::Warning,
                 data_log::DurabilityClass::D2Journaled, {},
                 initial_memory_status.error.code);
        reportFailure(request, initial_memory_status.error,
                      "memory", "writeTurn");
    }

    memory::MemoryContext memory_context;
    const auto recalled = memory_->getContext(
        preprocessed.value->normalized_request,
        preprocessed.value->normalized_request.text, internal);
    if (original_deadline_expired()) {
        return deadline_failure("memory", "getContext");
    }
    if (recalled.status.ok && recalled.value) {
        memory_context = *recalled.value;
        logEvent(request, "MEMORY_CONTEXT_RECALLED", "getContext", "success",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D1Buffered, {}, {},
                 nlohmann::json{{"stage", "memory"},
                                {"status", "SUCCEEDED"},
                                {"output", {{"context_blocks", memory_context.blocks.size()}}}}.dump());
        nlohmann::json memory_blocks = nlohmann::json::array();
        for (const auto& block : memory_context.blocks) {
            memory_blocks.push_back({{"type", block.memory_type},
                                     {"content", block.content},
                                     {"source", block.source_memory_id},
                                     {"relevance", block.relevance_score},
                                     {"turn_id", block.turn_id}});
        }
        debugEvent(request, "MEMORY_RECALL_DETAIL", "memory",
                   nlohmann::json{{"received", {{"query", preprocessed.value->normalized_request.text}}},
                                  {"operation", "按当前会话召回最近的短期记忆"},
                                  {"produced", {{"blocks", std::move(memory_blocks)},
                                                 {"flattened_context", memory_context.flattened_context}}}}.dump());
    } else {
        const auto memory_error =
            recalled.status.ok
                ? Status::Error(
                      "memory",
                      "MEMORY_SUCCESS_WITHOUT_VALUE",
                      "memory returned success without context")
                : recalled.status;
        logEvent(request, "MEMORY_CONTEXT_DEGRADED", "getContext", "degraded",
                 data_log::EventSeverity::Warning,
                 data_log::DurabilityClass::D2Journaled, {},
                 memory_error.error.code);
        reportFailure(request, memory_error.error, "memory", "getContext");
    }

    const auto catalog = atomic_->getToolCatalogSnapshot(internal);
    if (original_deadline_expired()) {
        return deadline_failure(
            "atomic_service", "getToolCatalogSnapshot");
    }
    if (!catalog.status.ok) {
        return failureResult(request, catalog.status, "atomic_service",
                             "getToolCatalogSnapshot");
    }
    if (!catalog.value) {
        return failureResult(
            request,
            Status::Error(
                "atomic_service",
                "ATOMIC_CATALOG_SUCCESS_WITHOUT_VALUE",
                "atomic catalog returned success without a snapshot"),
            "atomic_service", "getToolCatalogSnapshot");
    }
    nlohmann::json tool_catalog = nlohmann::json::array();
    const auto debug_catalog_call =
        makeChildCallContext(internal, CallerModuleId::PromptEngine);
    const auto listed_debug_tools = atomic_->listTools(debug_catalog_call);
    if (listed_debug_tools.status.ok && listed_debug_tools.value) {
        for (const auto& tool : *listed_debug_tools.value) {
            tool_catalog.push_back({{"name", tool.name},
                                    {"title", tool.title},
                                    {"description", tool.description},
                                    {"input_schema", tool.input_schema},
                                    {"output_schema", tool.output_schema}});
        }
    }
    debugEvent(request, "CAPABILITY_RECALL_DETAIL", "retrieval",
               nlohmann::json{{"received", {{"normalized_text", preprocessed.value->normalized_request.text}}},
                              {"operation", "读取本次请求可用的 Tool 能力目录"},
                              {"produced", {{"tools", std::move(tool_catalog)},
                                             {"catalog_digest", catalog.value->catalog_digest}}}}.dump());
    intent::IntentContext intent_context;
    intent_context.preprocess_result = *preprocessed.value;
    intent_context.memory_context = std::move(memory_context);
    intent_context.session_id = request.session_id;
    intent_context.turn_id = request.turn_id;
    intent_context.context_version = request.turn_id;
    intent_context.priority = request.priority;
    intent_context.deadline_mono_ns = request.deadline_mono_ns;
    intent_context.expected_capability_digest =
        catalog.value->catalog_digest;
    const auto intent_acceptance = intent_->submit(
        preprocessed.value->normalized_request, intent_context,
        "intent-job|" + request.request_id + "|" +
            std::to_string(request.turn_id),
        internal);
    logEvent(request, "INTENT_SUBMITTED", "submit", "accepted",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D1Buffered, {}, {},
             nlohmann::json{{"stage", "local_inference"},
                            {"status", "STARTED"},
                            {"input", {{"capability_catalog_digest", catalog.value->catalog_digest}}}}.dump());
    if (!intent_acceptance.accepted) {
        return failureResult(
            request,
            Status::Error(
                "intent",
                intent_acceptance.reject_code.empty()
                    ? "INTENT_SUBMIT_REJECTED"
                    : intent_acceptance.reject_code,
                "intent engine rejected asynchronous admission"),
            "intent", "submit");
    }
    auto intent_result =
        Result<intent::IntentOrchestrationResult>::Failure(
            Status::Error(
                "intent", "INTENT_RESULT_NOT_READY",
                "intent job has not reached a terminal state", true));
    // The public Agent Service contract is synchronous, while Intent is an
    // asynchronous job owner. Polling is only the local single-process
    // adapter; a process-separated deployment maps this loop to the reliable
    // completion callback/outbox and retains getResult for recovery.
    while (!original_deadline_expired()) {
        if (original_deadline_expired()) break;
        const auto job = intent_->getResult(
            intent_acceptance.job_id, internal);
        if (!job.status.ok || !job.value) {
            intent_result =
                Result<intent::IntentOrchestrationResult>::Failure(
                    job.status.ok
                        ? Status::Error(
                              "intent",
                              "INTENT_SUCCESS_WITHOUT_JOB",
                              "intent query returned no job snapshot")
                        : job.status);
            break;
        }
        if (job.value->state == intent::IntentJobState::Completed &&
            job.value->result) {
            intent_result =
                Result<intent::IntentOrchestrationResult>::Success(
                    *job.value->result);
            break;
        }
        if (job.value->state == intent::IntentJobState::Failed) {
            intent_result =
                Result<intent::IntentOrchestrationResult>::Failure(
                    Status::Error(
                        "intent",
                        job.value->error_code.empty()
                            ? "INTENT_JOB_FAILED"
                            : job.value->error_code,
                        "intent job failed"));
            break;
        }
        if (job.value->state == intent::IntentJobState::Cancelled) {
            if (job.value->result) {
                intent_result =
                    Result<intent::IntentOrchestrationResult>::Success(
                        *job.value->result);
            } else {
                intent_result =
                    Result<intent::IntentOrchestrationResult>::Failure(
                        Status::Error(
                            "intent", "INTENT_JOB_CANCELLED",
                            "intent job was cancelled"));
            }
            break;
        }
        // Real runtimes can spend seconds in prefill/decode. A tight bounded
        // spin exhausted its observation count before the asynchronous Intent
        // worker could publish a terminal result. The request deadline is the
        // actual bound; a short sleep keeps the local synchronous adapter from
        // consuming a core while preserving prompt cancellation checks.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (original_deadline_expired()) {
        return deadline_failure("intent", "getResult");
    }
    if (!intent_result.status.ok) {
        return failureResult(request, intent_result.status, "intent",
                             "getResult");
    }
    if (!intent_result.value) {
        return failureResult(
            request,
            Status::Error(
                "intent", "INTENT_SUCCESS_WITHOUT_VALUE",
                "intent returned success without a decision"),
            "intent", "getResult");
    }

    TurnResult result;
    result.request_id = request.request_id;
    result.trace_id = request.trace_id;
    result.session_id = request.session_id;
    result.turn_id = request.turn_id;
    const auto& decision = *intent_result.value;
    const auto decision_type = [&decision]() {
        using intent::IntentOutcomeType;
        switch (decision.outcome_type) {
            case IntentOutcomeType::DirectReply: return "Reply";
            case IntentOutcomeType::Clarify: return "Ask";
            case IntentOutcomeType::DeterministicPlan: return "ExecutionPlan";
            case IntentOutcomeType::Failed: return "Fail";
            case IntentOutcomeType::Cancelled: return "Cancelled";
        }
        return "Unknown";
    }();
    const auto reason_prefix = [](const std::string& value,
                                  const std::string& prefix) {
        return value.rfind(prefix, 0) == 0;
    };
    const bool deterministic_route =
        reason_prefix(decision.reason_code, "RULE_") ||
        reason_prefix(decision.reason_code, "SKILL_") ||
        reason_prefix(decision.reason_code, "BGE_");
    logEvent(request, "RULE_ROUTE_RESOLVED", "resolve", "observed",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D1Buffered, {}, {},
             nlohmann::json{{"stage", "rule_match"},
                            {"status", deterministic_route ? "SUCCEEDED" : "SKIPPED"},
                            {"output", {{"reason_code", decision.reason_code}}}}.dump());
    logEvent(request, "RETRIEVAL_RESOLVED", "retrieve", "observed",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D1Buffered, {}, {},
             nlohmann::json{{"stage", "retrieval"},
                            {"status", deterministic_route ? "SUCCEEDED" : "SKIPPED"}}.dump());

    std::size_t local_inference_events = 0;
    if (inference_) {
        for (const auto& event : inference_->events()) {
            if (event.trace_id != request.trace_id) continue;
            ++local_inference_events;
            const bool failed = event.state == inference::InferenceJobState::Failed;
            const bool completed = event.state == inference::InferenceJobState::Completed;
            nlohmann::json output{{"job_id", event.job_id},
                                  {"model_stage", event.stage}};
            if (event.result) {
                output["model_id"] = event.result->model_id;
                output["runtime"] = event.result->runtime_backend;
                output["prompt_digest"] = event.result->prompt_digest;
                output["model_output_digest"] = event.result->output_digest;
                output["prompt_tokens"] = event.result->prompt_token_count;
                output["generated_tokens"] = event.result->generated_token_count;
                output["total_latency_ms"] = event.result->total_latency_ms;
            }
            logEvent(request, "LOCAL_INFERENCE_OBSERVED", "infer", event.event_type,
                     failed ? data_log::EventSeverity::Error : data_log::EventSeverity::Info,
                     data_log::DurabilityClass::D1Buffered, {},
                     event.last_error ? event.last_error->code : std::string{},
                     nlohmann::json{{"stage", "local_inference"},
                                    {"status", failed ? "FAILED" :
                                        (completed ? "SUCCEEDED" : "STARTED")},
                                    {"output", std::move(output)}}.dump());
        }
    }
    if (local_inference_events == 0) {
        logEvent(request, "LOCAL_INFERENCE_SKIPPED", "infer", "not_required",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D1Buffered, {}, {},
                 nlohmann::json{{"stage", "local_inference"},
                                {"status", "SKIPPED"}}.dump());
    }

    std::size_t cloud_events = 0;
    for (const auto& event : intent_->cloudEvents()) {
        if (event.trace_id != request.trace_id) continue;
        ++cloud_events;
        logEvent(request, event.event_type, "cloudFallback", event.outcome,
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D2Journaled, {}, {},
                 nlohmann::json{{"stage", event.event_type.find("ARBITRATION") != std::string::npos ||
                                                   event.event_type.find("ESCALATION") != std::string::npos
                                               ? "cloud_arbitration" : "cloud_inference"},
                                {"status", event.outcome},
                                {"output", {{"reason_code", event.reason_code},
                                            {"payload_digest", event.payload_digest},
                                            {"model_output_digest", event.output_digest},
                                            {"runtime", event.runtime_tag}}}}.dump());
    }
    if (cloud_events == 0) {
        logEvent(request, "CLOUD_ARBITRATION_SKIPPED", "cloudFallback", "not_required",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D1Buffered, {}, {},
                 nlohmann::json{{"stage", "cloud_arbitration"},
                                {"status", "SKIPPED"}}.dump());
        logEvent(request, "CLOUD_INFERENCE_SKIPPED", "cloudInfer", "not_required",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D1Buffered, {}, {},
                 nlohmann::json{{"stage", "cloud_inference"},
                                {"status", "SKIPPED"}}.dump());
    }
    logEvent(request, "INTENT_DECISION_VALIDATED", "getResult", "success",
             data_log::EventSeverity::Info,
             data_log::DurabilityClass::D2Journaled, {}, {},
             nlohmann::json{{"stage", "decision_validation"},
                            {"status", "SUCCEEDED"},
                            {"output", {{"decision_type", decision_type},
                                        {"reason_code", decision.reason_code},
                                        {"reply_digest", secureDigest(decision.user_reply)}}}}.dump());
    nlohmann::json decision_detail{{"outcome", decision_type},
                                   {"reason_code", decision.reason_code},
                                   {"user_reply", decision.user_reply}};
    if (decision.task_dag) {
        decision_detail["plan"] = nlohmann::json::array();
        for (const auto& node : decision.task_dag->nodes) {
            decision_detail["plan"].push_back({{"node_id", node.node_id},
                                                {"executor", node.executor},
                                                {"action", node.action},
                                                {"target_agent", node.target_agent},
                                                {"params", node.params}});
        }
    }
    debugEvent(request, "DECISION_DETAIL", "decision_validation",
               nlohmann::json{{"received", {{"intent_job_id", intent_acceptance.job_id}}},
                              {"operation", "解析模型或规则结果，并校验为确定性执行单元"},
                              {"produced", std::move(decision_detail)}}.dump());
    if (decision.outcome_type == intent::IntentOutcomeType::DirectReply ||
        decision.outcome_type == intent::IntentOutcomeType::Clarify) {
        result.reply = decision.user_reply;
        result.success = true;
        result.turn_summary =
            decision.outcome_type == intent::IntentOutcomeType::Clarify
                ? "clarification"
                : "direct_reply";
    } else if (decision.outcome_type ==
                   intent::IntentOutcomeType::Failed &&
               !decision.task_dag &&
               !decision.user_reply.empty()) {
        // A protocol-valid business FAIL is a deterministic Intent terminal,
        // not an Orchestrator job and not an infrastructure exception.
        // Preserve the bounded user-facing reply and let the normal
        // completion path journal the failed turn.
        result.reply = decision.user_reply;
        result.success = false;
        result.error_code = normalizedErrorCode(
            decision.reason_code.empty()
                ? "INTENT_MODEL_FAILED"
                : decision.reason_code);
        result.error_message = safeClientErrorMessage();
        result.turn_summary = "intent_failed";
    } else if (decision.outcome_type !=
                   intent::IntentOutcomeType::DeterministicPlan ||
               !decision.task_dag) {
        return failureResult(
            request,
            Status::Error("intent",
                          decision.reason_code.empty()
                              ? "INTENT_NO_EXECUTABLE_RESULT"
                              : decision.reason_code,
                          "intent did not produce an executable result"),
            "intent", "process");
    } else {
        orchestrator::AdmissionContext admission;
        admission.principal_id_hash = call.principal_id_hash;
        admission.source_type =
            "USER_INTERACTION";
        admission.granted_priority = request.priority;
        admission.p0_authorization = false;
        admission.policy_snapshot_id = "agent-service-admission-v1";
        admission.policy_digest =
            secureDigest(admission.policy_snapshot_id);
        admission.authorization_ref =
            "policy:" + admission.policy_digest;
        admission.granted_permissions.insert(
            "vehicle.climate.write");
        admission.deadline_mono_ns = request.deadline_mono_ns;
        for (const auto& node : decision.task_dag->nodes) {
            if (productionCapabilityAllowlist().count(node.action) == 0) {
                return failureResult(
                    request,
                    Status::Error(
                        "agent_service",
                        "AGENT_SERVICE_CAPABILITY_NOT_ALLOWED",
                        "intent plan requested a capability outside policy"),
                    "agent_service", "admitPlan");
            }
            admission.allowed_capabilities.insert(node.action);
            if (node.executor == "atomic_service" &&
                node.max_attempts > 1) {
                const auto idempotency =
                    catalog.value->idempotency_policies.find(
                        node.action);
                const auto retryable =
                    catalog.value->retryable_errors.find(
                        node.action);
                if (idempotency ==
                        catalog.value->idempotency_policies.end() ||
                    retryable ==
                        catalog.value->retryable_errors.end() ||
                    retryable->second.empty()) {
                    return failureResult(
                        request,
                        Status::Error(
                            "agent_service",
                            "AGENT_SERVICE_RETRY_POLICY_UNAVAILABLE",
                            "trusted capability retry policy is unavailable"),
                        "agent_service", "admitPlan");
                }
                orchestrator::CapabilityRetryPolicy retry_policy;
                retry_policy.idempotency_policy =
                    idempotency->second;
                retry_policy.retryable_errors.insert(
                    retryable->second.begin(),
                    retryable->second.end());
                retry_policy.base_backoff_ns = 1'000'000LL;
                retry_policy.max_backoff_ns = 100'000'000LL;
                admission.retry_policies[node.action] =
                    std::move(retry_policy);
            }
        }
        if (!admission.retry_policies.empty()) {
            admission.retry_policy_digest =
                orchestrator::retryPoliciesDigest(
                    admission.retry_policies);
        }
        orchestrator::OrchestratorSubmitRequest submit;
        submit.dag = *decision.task_dag;
        submit.admission = admission;
        submit.idempotency_key = decision.task_dag->idempotency_key;
        submit.expected_capability_digest =
            catalog.value->catalog_digest;
        submit.trace_id = request.trace_id;
        submit.submitted_at_utc_ms = clock_->utcNowMs();
        auto orchestrator_call = internal;
        orchestrator_call.authorization_ref =
            admission.authorization_ref;
        const auto committed =
            orchestrator_->submit(submit, orchestrator_call);
        if (original_deadline_expired() &&
            !committed.accepted) {
            return deadline_failure(
                "orchestrator", "submit");
        }
        if (!committed.accepted) {
            return failureResult(
                request,
                Status::Error("orchestrator", committed.reject_code,
                              "orchestrator rejected plan"),
                "orchestrator", "submit");
        }
        result.plan_id = committed.plan_id;
        const auto pending_after_deadline =
            [this, &request, &result]() {
                result.success = false;
                result.pending = true;
                result.error_code =
                    "AGENT_SERVICE_RESULT_AFTER_DEADLINE";
                result.error_message =
                    safeClientErrorMessage();
                result.reply =
                    u8"请求已受理，执行结果请通过 plan_id 查询。";
                result.turn_summary =
                    "plan_result_after_deadline";
                logEvent(
                    request, "TURN_PENDING", "runTurn",
                    "deadline_elapsed",
                    data_log::EventSeverity::Warning,
                    data_log::DurabilityClass::D3Fsynced,
                    result.plan_id, result.error_code);
                return result;
            };
        logEvent(request, "PLAN_COMMITTED", "submit", "accepted",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D2Journaled, result.plan_id, {},
                 nlohmann::json{{"stage", "orchestration"},
                                {"status", "STARTED"},
                                {"output", {{"plan_id", result.plan_id},
                                            {"node_count", decision.task_dag->nodes.size()}}}}.dump());
        nlohmann::json submitted_nodes = nlohmann::json::array();
        for (const auto& node : decision.task_dag->nodes) {
            submitted_nodes.push_back({{"node_id", node.node_id},
                                       {"executor", node.executor},
                                       {"action", node.action},
                                       {"target_agent", node.target_agent},
                                       {"params", node.params},
                                       {"dependencies", node.dependencies}});
        }
        debugEvent(request, "PLAN_DETAIL", "orchestration",
                   nlohmann::json{{"received", {{"decision", "ExecutionPlan"},
                                                 {"nodes", std::move(submitted_nodes)}}},
                                  {"operation", "校验权限、幂等与依赖后提交 DAG"},
                                  {"produced", {{"plan_id", result.plan_id},
                                                 {"accepted", true}}}}.dump());
        if (original_deadline_expired()) {
            return pending_after_deadline();
        }
        const auto executed =
            orchestrator_->runUntilPlanTerminal(result.plan_id);
        auto plan_query = internal;
        const bool expired_after_execution =
            original_deadline_expired();
        if (expired_after_execution) {
            // Query is an internal observation after the business deadline;
            // it may enrich the pending response but cannot turn it into a
            // successful user result.
            plan_query.deadline_mono_ns =
                clock_->monotonicNowNs() +
                1'000'000'000LL;
        }
        const auto plan =
            orchestrator_->getPlan(result.plan_id, plan_query);
        if (expired_after_execution ||
            original_deadline_expired()) {
            if (plan.status.ok && plan.value) {
                result.plan_state = plan.value->state;
            }
            return pending_after_deadline();
        }
        if (!plan.status.ok) {
            return failureResult(request, plan.status, "orchestrator",
                                 "getPlan");
        }
        if (!plan.value) {
            return failureResult(
                request,
                Status::Error(
                    "orchestrator",
                    "ORCHESTRATOR_SUCCESS_WITHOUT_VALUE",
                    "orchestrator returned success without a plan"),
                "orchestrator", "getPlan");
        }
        result.plan_state = plan.value->state;
        result.reply = replyForPlan(*plan.value);
        nlohmann::json execution_nodes = nlohmann::json::array();
        for (const auto& [node_id, node] : plan.value->nodes) {
            execution_nodes.push_back({{"node_id", node_id},
                                       {"action", node.definition.action},
                                       {"executor", node.definition.executor},
                                       {"bound_params", node.bound_params},
                                       {"result", node.result},
                                       {"error_code", node.error_code},
                                       {"execution_id", node.execution_id},
                                       {"side_effect_state", toString(node.side_effect_state)}});
        }
        debugEvent(request, "EXECUTION_RESULT_DETAIL", "execution",
                   nlohmann::json{{"received", {{"plan_id", result.plan_id}}},
                                  {"operation", "执行 Tool/子 Agent 并收集完成证据"},
                                  {"produced", {{"nodes", std::move(execution_nodes)},
                                                 {"reply", result.reply}}}}.dump());
        for (const auto& event : atomic_->events()) {
            if (event.trace_id != request.trace_id) continue;
            const auto state = [&event]() {
                using atomic_service::AtomicExecutionState;
                switch (event.state) {
                    case AtomicExecutionState::Accepted: return "STARTED";
                    case AtomicExecutionState::Queued: return "STARTED";
                    case AtomicExecutionState::Running: return "STARTED";
                    case AtomicExecutionState::Suspended: return "STARTED";
                    case AtomicExecutionState::Succeeded: return "SUCCEEDED";
                    case AtomicExecutionState::Failed: return "FAILED";
                    case AtomicExecutionState::Cancelled: return "CANCELLED";
                    case AtomicExecutionState::Unknown: return "UNKNOWN";
                }
                return "UNKNOWN";
            }();
            logEvent(request, "ATOMIC_EXECUTION_OBSERVED", "executeTool",
                     event.event_type,
                     state == std::string("FAILED")
                         ? data_log::EventSeverity::Error
                         : data_log::EventSeverity::Info,
                     data_log::DurabilityClass::D2Journaled, result.plan_id,
                     event.error_code,
                     nlohmann::json{{"stage", "execution"},
                                    {"status", state},
                                    {"output", {{"tool_name", event.tool_name},
                                                {"execution_id", event.execution_id},
                                                {"side_effect_state", toString(event.side_effect_state)},
                                                {"result_digest", event.call_tool_result
                                                    ? secureDigest(event.call_tool_result->structured_content.dump())
                                                    : std::string{}}}}}.dump());
        }
        if (dispatch_) {
            for (const auto& event : dispatch_->events()) {
                if (event.trace_id != request.trace_id) continue;
                const auto terminal = event.state == agent_dispatch::DispatchState::Succeeded
                    ? "SUCCEEDED" : (event.state == agent_dispatch::DispatchState::Failed
                    ? "FAILED" : (event.state == agent_dispatch::DispatchState::Cancelled
                    ? "CANCELLED" : "STARTED"));
                logEvent(request, "SUBAGENT_EXECUTION_OBSERVED", "dispatchAgent",
                         event.event_type, data_log::EventSeverity::Info,
                         data_log::DurabilityClass::D2Journaled, result.plan_id,
                         event.error_code,
                         nlohmann::json{{"stage", "execution"},
                                        {"status", terminal},
                                        {"output", {{"agent_id", event.agent_id},
                                                    {"execution_id", event.execution_id},
                                                    {"side_effect_state", toString(event.side_effect_state)},
                                                    {"result_digest", secureDigest(event.result.dump())}}}}.dump());
            }
        }
        logEvent(request, "PLAN_OBSERVED", "getPlan",
                 planTerminal(plan.value->state) ? "terminal" : "pending",
                 data_log::EventSeverity::Info,
                 data_log::DurabilityClass::D2Journaled, result.plan_id, {},
                 nlohmann::json{{"stage", "reconciliation"},
                                {"status", planTerminal(plan.value->state)
                                               ? "SUCCEEDED" : "STARTED"},
                                {"output", {{"node_count", plan.value->nodes.size()}}}}.dump());
        if (!planTerminal(plan.value->state)) {
            // The reference runtime's synchronous driver is a bounded convenience.
            // STALLED/PUMP_LIMIT means a durably committed asynchronous plan
            // is still queued/running/reconciling; it is not a failed turn.
            result.pending = true;
            result.success = true;
            result.turn_summary =
                std::any_of(
                    plan.value->nodes.begin(),
                    plan.value->nodes.end(),
                    [](const auto& pair) {
                        return pair.second.state ==
                               orchestrator::ActivationState::Reconciling;
                    })
                    ? "plan_reconciling"
                    : "plan_pending";
        } else {
            (void)executed;
            result.success = planSuccess(plan.value->state);
            result.turn_summary =
                result.success ? "plan_succeeded"
                               : "plan_not_succeeded";
        }
        if (!result.success && !result.pending) {
            result.error_code =
                plan.value->state == orchestrator::PlanState::Unknown
                    ? "PLAN_EXECUTION_UNKNOWN"
                    : "PLAN_EXECUTION_FAILED";
            reportFailure(
                request,
                StructuredError{
                    "orchestrator", result.error_code, result.reply, false,
                    plan.value->state == orchestrator::PlanState::Unknown
                        ? SideEffectState::Unknown
                        : SideEffectState::NotApplicable},
                "orchestrator", "executePlan", result.plan_id);
        }
    }

    if (original_deadline_expired()) {
        return result.plan_id.empty()
                   ? deadline_failure(
                         "agent_service", "completeTurn")
                   : [&]() {
                         result.success = false;
                         result.pending = true;
                         result.error_code =
                             "AGENT_SERVICE_RESULT_AFTER_DEADLINE";
                         result.error_message =
                             safeClientErrorMessage();
                         result.reply =
                             u8"请求已受理，执行结果请通过 plan_id 查询。";
                         result.turn_summary =
                             "plan_result_after_deadline";
                         logEvent(
                             request, "TURN_PENDING",
                             "runTurn", "deadline_elapsed",
                             data_log::EventSeverity::Warning,
                             data_log::DurabilityClass::D3Fsynced,
                             result.plan_id,
                             result.error_code);
                         return result;
                     }();
    }

    // The frozen SDK contract writes a complete user/assistant turn.
    auto completed_request = request;
    auto memory_completion_call = internal;
    const auto memory_status = memory_->writeTurn(
        {completed_request,
         preprocessed.value->normalized_request.text, result.reply,
         "cockpit", 2},
        memory_completion_call);
    if (original_deadline_expired()) {
        return result.plan_id.empty()
                   ? deadline_failure("memory", "writeTurn")
                   : [&]() {
                         result.success = false;
                         result.pending = true;
                         result.error_code =
                             "AGENT_SERVICE_RESULT_AFTER_DEADLINE";
                         result.error_message =
                             safeClientErrorMessage();
                         result.reply =
                             u8"请求已受理，执行结果请通过 plan_id 查询。";
                         result.turn_summary =
                             "plan_result_after_deadline";
                         logEvent(
                             request, "TURN_PENDING",
                             "runTurn", "deadline_elapsed",
                             data_log::EventSeverity::Warning,
                             data_log::DurabilityClass::D3Fsynced,
                             result.plan_id,
                             result.error_code);
                         return result;
                     }();
    }
    if (!memory_status.ok) {
        logEvent(request, "MEMORY_WRITE_DEGRADED", "writeTurn", "degraded",
                 data_log::EventSeverity::Warning,
                 data_log::DurabilityClass::D2Journaled, result.plan_id,
                 memory_status.error.code);
        reportFailure(request, memory_status.error, "memory", "writeTurn",
                      result.plan_id);
    }
    logEvent(request,
             result.pending
                 ? "TURN_PENDING"
                 : (result.success ? "TURN_COMPLETED"
                                   : "TURN_FAILED"),
             "runTurn",
             result.pending
                 ? "pending"
                 : (result.success ? "success" : "failed"),
             (result.success || result.pending)
                 ? data_log::EventSeverity::Info
                 : data_log::EventSeverity::Error,
             data_log::DurabilityClass::D3Fsynced, result.plan_id,
             result.error_code,
             nlohmann::json{{"stage", "trace_finalize"},
                            {"status", result.pending ? "STARTED"
                                : (result.success ? "SUCCEEDED" : "FAILED")},
                            {"output", {{"reply_length", result.reply.size()},
                                        {"reply_digest", secureDigest(result.reply)},
                                        {"turn_summary", result.turn_summary},
                                        {"pending", result.pending}}}}.dump());
    if (original_deadline_expired()) {
        return result.plan_id.empty()
                   ? deadline_failure(
                         "data_log", "completeTurn")
                   : [&]() {
                         result.success = false;
                         result.pending = true;
                         result.error_code =
                             "AGENT_SERVICE_RESULT_AFTER_DEADLINE";
                         result.error_message =
                             safeClientErrorMessage();
                         result.reply =
                             u8"请求已受理，执行结果请通过 plan_id 查询。";
                         result.turn_summary =
                             "plan_result_after_deadline";
                         return result;
                     }();
    }
    return result;
}


}  // namespace master_agent::agent_service
