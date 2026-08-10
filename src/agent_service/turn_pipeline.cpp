/**
 * @file turn_pipeline.cpp
 * @brief Executes the end-to-end interaction, intent, plan, and response pipeline.
 */

#include "include/plan_response.h"
#include "include/capability_policy.h"
#include "include/client_error_sanitization.h"

#include <thread>

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
             data_log::DurabilityClass::D2Journaled);
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
             data_log::DurabilityClass::D1Buffered);
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
    for (std::size_t observation = 0; observation < 100000;
         ++observation) {
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
        std::this_thread::yield();
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
                 data_log::DurabilityClass::D2Journaled, result.plan_id);
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
             result.error_code);
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
