/**
 * @file global_arbiter.cpp
 * @brief Selects runnable jobs, applies preemption policy, and pumps work.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

bool InferenceFramework::pumpOne() {
    bool progressed = false;

    std::optional<std::string> release_job_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto eligible = [](const auto& pair) {
            return pair.second.kv_lease &&
                   pair.second.external_operation ==
                       ExternalOperation::None;
        };
        const auto candidate =
            jobs_.upper_bound(kv_cleanup_cursor_);
        for (auto it = candidate; it != jobs_.end(); ++it) {
            if (eligible(*it)) {
                release_job_id = it->first;
                break;
            }
        }
        if (!release_job_id) {
            for (auto it = jobs_.begin(); it != candidate; ++it) {
                if (eligible(*it)) {
                    release_job_id = it->first;
                    break;
                }
            }
        }
        if (release_job_id) kv_cleanup_cursor_ = *release_job_id;
    }
    if (release_job_id) {
        progressed = releaseKvLease(*release_job_id).ok;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : jobs_) {
            auto& job = pair.second;
            if ((job.snapshot.state == InferenceJobState::Accepted ||
                 job.snapshot.state == InferenceJobState::Ready ||
                 job.snapshot.state == InferenceJobState::Running ||
                 job.snapshot.state == InferenceJobState::Suspended) &&
                deadlineExpired(job.snapshot.deadline_mono_ns, *clock_)) {
                const bool external_owns_replica =
                    job.external_operation ==
                        ExternalOperation::KvAcquire ||
                    job.external_operation ==
                        ExternalOperation::RuntimeInfer;
                if (!external_owns_replica &&
                    !job.snapshot.replica_id.empty()) {
                    const auto occupied =
                        replica_to_job_.find(job.snapshot.replica_id);
                    if (occupied != replica_to_job_.end() &&
                        occupied->second == job.snapshot.job_id) {
                        replica_to_job_.erase(occupied);
                    }
                }
                if (!external_owns_replica) {
                    job.snapshot.replica_id.clear();
                }
                job.snapshot.state = InferenceJobState::Failed;
                job.snapshot.stage =
                    external_owns_replica
                        ? "DEADLINE_PENDING_EXTERNAL"
                        : "DEADLINE_EXPIRED";
                job.snapshot.last_error = StructuredError{
                    "inference", "INFERENCE_DEADLINE_EXPIRED",
                    "inference deadline expired", false,
                    SideEffectState::NotApplicable};
                ++job.snapshot.version;
                emit(job, "FAILED", !job.kv_lease);
                progressed = true;
            }
        }
    }
    if (progressed) return true;

    std::optional<std::string> start_job_id;
    std::optional<std::string> start_replica;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (freeReplica()) {
            for (auto& pair : jobs_) {
                if (pair.second.snapshot.state ==
                        InferenceJobState::Suspended &&
                    pair.second.external_operation ==
                        ExternalOperation::None &&
                    !pair.second.kv_lease) {
                    pair.second.snapshot.state =
                        InferenceJobState::Ready;
                    pair.second.snapshot.stage = "READY_TO_RESUME";
                    pair.second.snapshot.enqueue_sequence =
                        ++enqueue_sequence_;
                    ++pair.second.snapshot.version;
                    emit(pair.second, "READY");
                    progressed = true;
                    break;
                }
            }
        }

        auto ready_id = selectReadyJob();
        if (ready_id) {
            auto replica = freeReplica();
            if (!replica) {
                auto victim = selectPreemptionVictim(
                    jobs_.at(*ready_id)
                        .snapshot.effective_priority);
                if (victim) {
                    auto& victim_job = jobs_.at(*victim);
                    emit(victim_job, "PREEMPT_ACCEPTED");
                    emit(victim_job, "SAFE_POINT_REACHED");
                    const auto occupied = replica_to_job_.find(
                        victim_job.snapshot.replica_id);
                    if (occupied != replica_to_job_.end() &&
                        occupied->second ==
                            victim_job.snapshot.job_id) {
                        replica_to_job_.erase(occupied);
                    }
                    victim_job.snapshot.checkpoint_ref =
                        "checkpoint:" +
                        victim_job.snapshot.attempt_id + ":" +
                        std::to_string(
                            victim_job.remaining_work_units);
                    victim_job.snapshot.replica_id.clear();
                    victim_job.snapshot.state =
                        InferenceJobState::Suspended;
                    victim_job.snapshot.stage = "SUSPENDED";
                    ++victim_job.snapshot.version;
                    emit(victim_job, "SUSPENDED", true);
                    replica = freeReplica();
                    progressed = true;
                }
            }
            if (replica) {
                start_job_id = *ready_id;
                start_replica = *replica;
            }
        }
    }
    if (start_job_id && start_replica) {
        return startJob(*start_job_id, *start_replica) ||
               progressed;
    }

    std::vector<std::string> running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : jobs_) {
            auto& job = pair.second;
            if (job.snapshot.state != InferenceJobState::Running ||
                job.external_operation != ExternalOperation::None ||
                job.kv_lease) {
                continue;
            }
            if (job.remaining_work_units > 0) {
                --job.remaining_work_units;
                progressed = true;
            }
            if (job.remaining_work_units == 0) {
                running.push_back(pair.first);
            }
        }
    }
    for (const auto& job_id : running) {
        progressed = completeJob(job_id) || progressed;
    }
    return progressed;
}

Status InferenceFramework::runUntilIdle(std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps; ++i) {
        if (!pumpOne()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool pending_release = std::any_of(
                jobs_.begin(), jobs_.end(), [](const auto& pair) {
                    return pair.second.kv_lease.has_value();
                });
            if (pending_release) {
                return Status::Error(
                    "inference", "INFERENCE_KV_RELEASE_PENDING",
                    "job still owns a KV lease", true);
            }
            const bool external_inflight = std::any_of(
                jobs_.begin(), jobs_.end(), [](const auto& pair) {
                    return pair.second.external_operation !=
                           ExternalOperation::None;
                });
            if (external_inflight) {
                return Status::Error(
                    "inference",
                    "INFERENCE_EXTERNAL_OPERATION_INFLIGHT",
                    "an unlocked runtime/KV callback is still in flight",
                    true);
            }
            const bool nonterminal = std::any_of(
                jobs_.begin(), jobs_.end(), [](const auto& pair) {
                    const auto state = pair.second.snapshot.state;
                    return state == InferenceJobState::Accepted ||
                           state == InferenceJobState::Ready ||
                           state == InferenceJobState::Running ||
                           state == InferenceJobState::Suspended;
                });
            return nonterminal
                       ? Status::Error(
                             "inference",
                             "INFERENCE_SCHEDULER_STALLED",
                             "non-terminal work cannot currently advance",
                             true)
                       : Status::Ok();
        }
    }
    return Status::Error("inference", "INFERENCE_PUMP_LIMIT",
                         "inference did not become idle within step limit");
}


std::optional<std::string> InferenceFramework::selectReadyJob() const {
    const Job* best = nullptr;
    std::string best_id;
    for (const auto& pair : jobs_) {
        const auto& job = pair.second;
        if (job.snapshot.state != InferenceJobState::Ready ||
            job.external_operation != ExternalOperation::None ||
            job.kv_lease) {
            continue;
        }
        if (!best ||
            std::tie(job.snapshot.effective_priority,
                     job.snapshot.deadline_mono_ns,
                     job.snapshot.enqueue_sequence) <
                std::tie(best->snapshot.effective_priority,
                         best->snapshot.deadline_mono_ns,
                         best->snapshot.enqueue_sequence)) {
            best = &job;
            best_id = pair.first;
        }
    }
    return best ? std::optional<std::string>(best_id) : std::nullopt;
}

std::optional<std::string> InferenceFramework::selectPreemptionVictim(
    TaskPriority arriving) const {

    const Job* victim = nullptr;
    std::string victim_id;
    for (const auto& pair : jobs_) {
        const auto& job = pair.second;
        if (job.snapshot.state != InferenceJobState::Running ||
            job.external_operation != ExternalOperation::None ||
            job.kv_lease ||
            !isHigherPriority(arriving,
                              job.snapshot.effective_priority)) {
            continue;
        }
        if (!victim ||
            std::tie(job.snapshot.effective_priority,
                     job.snapshot.started_at_mono_ns) >
                std::tie(victim->snapshot.effective_priority,
                         victim->snapshot.started_at_mono_ns)) {
            victim = &job;
            victim_id = pair.first;
        }
    }
    return victim ? std::optional<std::string>(victim_id) : std::nullopt;
}


std::optional<std::string> InferenceFramework::freeReplica() const {
    for (const auto& replica : replicas_) {
        if (replica_to_job_.count(replica) == 0) {
            return replica;
        }
    }
    return std::nullopt;
}

}  // namespace master_agent::inference
