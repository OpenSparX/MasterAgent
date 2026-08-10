/**
 * @file service_control.cpp
 * @brief Implements flush, health, and emergency-summary operations.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

Status DataLogService::flush(const CallContext& call) {

    std::scoped_lock writer_locks(event_writer_mutex_,
                                  audit_writer_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_) {
        return Status::Error("data_log", "LOG_NOT_READY",
                             "log service is not ready", true);
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error("data_log", "LOG_FLUSH_DEADLINE_EXPIRED",
                             "flush deadline expired");
    }
    if (call.caller != CallerModuleId::AgentService &&
        call.caller != CallerModuleId::DataLogManager) {
        return Status::Error("data_log", "LOG_FLUSH_CALLER_NOT_ALLOWED",
                             "caller is not allowed to flush journals");
    }
    lock.unlock();
    event_stream_.flush();
    audit_stream_.flush();
    if (!event_stream_ || !audit_stream_) {
        return Status::Error("data_log", "LOG_FLUSH_FAILED",
                             "journal flush failed", true);
    }
    const auto event_sync = invokeDurabilitySync(
        durability_sync_,
        storage_directory_ / "events.jsonl");
    if (!event_sync.ok) return event_sync;
    const auto audit_sync = invokeDurabilitySync(
        durability_sync_,
        storage_directory_ / "audit.jsonl");
    if (!audit_sync.ok) return audit_sync;
    lock.lock();
    persisted_event_ids_.insert(buffered_event_ids_.begin(),
                                buffered_event_ids_.end());
    buffered_event_ids_.clear();
    return Status::Ok();
}

LogHealth DataLogService::getHealth(
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool authorized =
        (authorizedAgentServiceObserver(call) ||
         hasHostModuleIdentity(
             call, CallerModuleId::DataLogManager)) &&
        isValidTaskPriority(call.priority) &&
        !call.request_id.empty() && !call.trace_id.empty() &&
        !call.principal_id_hash.empty() &&
        call.deadline_mono_ns > 0 && clock_ &&
        !deadlineExpired(call.deadline_mono_ns, *clock_);
    if (!authorized) return {};
    return {ready_ && !audit_integrity_degraded_,
            buffered_event_ids_.size(),
            persisted_event_ids_.size(), audits_.size(),
            emergency_ring_.size(), hash_chain_head_,
            ready_ && !audit_integrity_degraded_ &&
                tamper_evidence_ &&
                !tamper_key_generation_.empty(),
            audit_integrity_degraded_,
            tamper_key_generation_,
            audit_anchor_generation_};
}

void DataLogService::appendEmergencySummary(const std::string& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    appendEmergencySummaryUnlocked(summary);
}

void DataLogService::appendEmergencySummaryUnlocked(
    const std::string& summary) {
    constexpr std::size_t kEmergencyCapacity = 64;
    if (emergency_ring_.size() == kEmergencyCapacity) {
        emergency_ring_.erase(emergency_ring_.begin());
    }
    emergency_ring_.push_back(summary.substr(0, 256));
}


}  // namespace master_agent::data_log
