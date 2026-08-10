/**
 * @file inference_events.cpp
 * @brief Publishes versioned inference lifecycle events.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

void InferenceFramework::emit(Job& job, const std::string& event_type,
                                 bool resource_released) {
    InferenceEvent event;
    event.event_id = ids_->next("inference-event");
    event.event_type = event_type;
    event.job_id = job.snapshot.job_id;
    event.attempt_id = job.snapshot.attempt_id;
    event.operation_id = job.snapshot.operation_id;
    event.request_id = job.request.request_id;
    event.parent_operation_id =
        job.request.parent_operation_id;
    event.state = job.snapshot.state;
    event.stage = job.snapshot.stage;
    event.priority = job.snapshot.effective_priority;
    event.replica_id = !job.snapshot.replica_id.empty()
                           ? job.snapshot.replica_id
                           : (job.snapshot.result
                                  ? job.snapshot.result->replica_id
                                  : std::string{});
    event.replica_epoch = job.snapshot.replica_epoch;
    event.lease_id = job.snapshot.lease_id;
    event.fencing_token = job.snapshot.fencing_token;
    event.checkpoint_ref = job.snapshot.checkpoint_ref;
    event.result = job.snapshot.result;
    event.last_error = job.snapshot.last_error;
    event.resource_released = resource_released;
    event.occurred_at_utc_ms = clock_->utcNowMs();
    event.trace_id = job.request.trace_id;
    events_.push_back(std::move(event));

    // A terminal job has nothing further to stream. Dropping the sink here --
    // at the single chokepoint every transition passes through -- keeps a
    // finished job from retaining caller-owned state, and means a late chunk
    // from a superseded attempt has no route to a presentation surface.
    if (job.snapshot.state == InferenceJobState::Completed ||
        job.snapshot.state == InferenceJobState::Failed ||
        job.snapshot.state == InferenceJobState::Cancelled) {
        stream_sinks_.erase(job.snapshot.job_id);
    }
}


}  // namespace master_agent::inference
