/**
 * @file atomic_lineage.cpp
 * @brief Validates and leases atomic child calls against live parent dispatches.
 */

#include "include/manifest_validation.h"

#include <algorithm>
#include <utility>

namespace master_agent::agent_dispatch {

Status AgentDispatch::validateAtomicParentLineage(
    const atomic_service::AtomicMcpCallEnvelope& request,
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validateAtomicParentLineageUnlocked(request, call);
}

Status AgentDispatch::validateAtomicParentLineageUnlocked(
    const atomic_service::AtomicMcpCallEnvelope& request,
    const CallContext& call) const {
    if (!hasHostModuleIdentity(call, CallerModuleId::SubAgent) ||
        request.runtime.caller_module_id !=
            CallerModuleId::SubAgent ||
        !request.runtime.parent_operation_id ||
        request.runtime.parent_operation_id->empty()) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_LINEAGE_CALLER_INVALID",
            "SubAgent identity and parent operation are required");
    }
    const auto operation = operation_to_dispatch_.find(
        *request.runtime.parent_operation_id);
    if (operation == operation_to_dispatch_.end()) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_OPERATION_NOT_FOUND",
            "parent Dispatch operation is not active");
    }
    const auto found = dispatches_.find(operation->second);
    if (found == dispatches_.end()) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_DISPATCH_NOT_FOUND",
            "parent Dispatch record is unavailable");
    }
    const auto& parent = found->second;
    if (parent.state != DispatchState::Running ||
        provider_submitted_.count(parent.dispatch_id) == 0) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_DISPATCH_NOT_RUNNING",
            "child Tool requires a running parent lease");
    }
    if (request.runtime.parent_dispatch_id !=
            parent.dispatch_id ||
        request.runtime.parent_agent_id !=
            parent.route.agent_id ||
        request.runtime.parent_agent_epoch !=
            parent.route.agent_epoch ||
        request.runtime.parent_lease_id !=
            parent.route.lease_id ||
        request.runtime.parent_fencing_token !=
            parent.task.fencing_token ||
        request.runtime.fencing_token !=
            parent.task.fencing_token ||
        request.runtime.request_id !=
            parent.task.request_id ||
        request.runtime.trace_id != parent.task.trace_id ||
        request.runtime.plan_id != parent.task.plan_id ||
        request.runtime.pid != parent.task.pid ||
        request.runtime.activation_id !=
            parent.task.activation_id ||
        request.runtime.principal_id_hash !=
            parent.task.principal_id_hash ||
        request.runtime.authorization_ref !=
            parent.task.authorization_ref ||
        isHigherPriority(request.runtime.priority,
                         parent.task.priority) ||
        request.runtime.deadline_mono_ns >
            parent.task.deadline_mono_ns) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_LINEAGE_MISMATCH",
            "child runtime context does not match the parent lease");
    }
    if (request.runtime.idempotency_key !=
        atomicChildIdempotencyKey(
            parent.dispatch_id,
            request.runtime.operation_id)) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_CHILD_IDEMPOTENCY_KEY_INVALID",
            "child idempotency key is not derived from parent and operation");
    }
    if (std::find(
            parent.task.allowed_child_capabilities.begin(),
            parent.task.allowed_child_capabilities.end(),
            request.mcp_request.name) ==
        parent.task.allowed_child_capabilities.end()) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_CHILD_CAPABILITY_NOT_AUTHORIZED",
            "parent Admission does not authorize this child Tool");
    }
    const std::set<std::string> parent_permissions{
        parent.task.granted_permissions.begin(),
        parent.task.granted_permissions.end()};
    if (!validBoundedUniqueClaims(
            request.runtime.granted_permissions) ||
        std::any_of(
            request.runtime.granted_permissions.begin(),
            request.runtime.granted_permissions.end(),
            [&parent_permissions](const auto& permission) {
                return parent_permissions.count(permission) == 0;
            })) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_CHILD_PERMISSION_ESCALATION",
            "child permissions exceed the parent Admission");
    }
    const std::set<std::string> parent_resource_leases{
        parent.task.resource_lease_refs.begin(),
        parent.task.resource_lease_refs.end()};
    if (!validBoundedUniqueClaims(
            request.runtime.resource_lease_refs) ||
        std::any_of(
            request.runtime.resource_lease_refs.begin(),
            request.runtime.resource_lease_refs.end(),
            [&parent_resource_leases](const auto& lease) {
                return parent_resource_leases.count(lease) == 0;
            })) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_CHILD_RESOURCE_LEASE_ESCALATION",
            "child resource leases exceed the parent Dispatch");
    }
    return Status::Ok();
}

Result<std::string>
AgentDispatch::acquireAtomicParentInvocationLease(
    const atomic_service::AtomicMcpCallEnvelope& request,
    const CallContext& call) {

    if (!ids_) {
        return Result<std::string>::Failure(Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_INVOCATION_LEASE_UNAVAILABLE",
            "parent invocation lease allocator is unavailable"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto validated =
        validateAtomicParentLineageUnlocked(request, call);
    if (!validated.ok) {
        return Result<std::string>::Failure(validated);
    }

    const auto existing =
        atomic_child_execution_to_reservation_.find(
            request.runtime.execution_id);
    if (existing !=
        atomic_child_execution_to_reservation_.end()) {
        const auto reservation =
            atomic_child_reservations_.find(existing->second);
        if (reservation ==
                atomic_child_reservations_.end() ||
            reservation->second.parent_dispatch_id !=
                request.runtime.parent_dispatch_id ||
            reservation->second.child_operation_id !=
                request.runtime.operation_id ||
            reservation->second.child_idempotency_key !=
                request.runtime.idempotency_key ||
            reservation->second.parent_operation_id !=
                *request.runtime.parent_operation_id ||
            reservation->second.parent_agent_id !=
                request.runtime.parent_agent_id ||
            reservation->second.parent_agent_epoch !=
                request.runtime.parent_agent_epoch ||
            reservation->second.parent_lease_id !=
                request.runtime.parent_lease_id ||
            reservation->second.parent_fencing_token !=
                request.runtime.parent_fencing_token ||
            dispatches_.at(
                request.runtime.parent_dispatch_id)
                    .control_epoch !=
                reservation->second.parent_control_epoch) {
            return Result<std::string>::Failure(Status::Error(
                "agent_dispatch",
                "ATOMIC_CHILD_INVOCATION_RESERVATION_CONFLICT",
                "child execution is bound to a different parent lease"));
        }
        return Result<std::string>::Success(existing->second);
    }

    AtomicChildInvocationReservation reservation;
    reservation.reservation_id =
        ids_->next("atomic-parent-invocation-lease");
    reservation.child_execution_id =
        request.runtime.execution_id;
    reservation.child_operation_id =
        request.runtime.operation_id;
    reservation.child_idempotency_key =
        request.runtime.idempotency_key;
    reservation.parent_dispatch_id =
        request.runtime.parent_dispatch_id;
    reservation.parent_operation_id =
        *request.runtime.parent_operation_id;
    reservation.parent_agent_id =
        request.runtime.parent_agent_id;
    reservation.parent_agent_epoch =
        request.runtime.parent_agent_epoch;
    reservation.parent_lease_id =
        request.runtime.parent_lease_id;
    reservation.parent_fencing_token =
        request.runtime.parent_fencing_token;
    reservation.parent_control_epoch =
        dispatches_.at(
            request.runtime.parent_dispatch_id)
            .control_epoch;
    const auto reservation_id = reservation.reservation_id;
    atomic_child_reservations_.emplace(
        reservation_id, std::move(reservation));
    atomic_child_execution_to_reservation_[
        request.runtime.execution_id] = reservation_id;
    return Result<std::string>::Success(reservation_id);
}

Status AgentDispatch::releaseAtomicParentInvocationLease(
    const std::string& reservation_id) {
    if (reservation_id.empty()) {
        return Status::Error(
            "agent_dispatch",
            "ATOMIC_PARENT_INVOCATION_LEASE_INVALID",
            "parent invocation reservation id is empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found =
        atomic_child_reservations_.find(reservation_id);
    if (found == atomic_child_reservations_.end()) {
        // Release is deliberately idempotent so a Provider completion
        // replay cannot leak a parent invocation reservation.
        return Status::Ok();
    }
    const auto reservation = found->second;
    const auto execution_id =
        reservation.child_execution_id;
    atomic_child_reservations_.erase(found);
    const auto reverse =
        atomic_child_execution_to_reservation_.find(
            execution_id);
    if (reverse !=
            atomic_child_execution_to_reservation_.end() &&
        reverse->second == reservation_id) {
        atomic_child_execution_to_reservation_.erase(reverse);
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
        snapshot.side_effect_state =
            SideEffectState::Unknown;
        snapshot.error_code =
            "DISPATCH_PARENT_LEASE_CHANGED_DURING_CHILD_INVOCATION";
        setStateUnlocked(snapshot, DispatchState::Unknown);
        emit(snapshot, "UNKNOWN", false, false);
    }
    applyDeferredParentTransitionUnlocked(
        reservation.parent_dispatch_id);
    return Status::Ok();
}

// Validate a SubAgent model job against the selected live AgentLease.

}  // namespace master_agent::agent_dispatch
