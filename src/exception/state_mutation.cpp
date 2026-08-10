/**
 * @file state_mutation.cpp
 * @brief Applies durable acknowledge, mitigating, and resolve transitions.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

Result<ExceptionMutationResult> ExceptionManager::mutate(
    const ExceptionMutationRequest& request, ExceptionLifecycle target,
    const CallContext& call) {

    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) {
        return Result<ExceptionMutationResult>::Failure(caller);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_) {
        return Result<ExceptionMutationResult>::Failure(
            initialization_status_);
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_CALL_EXPIRED",
            "a live monotonic deadline is required"));
    }
    const bool bounded_fields =
        safeExceptionReference(request.mutation_id, 256) &&
        safeExceptionReference(request.exception_id, 256) &&
        safeExceptionReference(request.actor_id_hash, 256) &&
        safeExceptionReference(request.actor_role, 128) &&
        safeExceptionReference(request.reason_code, 256) &&
        safeExceptionReference(
            request.resolution_waiver_id, 256, true) &&
        safeExceptionReference(call.request_id, 256) &&
        safeExceptionReference(call.trace_id, 256) &&
        request.verification_evidence_refs.size() <= 32 &&
        std::all_of(
            request.verification_evidence_refs.begin(),
            request.verification_evidence_refs.end(),
            [](const std::string& evidence) {
                return safeExceptionReference(
                    evidence, 512);
            });
    if (request.mutation_id.empty() ||
        request.exception_id.empty() ||
        request.actor_id_hash.empty() ||
        request.actor_role.empty() ||
        request.reason_code.empty() || !bounded_fields) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_MUTATION_INVALID",
            "lifecycle mutation requires bounded actor and reason fields"));
    }
    if (target == ExceptionLifecycle::Resolved &&
        request.verification_evidence_refs.empty() &&
        request.resolution_waiver_id.empty()) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_RESOLUTION_EVIDENCE_REQUIRED",
            "resolution requires verification evidence or a policy waiver"));
    }
    const auto digest = mutationDigest(request, target);
    const auto tx_id = transactionId(
        "mutation", request.mutation_id, digest);
    const auto replay =
        mutation_results_.find(request.mutation_id);
    if (replay != mutation_results_.end()) {
        if (mutation_digests_.at(request.mutation_id) != digest) {
            return Result<ExceptionMutationResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_MUTATION_IDEMPOTENCY_CONFLICT",
                    "mutation_id was reused with different content"));
        }
        return Result<ExceptionMutationResult>::Success(replay->second);
    }
    const auto fenced =
        fenced_transaction_failures_.find(tx_id);
    if (fenced != fenced_transaction_failures_.end()) {
        if (fenced_transaction_digests_.at(tx_id) != digest) {
            return Result<ExceptionMutationResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_MUTATION_IDEMPOTENCY_CONFLICT",
                    "fenced mutation was reused with different content"));
        }
        return Result<ExceptionMutationResult>::Failure(fenced->second);
    }
    if (journal_fenced_) {
        return Result<ExceptionMutationResult>::Failure(
            durabilityUnknown(
                "exception journal is fenced pending startup recovery"));
    }
    if (journal_commit_inflight_) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_WRITER_BUSY",
            "another exception journal transaction is in progress", true));
    }
    const auto found = groups_.find(request.exception_id);
    if (found == groups_.end()) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_INVALID_ARGUMENT",
            "mutation id or exception id is invalid"));
    }
    if (found->second.version != request.expected_group_version) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXM_VERSION_CONFLICT",
            "expected group version does not match"));
    }
    if (!validLifecycleTransition(
            found->second.lifecycle, target)) {
        return Result<ExceptionMutationResult>::Failure(Status::Error(
            "exception", "EXCEPTION_LIFECYCLE_REGRESSION",
            "lifecycle mutation is not an allowed state edge"));
    }

    auto next_group = found->second;
    const bool changed = next_group.lifecycle != target;
    next_group.lifecycle = target;
    if (changed) ++next_group.version;
    ExceptionMutationResult result{changed, next_group};

    data_log::LogEvent event;
    event.event_id =
        "exception-mutation-event-" +
        secureDigest(tx_id).substr(0, 32);
    event.event_type = "EXCEPTION_LIFECYCLE_MUTATED";
    event.module = "ExceptionManager";
    event.interface_name = "IExceptionManager.mutate";
    event.operation = request.reason_code;
    event.context.request_id = call.request_id;
    event.context.trace_id = call.trace_id;
    event.context.producer_endpoint_id = "ExceptionManager";
    event.context.producer_epoch = writer_epoch_;
    event.context.producer_sequence = observation_sequence_ + 1;
    event.context.task_priority = call.priority;
    event.context.deadline_mono_ns = call.deadline_mono_ns;
    event.old_state = std::to_string(
        static_cast<std::uint8_t>(found->second.lifecycle));
    event.new_state =
        std::to_string(static_cast<std::uint8_t>(target));
    event.outcome = changed ? "changed" : "unchanged";
    event.occurred_at_utc_ms = clock_->utcNowMs();
    event.occurred_at_mono_ns = clock_->monotonicNowNs();
    event.requested_durability =
        data_log::DurabilityClass::D3Fsynced;
    event.payload_summary_json =
        Json{{"exception_id", request.exception_id},
             {"actor_id_hash", request.actor_id_hash},
             {"actor_role", request.actor_role},
             {"reason_code", request.reason_code},
             {"verification_evidence_refs",
              request.verification_evidence_refs},
             {"resolution_waiver_id",
              request.resolution_waiver_id},
             {"transaction_id", tx_id}}
            .dump();
    data_log::LogEventBatch observation;
    observation.batch_id = "exception-observation:" + tx_id;
    observation.producer_endpoint_id = "ExceptionManager";
    observation.producer_epoch = writer_epoch_;
    observation.first_sequence = event.context.producer_sequence;
    observation.last_sequence = event.context.producer_sequence;
    observation.checksum = secureDigest(tx_id + "|" + digest);
    observation.records = {event};

    const Json transaction{
        {"kind", "mutation"},
        {"transaction_id", tx_id},
        {"idempotency_key", request.mutation_id},
        {"request_digest", digest},
        {"journal_sequence", journal_sequence_ + 1},
        {"writer_epoch", writer_epoch_},
        {"durability",
         static_cast<std::uint8_t>(
             data_log::DurabilityClass::D3Fsynced)},
        {"committed_at_utc_ms", clock_->utcNowMs()},
        {"group_after", groupToJson(next_group)},
        {"changed", changed},
        {"observation", batchToJson(observation)}};
    const auto transaction_payload = transaction.dump();
    journal_commit_inflight_ = true;
    lock.unlock();
    const auto persisted = persistTransactionUnlocked(
        transaction_payload,
        data_log::DurabilityClass::D3Fsynced);
    lock.lock();
    journal_commit_inflight_ = false;
    if (!persisted.ok) {
        if (persisted.error.side_effect_state ==
            SideEffectState::Unknown) {
            fenced_transaction_digests_[tx_id] = digest;
            fenced_transaction_failures_[tx_id] = persisted;
            journal_fenced_ = true;
        }
        return Result<ExceptionMutationResult>::Failure(persisted);
    }

    groups_[request.exception_id] = next_group;
    mutation_digests_[request.mutation_id] = digest;
    mutation_results_[request.mutation_id] = result;
    ++journal_sequence_;
    ++observation_sequence_;
    PendingObservation pending;
    pending.batch_digest =
        secureDigest(batchToJson(observation).dump());
    pending.source_transaction_id = tx_id;
    pending.durability = data_log::DurabilityClass::D3Fsynced;
    pending.batch = std::move(observation);
    pending_observations_.push_back(std::move(pending));
    lock.unlock();
    drainPendingObservations(call);
    return Result<ExceptionMutationResult>::Success(std::move(result));
}


}  // namespace master_agent::exception
