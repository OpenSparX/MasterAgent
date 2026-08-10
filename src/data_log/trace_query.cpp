/**
 * @file trace_query.cpp
 * @brief Queries paginated trace records from committed journals.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

Result<TracePage> DataLogService::queryTrace(
    const TraceQuery& query, const CallContext& call) const {

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        return Result<TracePage>::Failure(Status::Error(
            "data_log", "LOG_NOT_READY",
            "trace query is unavailable before journal recovery", true));
    }
    if (!authorizedAgentServiceObserver(call) ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        !isValidTaskPriority(call.priority)) {
        return Result<TracePage>::Failure(Status::Error(
            "data_log", "TRACE_CALLER_NOT_ALLOWED",
            "trace queries must be mediated by AgentService"));
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<TracePage>::Failure(Status::Error(
            "data_log", "TRACE_CALL_EXPIRED",
            "trace query deadline expired"));
    }
    const bool has_selector =
        query.trace_id || query.request_id ||
        query.plan_id || query.execution_id;
    if (!has_selector || query.max_records == 0 ||
        query.max_records > 1000) {
        return Result<TracePage>::Failure(Status::Error(
            "data_log", "TRACE_QUERY_INVALID",
            "trace query requires a selector and a bounded page size"));
    }
    TracePage page;
    const std::size_t limit = query.max_records;
    for (const auto& event : events_) {
        if (matches(event, query)) {
            if (page.events.size() < limit) {
                page.events.push_back(event);
            } else {
                page.complete_for_requested_range = false;
                break;
            }
        }
    }
    return Result<TracePage>::Success(std::move(page));
}


}  // namespace master_agent::data_log
