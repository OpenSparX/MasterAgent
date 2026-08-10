/**
 * @file observability.cpp
 * @brief Projects turn events and failures into data-log and exception services.
 */

#include "include/plan_response.h"
#include "include/capability_policy.h"
#include "include/client_error_sanitization.h"

namespace master_agent::agent_service {

void AgentService::logEvent(
    const interaction::StandardRequest& request,
    const std::string& event_type, const std::string& operation,
    const std::string& outcome, data_log::EventSeverity severity,
    data_log::DurabilityClass durability, const std::string& plan_id,
    const std::string& error_ref) {

    std::lock_guard<std::mutex> producer_lock(
        log_producer_mutex_);
    data_log::LogEvent event;
    event.event_id = ids_->next("log-event");
    event.event_type = event_type;
    event.module = "AgentService";
    event.interface_name = "IAgentService";
    event.operation = operation;
    event.context.request_id = request.request_id;
    event.context.trace_id = request.trace_id;
    event.context.span_id = ids_->next("span");
    event.context.session_id = request.session_id;
    if (!plan_id.empty()) event.context.plan_id = plan_id;
    event.context.producer_endpoint_id =
        "agent-service/data-log";
    event.context.producer_epoch = producer_epoch_;
    event.context.producer_sequence =
        log_observation_sequence_.fetch_add(1);
    event.context.task_priority = request.priority;
    event.context.deadline_mono_ns = request.deadline_mono_ns;
    event.outcome = outcome;
    if (!error_ref.empty()) {
        event.error_ref = normalizedErrorCode(error_ref);
    }
    event.occurred_at_utc_ms = clock_->utcNowMs();
    event.occurred_at_mono_ns = clock_->monotonicNowNs();
    event.severity = severity;
    event.requested_durability = durability;
    event.payload_summary_json =
        "{\"content\":\"redacted\",\"request_linked\":true}";
    data_log::LogEventBatch batch;
    batch.batch_id = ids_->next("log-batch");
    batch.producer_endpoint_id = event.context.producer_endpoint_id;
    batch.producer_epoch = event.context.producer_epoch;
    batch.first_sequence = event.context.producer_sequence;
    batch.last_sequence = event.context.producer_sequence;
    batch.records.push_back(std::move(event));
    batch.checksum = secureDigest(batch.batch_id + "|" + event_type);
    CallContext log_call{CallerModuleId::AgentService, request.request_id,
                         request.trace_id, {}, request.priority,
                         clock_->monotonicNowNs() + 1'000'000'000LL,
                         "agent-service/data-log",
                         producer_epoch_};
    const auto appended = log_->appendEvents(batch, log_call);
    if (!appended.status.ok) {
        auto concrete = std::dynamic_pointer_cast<data_log::DataLogService>(
            log_);
        if (concrete) {
            concrete->appendEmergencySummary(event_type + ":" +
                                             appended.status.error.code);
        }
    }
}

void AgentService::reportFailure(
    const interaction::StandardRequest& request,
    const StructuredError& error, const std::string& source_module,
    const std::string& operation, const std::string& plan_id) {

    std::lock_guard<std::mutex> producer_lock(
        exception_producer_mutex_);
    exception::ExceptionOccurrence occurrence;
    occurrence.occurrence_id = ids_->next("occurrence");
    occurrence.producer_endpoint_id =
        "agent-service/exception";
    occurrence.producer_epoch = producer_epoch_;
    occurrence.producer_sequence =
        exception_observation_sequence_.fetch_add(1);
    occurrence.domain =
        normalizedErrorDomain(error.domain, source_module);
    occurrence.code = normalizedErrorCode(error.code);
    occurrence.reported_severity =
        error.side_effect_state == SideEffectState::Unknown
            ? exception::ExceptionSeverity::Critical
            : exception::ExceptionSeverity::Error;
    occurrence.impact =
        error.side_effect_state == SideEffectState::Unknown
            ? exception::ExceptionImpact::SafetyAffected
            : exception::ExceptionImpact::RequestFailed;
    occurrence.source_module = source_module;
    occurrence.source_interface = "agent-service";
    occurrence.operation = operation;
    occurrence.context.request_id = request.request_id;
    occurrence.context.trace_id = request.trace_id;
    occurrence.context.span_id = ids_->next("span");
    occurrence.context.session_id = request.session_id;
    if (!plan_id.empty()) occurrence.context.plan_id = plan_id;
    occurrence.context.producer_endpoint_id =
        "agent-service/exception";
    occurrence.context.producer_epoch = producer_epoch_;
    occurrence.context.producer_sequence = occurrence.producer_sequence;
    occurrence.context.task_priority = request.priority;
    occurrence.context.deadline_mono_ns =
        request.deadline_mono_ns;
    occurrence.side_effect_state = error.side_effect_state;
    occurrence.recoverable_hint = error.retryable;
    occurrence.retryable_hint = error.retryable;
    occurrence.bounded_detail_code = occurrence.code;
    occurrence.bounded_detail_summary =
        "failure_detail_redacted";
    occurrence.privacy_labels = {"EXCEPTION_METADATA"};
    occurrence.occurred_at_utc_ms = clock_->utcNowMs();
    occurrence.occurred_at_mono_ns = clock_->monotonicNowNs();
    occurrence.received_at_utc_ms = clock_->utcNowMs();
    exception::ExceptionReportRequest report;
    report.report_id = ids_->next("exception-report");
    report.occurrences.push_back(std::move(occurrence));
    report.source_redaction_proof =
        "producer-redacted:v1";
    report.requested_durability =
        error.side_effect_state == SideEffectState::Unknown
            ? data_log::DurabilityClass::D3Fsynced
            : data_log::DurabilityClass::D2Journaled;
    report.batch_checksum =
        exception::exceptionBatchChecksum(report);
    CallContext exception_call{
        CallerModuleId::AgentService, request.request_id, request.trace_id, {},
        request.priority,
        clock_->monotonicNowNs() + 1'000'000'000LL,
        "agent-service/exception",
        producer_epoch_};
    exceptions_->report(report, exception_call);
}


}  // namespace master_agent::agent_service
