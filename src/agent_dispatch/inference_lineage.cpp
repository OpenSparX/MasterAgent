/**
 * @file inference_lineage.cpp
 * @brief Validates and leases inference child jobs against live agent leases.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <utility>

namespace master_agent::agent_dispatch {

Status AgentDispatch::validateInferenceParentLineage(
    const inference::InferenceRequest& request,
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validateInferenceParentLineageUnlocked(request, call);
}

Status
AgentDispatch::validateInferenceParentLineageUnlocked(
    const inference::InferenceRequest& request,
    const CallContext& call) const {
    if (!hasHostModuleIdentity(call, CallerModuleId::SubAgent) ||
        request.admission.caller_module_id !=
            CallerModuleId::SubAgent ||
        request.parent_operation_id.empty()) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_LINEAGE_CALLER_INVALID",
            "SubAgent identity and parent operation are required");
    }
    const auto operation =
        operation_to_dispatch_.find(request.parent_operation_id);
    if (operation == operation_to_dispatch_.end()) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_OPERATION_NOT_FOUND",
            "parent Dispatch operation is not active");
    }
    const auto found = dispatches_.find(operation->second);
    if (found == dispatches_.end()) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_DISPATCH_NOT_FOUND",
            "parent Dispatch record is unavailable");
    }
    const auto& parent = found->second;
    if (parent.state != DispatchState::Running ||
        provider_submitted_.count(parent.dispatch_id) == 0) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_DISPATCH_NOT_RUNNING",
            "child inference requires a running parent lease");
    }
    const auto agent = agents_.find(parent.route.agent_id);
    if (agent == agents_.end() ||
        agent->second.manifest.agent_epoch !=
            parent.route.agent_epoch ||
        agent->second.manifest.manifest_digest !=
            parent.route.manifest_digest) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_AGENT_MANIFEST_STALE",
            "parent Agent manifest no longer matches its route");
    }
    if (request.parent_dispatch_id != parent.dispatch_id ||
        request.parent_agent_id != parent.route.agent_id ||
        request.parent_agent_epoch != parent.route.agent_epoch ||
        request.parent_lease_id != parent.route.lease_id ||
        request.parent_fencing_token !=
            parent.task.fencing_token ||
        request.request_id != parent.task.request_id ||
        request.trace_id != parent.task.trace_id ||
        request.admission.principal_id !=
            parent.task.principal_id_hash ||
        call.request_id != parent.task.request_id ||
        call.trace_id != parent.task.trace_id ||
        call.principal_id_hash !=
            parent.task.principal_id_hash ||
        call.authorization_ref !=
            parent.task.authorization_ref ||
        isHigherPriority(request.priority, parent.task.priority) ||
        isHigherPriority(call.priority, parent.task.priority) ||
        request.deadline_mono_ns >
            parent.task.deadline_mono_ns ||
        request.admission.deadline_mono_ns >
            parent.task.deadline_mono_ns ||
        call.deadline_mono_ns >
            parent.task.deadline_mono_ns) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_LINEAGE_MISMATCH",
            "child inference context does not match the parent lease");
    }
    if (request.model !=
        agent->second.manifest.model_profile_id) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_CHILD_MODEL_PROFILE_NOT_AUTHORIZED",
            "child model must match the selected Agent manifest");
    }
    if (request.idempotency_key !=
        inference::inferenceChildIdempotencyKey(
            parent.dispatch_id, request.job_id)) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_CHILD_IDEMPOTENCY_KEY_INVALID",
            "child idempotency key is not derived from parent and job");
    }
    return Status::Ok();
}

// Pin the parent lease across the physical runtime call.
Result<std::string>
AgentDispatch::acquireInferenceParentInvocationLease(
    const inference::InferenceRequest& request,
    const CallContext& call) {
    if (!ids_) {
        return Result<std::string>::Failure(Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_INVOCATION_LEASE_UNAVAILABLE",
            "parent invocation lease allocator is unavailable"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto validated =
        validateInferenceParentLineageUnlocked(request, call);
    if (!validated.ok) {
        return Result<std::string>::Failure(validated);
    }
    const auto existing =
        inference_child_job_to_reservation_.find(request.job_id);
    if (existing !=
        inference_child_job_to_reservation_.end()) {
        const auto reservation =
            inference_child_reservations_.find(existing->second);
        if (reservation ==
                inference_child_reservations_.end() ||
            reservation->second.child_idempotency_key !=
                request.idempotency_key ||
            reservation->second.parent_dispatch_id !=
                request.parent_dispatch_id ||
            reservation->second.parent_operation_id !=
                request.parent_operation_id ||
            reservation->second.parent_agent_id !=
                request.parent_agent_id ||
            reservation->second.parent_agent_epoch !=
                request.parent_agent_epoch ||
            reservation->second.parent_lease_id !=
                request.parent_lease_id ||
            reservation->second.parent_fencing_token !=
                request.parent_fencing_token ||
            dispatches_.at(request.parent_dispatch_id)
                    .control_epoch !=
                reservation->second.parent_control_epoch) {
            return Result<std::string>::Failure(Status::Error(
                "agent_dispatch",
                "INFERENCE_CHILD_INVOCATION_RESERVATION_CONFLICT",
                "child job is bound to a different parent lease"));
        }
        return Result<std::string>::Success(existing->second);
    }

    InferenceChildInvocationReservation reservation;
    reservation.reservation_id =
        ids_->next("inference-parent-invocation-lease");
    reservation.child_job_id = request.job_id;
    reservation.child_idempotency_key =
        request.idempotency_key;
    reservation.parent_dispatch_id =
        request.parent_dispatch_id;
    reservation.parent_operation_id =
        request.parent_operation_id;
    reservation.parent_agent_id =
        request.parent_agent_id;
    reservation.parent_agent_epoch =
        request.parent_agent_epoch;
    reservation.parent_lease_id =
        request.parent_lease_id;
    reservation.parent_fencing_token =
        request.parent_fencing_token;
    reservation.parent_control_epoch =
        dispatches_.at(request.parent_dispatch_id).control_epoch;
    const auto reservation_id = reservation.reservation_id;
    inference_child_reservations_.emplace(
        reservation_id, std::move(reservation));
    inference_child_job_to_reservation_[request.job_id] =
        reservation_id;
    return Result<std::string>::Success(reservation_id);
}

Status AgentDispatch::releaseInferenceParentInvocationLease(
    const std::string& reservation_id) {
    if (reservation_id.empty()) {
        return Status::Error(
            "agent_dispatch",
            "INFERENCE_PARENT_INVOCATION_LEASE_INVALID",
            "parent invocation reservation id is empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        inference_child_reservations_.find(reservation_id);
    if (found == inference_child_reservations_.end()) {
        return Status::Ok();
    }
    const auto reservation = found->second;
    inference_child_reservations_.erase(found);
    const auto reverse =
        inference_child_job_to_reservation_.find(
            reservation.child_job_id);
    if (reverse !=
            inference_child_job_to_reservation_.end() &&
        reverse->second == reservation_id) {
        inference_child_job_to_reservation_.erase(reverse);
    }
    const auto parent =
        dispatches_.find(reservation.parent_dispatch_id);
    if (parent != dispatches_.end() &&
        (parent->second.route.agent_id !=
             reservation.parent_agent_id ||
         parent->second.route.agent_epoch !=
             reservation.parent_agent_epoch ||
         parent->second.route.lease_id !=
             reservation.parent_lease_id ||
         parent->second.task.fencing_token !=
             reservation.parent_fencing_token ||
         parent->second.control_epoch !=
             reservation.parent_control_epoch)) {
        auto& snapshot = parent->second;
        if (!hasActiveChildInvocationUnlocked(
                reservation.parent_dispatch_id)) {
            deferred_parent_transitions_.erase(
                reservation.parent_dispatch_id);
        }
        snapshot.result = nlohmann::json::object();
        snapshot.side_effect_state = SideEffectState::Unknown;
        snapshot.error_code =
            "DISPATCH_PARENT_LEASE_CHANGED_DURING_CHILD_INFERENCE";
        setStateUnlocked(snapshot, DispatchState::Unknown);
        emit(snapshot, "UNKNOWN", false, false);
    }
    applyDeferredParentTransitionUnlocked(
        reservation.parent_dispatch_id);
    return Status::Ok();
}


}  // namespace master_agent::agent_dispatch
