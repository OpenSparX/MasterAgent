/**
 * @file exception_query.cpp
 * @brief Implements exception lookup and public mutation entry points.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

Result<ExceptionGroup> ExceptionManager::getException(
    const std::string& exception_id, const CallContext& call) const {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) return Result<ExceptionGroup>::Failure(caller);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ || call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<ExceptionGroup>::Failure(Status::Error(
            "exception", "EXM_CALL_EXPIRED",
            "a live monotonic deadline is required"));
    }
    if (!ready_) {
        return Result<ExceptionGroup>::Failure(
            initialization_status_);
    }
    const auto found = groups_.find(exception_id);
    if (found == groups_.end()) {
        return Result<ExceptionGroup>::Failure(Status::Error(
            "exception", "EXM_GROUP_NOT_FOUND",
            "exception group not found"));
    }
    return Result<ExceptionGroup>::Success(found->second);
}

Result<ExceptionMutationResult> ExceptionManager::acknowledge(
    const ExceptionMutationRequest& request, const CallContext& call) {
    return mutate(request, ExceptionLifecycle::Acknowledged, call);
}

Result<ExceptionMutationResult> ExceptionManager::markMitigating(
    const ExceptionMutationRequest& request, const CallContext& call) {
    return mutate(request, ExceptionLifecycle::Mitigating, call);
}

Result<ExceptionMutationResult> ExceptionManager::resolve(
    const ExceptionMutationRequest& request, const CallContext& call) {
    return mutate(request, ExceptionLifecycle::Resolved, call);
}

std::string ExceptionManager::fingerprintOf(
    const ExceptionOccurrence& occurrence) {

    const auto capability =
        occurrence.capability_id ? *occurrence.capability_id : "";
    const auto side_effect =
        occurrence.side_effect_state
            ? toString(*occurrence.side_effect_state)
            : "";
    return secureDigest(
        "fingerprint-v1|" + occurrence.domain + "|" + occurrence.code +
        "|" + occurrence.source_module + "|" +
        occurrence.source_interface + "|" + occurrence.operation + "|" +
        capability + "|" + side_effect);
}

EscalationKind ExceptionManager::escalationFor(
    const ExceptionOccurrence& occurrence, std::uint64_t count) {

    if (occurrence.reported_severity == ExceptionSeverity::Critical ||
        occurrence.impact == ExceptionImpact::SafetyAffected ||
        occurrence.side_effect_state == SideEffectState::Unknown) {
        return EscalationKind::SafetyCandidate;
    }
    return count >= 3 ? EscalationKind::Ops : EscalationKind::None;
}

// Lifecycle transitions use expected_version to prevent lost updates. Durable
// transaction success remains authoritative even if the later log projection
// is delayed or unavailable.

}  // namespace master_agent::exception
