/**
 * @file preprocess_observer.cpp
 * @brief Connects preprocessing events to optional DataLog and Exception services.
 *
 * Design mapping: Sections 2.1 and 5. User text and parameter values are
 * never copied into observability payloads.
 */

#include "include/preprocess_observer.h"

#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"

#include <utility>

namespace master_agent::preprocess::detail {

PreprocessObserver::PreprocessObserver(
    std::shared_ptr<IRuntimeClock> clock,
    std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<data_log::IDataLogService> log_service,
    std::shared_ptr<exception::IExceptionManager>
        exception_manager)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      log_service_(std::move(log_service)),
      exception_manager_(std::move(exception_manager)) {
    if (!ids_) {
        ids_ = std::make_shared<IdGenerator>(
            "preprocess");
    }
}

void PreprocessObserver::record(
    const CallContext& call,
    const std::string& session_id,
    const std::string& operation,
    const std::string& outcome,
    const std::string& error_code) const noexcept {
    if (!clock_ || !ids_) {
        return;
    }

    const auto endpoint =
        hostModuleEndpoint(
            CallerModuleId::PreprocessingEngine);
    const auto epoch =
        hostModuleProcessEpoch(
            CallerModuleId::PreprocessingEngine);
    const auto observation_deadline =
        clock_->monotonicNowNs() +
        1'000'000'000LL;
    const CallContext observation_call{
        CallerModuleId::PreprocessingEngine,
        call.request_id,
        call.trace_id,
        call.principal_id_hash,
        call.priority,
        observation_deadline,
        endpoint,
        epoch,
        call.authorization_ref};

    if (log_service_) {
        try {
            const auto sequence =
                log_sequence_.fetch_add(1);
            data_log::LogEvent event;
            event.event_id =
                ids_->next("preprocess-event");
            event.event_type =
                error_code.empty()
                    ? "PREPROCESS_OPERATION_COMPLETED"
                    : "PREPROCESS_OPERATION_FAILED";
            event.module = "PreprocessingEngine";
            event.interface_name =
                operation == "process"
                    ? "IPreprocess"
                    : "IStateQuery";
            event.operation = operation;
            event.context.request_id = call.request_id;
            event.context.trace_id = call.trace_id;
            event.context.span_id =
                ids_->next("preprocess-span");
            if (!session_id.empty()) {
                event.context.session_id = session_id;
            }
            event.context.producer_endpoint_id =
                endpoint;
            event.context.producer_epoch = epoch;
            event.context.producer_sequence = sequence;
            event.context.task_priority = call.priority;
            event.context.deadline_mono_ns =
                observation_deadline;
            event.outcome = outcome;
            if (!error_code.empty()) {
                event.error_ref = error_code;
            }
            event.occurred_at_utc_ms =
                clock_->utcNowMs();
            event.occurred_at_mono_ns =
                clock_->monotonicNowNs();
            event.severity =
                error_code.empty()
                    ? data_log::EventSeverity::Info
                    : data_log::EventSeverity::Warning;
            event.requested_durability =
                data_log::DurabilityClass::D1Buffered;
            event.privacy_labels =
                {"OPERATIONAL_METADATA"};
            event.payload_summary_json =
                "{\"content\":\"redacted\","
                "\"request_linked\":true}";

            data_log::LogEventBatch batch;
            batch.batch_id =
                ids_->next("preprocess-log-batch");
            batch.producer_endpoint_id = endpoint;
            batch.producer_epoch = epoch;
            batch.first_sequence = sequence;
            batch.last_sequence = sequence;
            batch.checksum = secureDigest(
                batch.batch_id + "|" +
                event.event_type + "|" + operation);
            batch.records.push_back(std::move(event));
            (void)log_service_->appendEvents(
                batch, observation_call);
        } catch (...) {
            // The design defines observability as an injected dependency. Failure
            // to record best-effort metadata must not rewrite process truth.
        }
    }

    if (!exception_manager_ ||
        error_code.empty()) {
        return;
    }
    try {
        const auto sequence =
            exception_sequence_.fetch_add(1);
        exception::ExceptionOccurrence occurrence;
        occurrence.occurrence_id =
            ids_->next("preprocess-occurrence");
        occurrence.producer_endpoint_id = endpoint;
        occurrence.producer_epoch = epoch;
        occurrence.producer_sequence = sequence;
        occurrence.domain = "preprocess";
        occurrence.code = error_code;
        occurrence.reported_severity =
            exception::ExceptionSeverity::Warning;
        occurrence.impact =
            exception::ExceptionImpact::RequestFailed;
        occurrence.source_module =
            "PreprocessingEngine";
        occurrence.source_interface =
            operation == "process"
                ? "IPreprocess"
                : "IStateQuery";
        occurrence.operation = operation;
        occurrence.context.request_id = call.request_id;
        occurrence.context.trace_id = call.trace_id;
        occurrence.context.span_id =
            ids_->next("preprocess-exception-span");
        if (!session_id.empty()) {
            occurrence.context.session_id = session_id;
        }
        occurrence.context.producer_endpoint_id =
            endpoint;
        occurrence.context.producer_epoch = epoch;
        occurrence.context.producer_sequence = sequence;
        occurrence.context.task_priority = call.priority;
        occurrence.context.deadline_mono_ns =
            observation_deadline;
        occurrence.side_effect_state =
            SideEffectState::NotApplicable;
        occurrence.recoverable_hint = false;
        occurrence.retryable_hint = false;
        occurrence.bounded_detail_code = error_code;
        occurrence.bounded_detail_summary =
            "preprocess_failure_redacted";
        occurrence.privacy_labels =
            {"EXCEPTION_METADATA"};
        occurrence.occurred_at_utc_ms =
            clock_->utcNowMs();
        occurrence.occurred_at_mono_ns =
            clock_->monotonicNowNs();
        occurrence.received_at_utc_ms =
            clock_->utcNowMs();

        exception::ExceptionReportRequest report;
        report.report_id =
            ids_->next("preprocess-exception-report");
        report.occurrences.push_back(
            std::move(occurrence));
        report.source_redaction_proof =
            "producer-redacted:v1";
        report.requested_durability =
            data_log::DurabilityClass::D2Journaled;
        report.batch_checksum =
            exception::exceptionBatchChecksum(report);
        (void)exception_manager_->report(
            report, observation_call);
    } catch (...) {
        // The same non-interference rule applies to exception reporting.
    }
}

}  // namespace master_agent::preprocess::detail
