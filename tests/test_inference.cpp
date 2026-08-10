/**
 * @file test_inference.cpp
 * @brief Verifies inference admission, KV leases, priority, preemption, and fencing.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/common/types.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/kv_cache/kv_cache_manager.h"
#include "master_agent/sub_agents/sub_agent.h"
#include "test_support.h"

using namespace master_agent;
using namespace master_agent::inference;
using master_agent::test_support::expect;

namespace dispatch = master_agent::agent_dispatch;
namespace sub = master_agent::sub_agents;

namespace {

/// A bounded probe turns a would-be self-deadlock into a deterministic test
/// failure. The worker only needs to enter events(), i.e. mutex_.
bool probeFrameworkLock(InferenceFramework* framework) {
    if (!framework) return true;
    auto completion = std::make_shared<std::promise<void>>();
    auto ready = completion->get_future();
    std::thread([framework, completion]() {
        (void)framework->events();
        try {
            completion->set_value();
        } catch (...) {
        }
    }).detach();
    return ready.wait_for(std::chrono::milliseconds(500)) ==
           std::future_status::ready;
}

class ReentrantRuntime final : public IModelRuntime {
public:
    explicit ReentrantRuntime(std::uint32_t work_units)
        : delegate_(work_units) {}

    void bind(InferenceFramework* framework) {
        framework_ = framework;
    }

    std::uint32_t requiredWorkUnits(
        const InferenceRequest& request) const override {
        ++sizing_calls_;
        if (!probeFrameworkLock(framework_)) probe_failed_ = true;
        return delegate_.requiredWorkUnits(request);
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        ++infer_calls_;
        if (!probeFrameworkLock(framework_)) probe_failed_ = true;
        return delegate_.infer(request, seal);
    }

    bool probesPassed() const { return !probe_failed_.load(); }
    int sizingCalls() const { return sizing_calls_.load(); }
    int inferCalls() const { return infer_calls_.load(); }

private:
    MockModelRuntime delegate_;
    InferenceFramework* framework_ = nullptr;
    mutable std::atomic<bool> probe_failed_{false};
    mutable std::atomic<int> sizing_calls_{0};
    std::atomic<int> infer_calls_{0};
};

/// A runtime that performs one same-thread cancellation from inside infer().
/// This reproduces the late-result race without requiring a real model.
class CancellingRuntime final : public IModelRuntime {
public:
    void bind(InferenceFramework* framework) {
        framework_ = framework;
    }

    std::uint32_t requiredWorkUnits(
        const InferenceRequest&) const override {
        return 1;
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        if (!cancelled_once_.exchange(true) && framework_) {
            CallContext control{
                CallerModuleId::AgentService, request.request_id,
                request.trace_id, request.admission.principal_id,
                TaskPriority::P0, request.deadline_mono_ns, {}, 0,
                "trusted-safety:test-control"};
            cancel_status_ = framework_->cancelInference(
                request.job_id, 1, control);
        }
        return delegate_.infer(request, seal);
    }

    bool cancelSucceeded() const { return cancel_status_.ok; }

private:
    MockModelRuntime delegate_{1};
    InferenceFramework* framework_ = nullptr;
    std::atomic<bool> cancelled_once_{false};
    Status cancel_status_ = Status::Error(
        "test", "CANCEL_NOT_CALLED", "runtime did not cancel");
};

/// Advances the deterministic clock while infer() is outside the framework
/// lock, exercising the post-runtime absolute-deadline fence.
class DeadlineAdvancingRuntime final : public IModelRuntime {
public:
    explicit DeadlineAdvancingRuntime(
        std::shared_ptr<ManualRuntimeClock> clock)
        : clock_(std::move(clock)) {}

    std::uint32_t requiredWorkUnits(
        const InferenceRequest&) const override {
        return 1;
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        clock_->advanceMs(2000);
        return delegate_.infer(request, seal);
    }

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
    MockModelRuntime delegate_{1};
};

/// Returns a cryptographically self-consistent envelope carrying one stale
/// invocation field. The framework must compare the echoed seal, not merely
/// trust output_digest.
class SealTamperingRuntime final : public IModelRuntime {
public:
    enum class Field {
        ReplicaEpoch,
        Lease,
        Fence,
        Operation
    };

    explicit SealTamperingRuntime(Field field) : field_(field) {}

    std::uint32_t requiredWorkUnits(
        const InferenceRequest&) const override {
        return 1;
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        auto output = delegate_.infer(request, seal);
        if (!output.status.ok || !output.value) return output;
        switch (field_) {
            case Field::ReplicaEpoch:
                ++output.value->replica_epoch;
                break;
            case Field::Lease:
                output.value->lease_id += "-stale";
                break;
            case Field::Fence:
                ++output.value->fencing_token;
                break;
            case Field::Operation:
                output.value->operation_id += "-stale";
                break;
        }
        output.value->output_digest =
            inferenceOutputDigest(*output.value);
        return output;
    }

private:
    Field field_;
    MockModelRuntime delegate_{1};
};

/// Produces a seal-correct but contract-hostile Runtime result. This verifies
/// that the framework bounds actual bytes/text metadata rather than trusting
/// generated_token_count or an attacker-supplied output digest.
class MaliciousOutputRuntime final : public IModelRuntime {
public:
    enum class Mutation {
        OversizedRawOutput,
        InvalidUtf8,
        ControlCharacter,
        OversizedMetadata
    };

    explicit MaliciousOutputRuntime(Mutation mutation)
        : mutation_(mutation) {}

    std::uint32_t requiredWorkUnits(
        const InferenceRequest&) const override {
        return 1;
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        auto output = delegate_.infer(request, seal);
        if (!output.status.ok || !output.value) {
            return output;
        }
        switch (mutation_) {
            case Mutation::OversizedRawOutput:
                // Default admission permits 1024 output tokens, whose default
                // wire cap is 20 KiB. Keep the claimed count tiny while the
                // actual payload exceeds that cap.
                output.value->raw_output.assign(32U * 1024U, 'x');
                output.value->generated_token_count = 1;
                break;
            case Mutation::InvalidUtf8:
                output.value->raw_output =
                    std::string("\xC3\x28", 2);
                output.value->generated_token_count = 1;
                break;
            case Mutation::ControlCharacter:
                output.value->raw_output =
                    std::string("visible") +
                    static_cast<char>(0x01);
                output.value->generated_token_count = 1;
                break;
            case Mutation::OversizedMetadata:
                output.value->runtime_backend.assign(129U, 'm');
                break;
        }
        // It remains non-empty and bounded. Contract validation must reject
        // the hostile field before trusting this self-asserted digest.
        output.value->output_digest = "attacker-asserted-digest";
        return output;
    }

private:
    Mutation mutation_;
    MockModelRuntime delegate_{1};
};

/// Synthetic valid-HIT KV service with per-job completion failure policy.
/// It lets the test create several simultaneous pending leases without
/// depending on a particular physical Replica's previously published cache.
class RotatingFailureKvCache final
    : public kv_cache::IKvCacheManager {
public:
    void failForever(const std::string& job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        permanent_failures_.insert(job_id);
    }

    void failTimes(const std::string& job_id,
                   std::size_t failure_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_remaining_[job_id] = failure_count;
    }

    std::vector<kv_cache::KvCacheUseReport> completionReports()
        const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completion_reports_;
    }

    Result<kv_cache::KvCacheAcquireResult> acquire(
        const kv_cache::KvCacheAcquireRequest& request,
        const CallContext& call) override {
        if (request.replica_candidates.empty() ||
            request.segments.empty()) {
            return Result<kv_cache::KvCacheAcquireResult>::Failure(
                Status::Error("test_kv", "TEST_KV_INVALID_ACQUIRE",
                              "test acquire has no Replica or segment"));
        }
        const auto& replica = request.replica_candidates.front();
        const auto epoch =
            request.replica_candidate_epochs.find(replica);
        if (epoch ==
            request.replica_candidate_epochs.end()) {
            return Result<kv_cache::KvCacheAcquireResult>::Failure(
                Status::Error("test_kv", "TEST_KV_INVALID_EPOCH",
                              "test acquire has no Replica epoch"));
        }
        std::uint32_t cached_tokens = 0;
        for (const auto& segment : request.segments) {
            cached_tokens += segment.token_count;
        }

        kv_cache::KvCacheLease lease;
        lease.lease_id = "synthetic-lease-" + request.job_id;
        lease.job_id = request.job_id;
        lease.request_id = request.request_id;
        lease.trace_id = call.trace_id;
        lease.principal_id_hash = call.principal_id_hash;
        lease.priority = request.priority;
        lease.binding.cache_id =
            "synthetic-cache-" + request.job_id;
        lease.binding.lease_id = lease.lease_id;
        lease.binding.runtime_cache_handle =
            "synthetic-handle-" + request.job_id;
        lease.binding.replica_id = replica;
        lease.binding.replica_epoch = epoch->second;
        lease.binding.cached_token_count = cached_tokens;
        lease.binding.fingerprint_digest =
            request.runtime_fingerprint_digest;
        lease.expires_at_mono_ns = request.deadline_mono_ns;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_leases_.insert(lease.lease_id);
        }
        kv_cache::KvCacheAcquireResult result;
        result.outcome = kv_cache::AcquireOutcome::Hit;
        result.lease = std::move(lease);
        result.matched_segment_count = request.segments.size();
        result.reason_code = "SYNTHETIC_HIT";
        return Result<kv_cache::KvCacheAcquireResult>::Success(
            std::move(result));
    }

    Result<kv_cache::KvCachePublishResult> publish(
        const kv_cache::KvCachePublishRequest& request,
        const CallContext&) override {
        kv_cache::KvCachePublishResult result;
        result.admitted = true;
        result.cache_id = "synthetic-publish-" + request.job_id;
        return Result<kv_cache::KvCachePublishResult>::Success(
            std::move(result));
    }

    Status completeUse(
        const kv_cache::KvCacheUseReport& report,
        const CallContext&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        completion_reports_.push_back(report);
        auto remaining =
            failures_remaining_.find(report.job_id);
        const bool transient_failure =
            remaining != failures_remaining_.end() &&
            remaining->second > 0U;
        if (transient_failure) {
            --remaining->second;
        }
        if (permanent_failures_.count(report.job_id) != 0U ||
            transient_failure) {
            return Status::Error(
                "test_kv", "TEST_KV_COMPLETE_UNAVAILABLE",
                "synthetic completeUse failure", true,
                SideEffectState::Unknown);
        }
        active_leases_.erase(report.lease_id);
        return Status::Ok();
    }

    Result<kv_cache::KvCacheInvalidateResult> invalidate(
        const kv_cache::KvCacheInvalidateRequest&,
        const CallContext&) override {
        return Result<kv_cache::KvCacheInvalidateResult>::Success(
            kv_cache::KvCacheInvalidateResult{});
    }

    kv_cache::KvCacheManagerStatus queryStatus(
        const CallContext&) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        kv_cache::KvCacheManagerStatus status;
        status.active_leases = active_leases_.size();
        return status;
    }

private:
    mutable std::mutex mutex_;
    std::set<std::string> permanent_failures_;
    std::map<std::string, std::size_t> failures_remaining_;
    std::set<std::string> active_leases_;
    std::vector<kv_cache::KvCacheUseReport>
        completion_reports_;
};

class ReentrantKvCache final : public kv_cache::IKvCacheManager {
public:
    explicit ReentrantKvCache(
        std::shared_ptr<kv_cache::IKvCacheManager> delegate)
        : delegate_(std::move(delegate)) {}

    void bind(InferenceFramework* framework) {
        framework_ = framework;
    }

    void failNextCompleteAfterApply() {
        fail_next_complete_ = true;
    }

    std::vector<kv_cache::KvCacheUseReport> completeReports() const {
        std::lock_guard<std::mutex> lock(reports_mutex_);
        return complete_reports_;
    }

    bool probesPassed() const { return !probe_failed_.load(); }

    Result<kv_cache::KvCacheAcquireResult> acquire(
        const kv_cache::KvCacheAcquireRequest& request,
        const CallContext& call) override {
        probe();
        return delegate_->acquire(request, call);
    }

    Result<kv_cache::KvCachePublishResult> publish(
        const kv_cache::KvCachePublishRequest& request,
        const CallContext& call) override {
        probe();
        return delegate_->publish(request, call);
    }

    Status completeUse(
        const kv_cache::KvCacheUseReport& report,
        const CallContext& call) override {
        probe();
        {
            std::lock_guard<std::mutex> lock(reports_mutex_);
            complete_reports_.push_back(report);
        }
        const auto applied = delegate_->completeUse(report, call);
        if (applied.ok && fail_next_complete_.exchange(false)) {
            return Status::Error(
                "kv_cache", "KV_COMPLETE_RESPONSE_TIMEOUT",
                "completion applied but response was lost", true,
                SideEffectState::Unknown);
        }
        return applied;
    }

    Result<kv_cache::KvCacheInvalidateResult> invalidate(
        const kv_cache::KvCacheInvalidateRequest& request,
        const CallContext& call) override {
        return delegate_->invalidate(request, call);
    }

    kv_cache::KvCacheManagerStatus queryStatus(
        const CallContext& call) const override {
        return delegate_->queryStatus(call);
    }

private:
    void probe() {
        if (!probeFrameworkLock(framework_)) probe_failed_ = true;
    }

    std::shared_ptr<kv_cache::IKvCacheManager> delegate_;
    InferenceFramework* framework_ = nullptr;
    std::atomic<bool> probe_failed_{false};
    std::atomic<bool> fail_next_complete_{false};
    mutable std::mutex reports_mutex_;
    std::vector<kv_cache::KvCacheUseReport> complete_reports_;
};

/// Cancels the inference after the real KV manager has granted a HIT lease
/// but before acquire() returns to the framework. This is the narrow race
/// where cancellation, late acquire output and lease ownership overlap.
class CancellingAcquireKvCache final
    : public kv_cache::IKvCacheManager {
public:
    explicit CancellingAcquireKvCache(
        std::shared_ptr<kv_cache::IKvCacheManager> delegate)
        : delegate_(std::move(delegate)) {}

    void arm(InferenceFramework* framework,
             const std::string& job_id) {
        framework_ = framework;
        target_job_id_ = job_id;
    }

    bool cancelSucceeded() const { return cancel_status_.ok; }
    bool observedActiveLease() const {
        return observed_active_lease_;
    }

    Result<kv_cache::KvCacheAcquireResult> acquire(
        const kv_cache::KvCacheAcquireRequest& request,
        const CallContext& call) override {
        auto result = delegate_->acquire(request, call);
        if (!cancelled_ && framework_ &&
            request.job_id == target_job_id_ &&
            result.status.ok && result.value && result.value->lease) {
            cancelled_ = true;
            observed_active_lease_ =
                delegate_->queryStatus(call).active_leases == 1;
            CallContext control{
                CallerModuleId::AgentService, request.request_id,
                "trace-" + request.job_id, "principal",
                TaskPriority::P0, request.deadline_mono_ns, {}, 0,
                "trusted-safety:test-control"};
            cancel_status_ = framework_->cancelInference(
                request.job_id, 1, control);
        }
        return result;
    }

    Result<kv_cache::KvCachePublishResult> publish(
        const kv_cache::KvCachePublishRequest& request,
        const CallContext& call) override {
        return delegate_->publish(request, call);
    }

    Status completeUse(
        const kv_cache::KvCacheUseReport& report,
        const CallContext& call) override {
        return delegate_->completeUse(report, call);
    }

    Result<kv_cache::KvCacheInvalidateResult> invalidate(
        const kv_cache::KvCacheInvalidateRequest& request,
        const CallContext& call) override {
        return delegate_->invalidate(request, call);
    }

    kv_cache::KvCacheManagerStatus queryStatus(
        const CallContext& call) const override {
        return delegate_->queryStatus(call);
    }

private:
    std::shared_ptr<kv_cache::IKvCacheManager> delegate_;
    InferenceFramework* framework_ = nullptr;
    std::string target_job_id_;
    bool cancelled_ = false;
    bool observed_active_lease_ = false;
    Status cancel_status_ = Status::Error(
        "test", "CANCEL_NOT_CALLED",
        "KV acquire callback did not cancel inference");
};

InferenceRequest request(const std::string& id, TaskPriority priority,
                         bool p0_authorized, std::int64_t deadline) {
    InferenceRequest value;
    value.job_id = id;
    value.request_id = "request-" + id;
    value.session_id = "session";
    value.prompt = u8"请规划行程 " + id;
    value.prompt_digest = secureDigest(value.prompt);
    value.prompt_segments = {
        {"user", value.prompt_digest, 8}};
    value.priority = priority;
    value.deadline_mono_ns = deadline;
    value.idempotency_key = "idem-" + id;
    value.trace_id = "trace-" + id;
    value.admission.principal_id = "principal";
    value.admission.caller_module_id =
        CallerModuleId::IntentRecognitionEngine;
    value.admission.source_request_id = value.request_id;
    value.admission.granted_priority = priority;
    value.admission.p0_authorization = p0_authorized;
    value.admission.policy_snapshot_id = "policy";
    value.admission.deadline_mono_ns = deadline;
    if (p0_authorized) {
        value.admission.signature_ref =
            "trusted-safety:test-grant:" + id;
    }
    return value;
}

CallContext intentCall(const InferenceRequest& value) {
    return {CallerModuleId::IntentRecognitionEngine,
            value.request_id, value.trace_id,
            value.admission.principal_id, value.priority,
            value.deadline_mono_ns, {}, 0,
            value.admission.signature_ref};
}

/// Blocks inside the physical model boundary so the test can inspect the
/// parent AgentLease while its inference reservation is live.
///
class BlockingInferenceRuntime final : public IModelRuntime {
public:
    explicit BlockingInferenceRuntime(bool throw_after_release = false)
        : throw_after_release_(throw_after_release) {}

    std::uint32_t requiredWorkUnits(
        const InferenceRequest&) const override {
        ++sizing_calls_;
        return 1;
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++infer_calls_;
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return released_; });
        }
        if (throw_after_release_) {
            throw std::runtime_error(
                "injected model runtime failure");
        }
        return delegate_.infer(request, seal);
    }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, std::chrono::seconds(2),
            [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    int sizingCalls() const { return sizing_calls_.load(); }
    int inferCalls() const { return infer_calls_.load(); }

private:
    MockModelRuntime delegate_{1};
    bool throw_after_release_ = false;
    mutable std::atomic<int> sizing_calls_{0};
    std::atomic<int> infer_calls_{0};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

/// Records the reservation protocol while delegating every authority decision
/// to the real AgentDispatch implementation.
///
class CountingInferenceLineageValidator final
    : public IInferenceParentLineageValidator {
public:
    explicit CountingInferenceLineageValidator(
        std::shared_ptr<dispatch::AgentDispatch> delegate)
        : delegate_(std::move(delegate)) {}

    Status validateInferenceParentLineage(
        const InferenceRequest& request,
        const CallContext& call) const override {
        ++validate_calls_;
        return delegate_->validateInferenceParentLineage(
            request, call);
    }

    Result<std::string> acquireInferenceParentInvocationLease(
        const InferenceRequest& request,
        const CallContext& call) override {
        ++acquire_calls_;
        auto acquired =
            delegate_->acquireInferenceParentInvocationLease(
                request, call);
        if (acquired.status.ok && acquired.value) {
            std::lock_guard<std::mutex> lock(mutex_);
            acquired_ids_.push_back(*acquired.value);
        }
        return acquired;
    }

    Status releaseInferenceParentInvocationLease(
        const std::string& reservation_id) override {
        ++release_calls_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ids_.push_back(reservation_id);
        }
        return delegate_->releaseInferenceParentInvocationLease(
            reservation_id);
    }

    int validateCalls() const {
        return validate_calls_.load();
    }

    int acquireCalls() const {
        return acquire_calls_.load();
    }

    int releaseCalls() const {
        return release_calls_.load();
    }

    std::vector<std::string> acquiredIds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return acquired_ids_;
    }

    std::vector<std::string> releasedIds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return released_ids_;
    }

private:
    std::shared_ptr<dispatch::AgentDispatch> delegate_;
    mutable std::atomic<int> validate_calls_{0};
    std::atomic<int> acquire_calls_{0};
    std::atomic<int> release_calls_{0};
    mutable std::mutex mutex_;
    std::vector<std::string> acquired_ids_;
    std::vector<std::string> released_ids_;
};

sub::AgentManifest inferenceParentManifest(
    const std::string& suffix) {
    sub::AgentManifest manifest;
    manifest.agent_id = "inference-parent-agent-" + suffix;
    manifest.agent_epoch = 7;
    manifest.manifest_digest = secureDigest(
        manifest.agent_id + "|7|plan_trip|model-v2");
    manifest.capability_version = "contract-v2";
    manifest.capabilities = {"plan_trip"};
    manifest.max_concurrency = 1;
    manifest.supports_safe_point_preemption = true;
    manifest.model_profile_id = "model-v2-" + suffix;
    return manifest;
}

dispatch::DispatchTask inferenceParentTask(
    const std::shared_ptr<ManualRuntimeClock>& clock,
    const sub::AgentManifest& manifest,
    const std::string& suffix) {
    dispatch::DispatchTask task;
    task.request_id = "request-parent-" + suffix;
    task.plan_id = "plan-parent-" + suffix;
    task.pid = "pid-parent-" + suffix;
    task.activation_id = "activation-parent-" + suffix;
    task.execution_id = "execution-parent-" + suffix;
    task.operation_id = "operation-parent-" + suffix;
    task.task_id = "task-parent-" + suffix;
    task.action = "plan_trip";
    task.target_agent = manifest.agent_id;
    task.params = nlohmann::json{
        {"destination", "lineage-test"}};
    task.priority = TaskPriority::P2;
    task.deadline_mono_ns =
        clock->monotonicNowNs() + 60'000'000'000LL;
    task.idempotency_key = "idem-parent-" + suffix;
    task.fencing_token = 17;
    task.capability_digest =
        dispatch::dispatchCapabilityDigest(
            task.action, task.input_schema_version,
            task.expected_output_schema_version);
    task.capacity_epoch = 0;
    task.principal_id_hash = "principal-parent-" + suffix;
    task.authorization_ref = "authorization:parent-" + suffix;
    task.trace_id = "trace-parent-" + suffix;
    return task;
}

CallContext inferenceDispatchCall(
    const dispatch::DispatchTask& task) {
    return {CallerModuleId::TaskOrchestrationEngine,
            task.request_id, task.trace_id,
            task.principal_id_hash, task.priority,
            task.deadline_mono_ns, {}, 0,
            task.authorization_ref};
}

/// A real Running Dispatch is the trust root for every child-inference test.
///
struct RunningInferenceParent {
    explicit RunningInferenceParent(const std::string& suffix)
        : clock(std::make_shared<ManualRuntimeClock>()),
          ids(std::make_shared<IdGenerator>(
              "inference-parent-" + suffix)),
          scheduler(
              std::make_shared<dispatch::AgentDispatch>(
                  clock, ids)),
          manifest(inferenceParentManifest(suffix)),
          task(inferenceParentTask(clock, manifest, suffix)) {
        auto provider =
            std::make_shared<sub::DeterministicSubAgent>(
                manifest, clock, ids, 100);
        CallContext bootstrap{
            CallerModuleId::AgentService,
            "bootstrap-" + suffix,
            "trace-bootstrap-" + suffix,
            task.principal_id_hash, TaskPriority::P1,
            task.deadline_mono_ns};
        expect(scheduler->registerAgent(
                   std::move(provider), bootstrap).ok,
               "parent SubAgent must register");
        const auto capacity =
            scheduler->getCapacity(inferenceDispatchCall(task));
        expect(capacity.capacity_epoch != 0 &&
                   capacity.health_state != "CALLER_NOT_ALLOWED",
               "parent capacity must be read from AgentDispatch");
        task.capacity_epoch = capacity.capacity_epoch;
        acceptance = scheduler->submitDispatch(
            task, inferenceDispatchCall(task));
        expect(acceptance.accepted,
               "parent Dispatch must be accepted");
        expect(scheduler->pumpOne(),
               "parent Dispatch must enter its provider");
        const auto queried = scheduler->queryDispatch(
            acceptance.dispatch_id, queryCall());
        expect(queried.status.ok && queried.value &&
                   queried.value->state ==
                       dispatch::DispatchState::Running,
               "parent Dispatch must be Running");
        snapshot = *queried.value;
    }

    CallContext queryCall() const {
        auto call = inferenceDispatchCall(task);
        call.deadline_mono_ns =
            clock->monotonicNowNs() + 1'000'000'000LL;
        return call;
    }

    CallContext preemptCall() const {
        return {
            CallerModuleId::TaskOrchestrationEngine,
            task.request_id, task.trace_id,
            task.principal_id_hash, TaskPriority::P0,
            task.deadline_mono_ns, {}, 0,
            "trusted-safety:inference-parent-preempt"};
    }

    std::shared_ptr<ManualRuntimeClock> clock;
    std::shared_ptr<IdGenerator> ids;
    std::shared_ptr<dispatch::AgentDispatch> scheduler;
    sub::AgentManifest manifest;
    dispatch::DispatchTask task;
    dispatch::DispatchAcceptance acceptance;
    dispatch::DispatchSnapshot snapshot;
};

InferenceRequest childInferenceRequest(
    const RunningInferenceParent& parent,
    const std::string& suffix) {
    InferenceRequest child;
    child.job_id = "child-inference-" + suffix;
    child.request_id = parent.task.request_id;
    child.parent_operation_id = parent.task.operation_id;
    child.session_id = "session-child-" + suffix;
    child.prompt = "simulate child reasoning for " + suffix;
    child.prompt_digest = secureDigest(child.prompt);
    child.prompt_segments = {
        {"child-user", child.prompt_digest, 8}};
    child.model = parent.manifest.model_profile_id;
    child.priority = parent.task.priority;
    child.deadline_mono_ns = parent.task.deadline_mono_ns;
    child.parent_dispatch_id = parent.snapshot.dispatch_id;
    child.parent_agent_id = parent.snapshot.route.agent_id;
    child.parent_agent_epoch =
        parent.snapshot.route.agent_epoch;
    child.parent_lease_id = parent.snapshot.route.lease_id;
    child.parent_fencing_token =
        parent.snapshot.task.fencing_token;
    child.idempotency_key = inferenceChildIdempotencyKey(
        child.parent_dispatch_id, child.job_id);
    child.trace_id = parent.task.trace_id;
    child.admission.principal_id =
        parent.task.principal_id_hash;
    child.admission.caller_module_id =
        CallerModuleId::SubAgent;
    child.admission.source_request_id = child.request_id;
    child.admission.granted_priority = child.priority;
    child.admission.policy_snapshot_id =
        "policy-child-" + suffix;
    child.admission.allowed_model_profiles = {child.model};
    child.admission.deadline_mono_ns =
        child.deadline_mono_ns;
    child.admission.signature_ref =
        parent.task.authorization_ref;
    return child;
}

CallContext childInferenceCall(
    const RunningInferenceParent& parent,
    const InferenceRequest& child) {
    return {
        CallerModuleId::SubAgent, child.request_id,
        child.trace_id, child.admission.principal_id,
        child.priority, child.deadline_mono_ns, {}, 0,
        parent.task.authorization_ref};
}

/// Submission from a SubAgent without an AgentDispatch lineage validator must
/// stop before sizing or touching the physical model runtime.
///
void testSubAgentInferenceFailsClosedWithoutLineageValidator() {
    RunningInferenceParent parent("no-validator");
    auto runtime =
        std::make_shared<BlockingInferenceRuntime>();
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(
            parent.clock, parent.ids);
    InferenceFramework framework(
        parent.clock, parent.ids, runtime, kv, 1);
    const auto child =
        childInferenceRequest(parent, "no-validator");
    const auto accepted = framework.submitInference(
        child, childInferenceCall(parent, child));
    expect(!accepted.accepted &&
               accepted.reject_code ==
                   "INFERENCE_PARENT_LINEAGE_VALIDATOR_UNAVAILABLE",
           "SubAgent inference must fail closed without lineage validator");
    expect(runtime->sizingCalls() == 0 &&
               runtime->inferCalls() == 0,
           "fail-closed lineage rejection must precede all model calls");
}

/// Every mutable parent-lease coordinate is independently fail-closed.
///
void testSubAgentInferenceRejectsForgedParentLineageMatrix() {
    RunningInferenceParent parent("forgery-matrix");
    auto runtime =
        std::make_shared<MockModelRuntime>(1);
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(
            parent.clock, parent.ids);
    InferenceFramework framework(
        parent.clock, parent.ids, runtime, kv, 1,
        parent.scheduler);
    const auto valid =
        childInferenceRequest(parent, "forgery-matrix");

    struct ForgeryCase {
        std::string label;
        std::function<void(InferenceRequest&)> mutate;
        std::string reject_code;
    };
    const std::vector<ForgeryCase> cases{
        {"parent dispatch",
         [](auto& value) {
             value.parent_dispatch_id += "-forged";
         },
         "INFERENCE_PARENT_LINEAGE_MISMATCH"},
        {"parent agent",
         [](auto& value) {
             value.parent_agent_id += "-forged";
         },
         "INFERENCE_PARENT_LINEAGE_MISMATCH"},
        {"parent agent epoch",
         [](auto& value) {
             ++value.parent_agent_epoch;
         },
         "INFERENCE_PARENT_LINEAGE_MISMATCH"},
        {"parent lease",
         [](auto& value) {
             value.parent_lease_id += "-forged";
         },
         "INFERENCE_PARENT_LINEAGE_MISMATCH"},
        {"parent fence",
         [](auto& value) {
             ++value.parent_fencing_token;
         },
         "INFERENCE_PARENT_LINEAGE_MISMATCH"},
        {"model",
         [](auto& value) {
             value.model += "-forged";
             value.admission.allowed_model_profiles = {
                 value.model};
         },
         "INFERENCE_CHILD_MODEL_PROFILE_NOT_AUTHORIZED"},
        {"idempotency",
         [](auto& value) {
             value.idempotency_key += "-forged";
         },
         "INFERENCE_CHILD_IDEMPOTENCY_KEY_INVALID"}};

    for (const auto& forgery : cases) {
        auto child = valid;
        forgery.mutate(child);
        const auto accepted = framework.submitInference(
            child, childInferenceCall(parent, child));
        expect(!accepted.accepted &&
                   accepted.reject_code ==
                       forgery.reject_code,
               forgery.label +
                   " forgery must be rejected by parent lineage");
    }
}

/// A legitimate child of a live Running Dispatch must complete, pair its
/// reservation, and leave the parent immediately preemptible afterward.
///
void testRunningDispatchChildInferenceCompletesAndReleasesParent() {
    RunningInferenceParent parent("legal-child");
    auto validator =
        std::make_shared<CountingInferenceLineageValidator>(
            parent.scheduler);
    auto runtime =
        std::make_shared<MockModelRuntime>(1);
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(
            parent.clock, parent.ids);
    InferenceFramework framework(
        parent.clock, parent.ids, runtime, kv, 1,
        validator);
    const auto child =
        childInferenceRequest(parent, "legal-child");
    const auto child_call =
        childInferenceCall(parent, child);
    expect(framework.submitInference(
               child, child_call).accepted,
           "valid Running Dispatch child inference must be accepted");
    expect(framework.runUntilIdle().ok,
           "valid child inference must drain");
    const auto completed =
        framework.queryInference(child.job_id, child_call);
    expect(completed.status.ok && completed.value &&
               completed.value->state ==
                   InferenceJobState::Completed &&
               completed.value->result,
           "valid Running Dispatch child inference must complete");
    expect(validator->validateCalls() >= 1 &&
               validator->acquireCalls() == 1 &&
               validator->releaseCalls() == 1 &&
               validator->acquiredIds() ==
                   validator->releasedIds() &&
               !validator->acquiredIds().front().empty(),
           "successful runtime must pair one non-empty parent reservation");

    const auto preempted = parent.scheduler->requestPreempt(
        parent.snapshot.dispatch_id, TaskPriority::P0,
        parent.snapshot.control_epoch + 1,
        parent.preemptCall());
    const auto suspended = parent.scheduler->queryDispatch(
        parent.snapshot.dispatch_id, parent.queryCall());
    expect(preempted.ok && suspended.value &&
               suspended.value->state ==
                   dispatch::DispatchState::Suspended,
           "parent must become preemptible immediately after release");
}

/// Reservation acquisition happens before the physical model call. While the
/// model owns it, AgentDispatch must reject parent preemption; after release
/// the exact same control request succeeds.
///
void testParentLeasePinnedAcrossPhysicalModelInvocation() {
    RunningInferenceParent parent("pinned-runtime");
    auto validator =
        std::make_shared<CountingInferenceLineageValidator>(
            parent.scheduler);
    auto runtime =
        std::make_shared<BlockingInferenceRuntime>();
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(
            parent.clock, parent.ids);
    InferenceFramework framework(
        parent.clock, parent.ids, runtime, kv, 1,
        validator);
    const auto child =
        childInferenceRequest(parent, "pinned-runtime");
    const auto child_call =
        childInferenceCall(parent, child);
    expect(framework.submitInference(
               child, child_call).accepted,
           "pinned child inference must be accepted");

    auto drained = std::async(
        std::launch::async,
        [&framework] { return framework.runUntilIdle(); });
    const bool entered = runtime->waitUntilEntered();
    if (!entered) runtime->release();
    expect(entered,
           "model runtime must enter while parent reservation is live");

    const auto rejected = parent.scheduler->requestPreempt(
        parent.snapshot.dispatch_id, TaskPriority::P0,
        parent.snapshot.control_epoch + 1,
        parent.preemptCall());
    const auto during = parent.scheduler->queryDispatch(
        parent.snapshot.dispatch_id, parent.queryCall());
    const bool reservation_live =
        validator->acquireCalls() == 1 &&
        validator->releaseCalls() == 0;
    runtime->release();
    const auto drain_status = drained.get();

    expect(!rejected.ok &&
               rejected.error.code ==
                   "DISPATCH_CHILD_INVOCATION_ACTIVE" &&
               during.value &&
               during.value->state ==
                   dispatch::DispatchState::Running &&
               reservation_live,
           "live physical inference must pin its Running parent lease");
    expect(drain_status.ok &&
               validator->releaseCalls() == 1 &&
               validator->acquiredIds() ==
                   validator->releasedIds(),
           "normal model completion must release the exact reservation");

    const auto after_release =
        parent.scheduler->requestPreempt(
            parent.snapshot.dispatch_id, TaskPriority::P0,
            parent.snapshot.control_epoch + 1,
            parent.preemptCall());
    const auto suspended = parent.scheduler->queryDispatch(
        parent.snapshot.dispatch_id, parent.queryCall());
    expect(after_release.ok && suspended.value &&
               suspended.value->state ==
                   dispatch::DispatchState::Suspended,
           "preemption must succeed after model reservation release");
}

/// Runtime exceptions are contained as Failed jobs and must still execute the
/// reservation release path exactly once.
///
void testRuntimeExceptionStillReleasesParentReservation() {
    RunningInferenceParent parent("runtime-exception");
    auto validator =
        std::make_shared<CountingInferenceLineageValidator>(
            parent.scheduler);
    auto runtime =
        std::make_shared<BlockingInferenceRuntime>(true);
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(
            parent.clock, parent.ids);
    InferenceFramework framework(
        parent.clock, parent.ids, runtime, kv, 1,
        validator);
    const auto child =
        childInferenceRequest(parent, "runtime-exception");
    const auto child_call =
        childInferenceCall(parent, child);
    expect(framework.submitInference(
               child, child_call).accepted,
           "exception fixture child inference must be accepted");

    auto drained = std::async(
        std::launch::async,
        [&framework] { return framework.runUntilIdle(); });
    const bool entered = runtime->waitUntilEntered();
    const bool held_before_throw =
        entered && validator->acquireCalls() == 1 &&
        validator->releaseCalls() == 0;
    runtime->release();
    const auto drain_status = drained.get();
    const auto failed =
        framework.queryInference(child.job_id, child_call);

    expect(held_before_throw && drain_status.ok &&
               failed.value &&
               failed.value->state ==
                   InferenceJobState::Failed &&
               failed.value->last_error &&
               failed.value->last_error->code ==
                   "INFERENCE_RUNTIME_FAILED",
           "runtime exception must be contained as a failed inference");
    expect(validator->acquireCalls() == 1 &&
               validator->releaseCalls() == 1 &&
               validator->acquiredIds() ==
                   validator->releasedIds(),
           "runtime exception must release the exact parent reservation");

    const auto preempted = parent.scheduler->requestPreempt(
        parent.snapshot.dispatch_id, TaskPriority::P0,
        parent.snapshot.control_epoch + 1,
        parent.preemptCall());
    expect(preempted.ok,
           "parent lease must not remain pinned after runtime exception");
}

void testP0PreemptsP2AndKvLeaseCloses() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-test");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(4);
    InferenceFramework framework(clock, ids, model, kv, 1);
    CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine, "r", "t", "principal",
        TaskPriority::P1, clock->monotonicNowNs() + 10000000000LL};

    auto low = request("job-low", TaskPriority::P2, false,
                       clock->monotonicNowNs() + 9000000000LL);
    intent_call.request_id = low.request_id;
    intent_call.trace_id = low.trace_id;
    expect(framework.submitInference(low, intent_call).accepted,
           "P2 inference must be accepted");
    expect(framework.pumpOne(), "P2 inference must start");
    auto high = request("job-high", TaskPriority::P0, true,
                        clock->monotonicNowNs() + 5000000000LL);
    intent_call.priority = TaskPriority::P0;
    intent_call.request_id = high.request_id;
    intent_call.trace_id = high.trace_id;
    intent_call.authorization_ref =
        high.admission.signature_ref;
    expect(framework.submitInference(high, intent_call).accepted,
           "P0 inference must be accepted");
    expect(framework.runUntilIdle().ok,
           "inference scheduler must drain");
    auto low_query = intent_call;
    low_query.request_id = low.request_id;
    low_query.trace_id = low.trace_id;
    low_query.principal_id_hash = low.admission.principal_id;
    low_query.priority = low.priority;
    low_query.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000'000LL;
    low_query.authorization_ref.clear();
    auto high_query = intent_call;
    high_query.request_id = high.request_id;
    high_query.trace_id = high.trace_id;
    high_query.principal_id_hash = high.admission.principal_id;
    high_query.priority = high.priority;
    high_query.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000'000LL;
    high_query.authorization_ref =
        high.admission.signature_ref;
    const auto low_state = framework.queryInference("job-low", low_query);
    const auto high_state = framework.queryInference("job-high", high_query);
    expect(low_state.value &&
               low_state.value->state == InferenceJobState::Completed &&
               high_state.value &&
               high_state.value->state == InferenceJobState::Completed,
           "P0 and resumed P2 inference must complete");
    const auto events = framework.events();
    expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
               return event.job_id == "job-low" &&
                      event.event_type == "SUSPENDED" &&
                      event.resource_released;
           }),
           "P2 inference must emit safe suspension");
    CallContext kv_call{CallerModuleId::InferenceFramework, "r", "t",
                        "principal",
                        TaskPriority::P1,
                        clock->monotonicNowNs() + 1000000000LL};
    expect(kv->queryStatus(kv_call).active_leases == 0,
           "all KV leases must be released after terminal output");
}

void testAdmissionAndMockReality() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-auth");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(1);
    InferenceFramework framework(clock, ids, model, kv, 1);
    auto value = request("job-auth", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    CallContext denied{CallerModuleId::AgentService, "r", "t", {},
                       TaskPriority::P1, value.deadline_mono_ns};
    expect(!framework.submitInference(value, denied).accepted,
           "AgentService must not impersonate an inference owner");

    CallContext allowed{CallerModuleId::IntentRecognitionEngine, "r", "t",
                        {}, TaskPriority::P1, value.deadline_mono_ns};
    allowed.request_id = value.request_id;
    allowed.trace_id = value.trace_id;
    allowed.principal_id_hash = value.admission.principal_id;
    expect(framework.submitInference(value, allowed).accepted,
           "authorized intent inference must be accepted");
    expect(framework.runUntilIdle().ok, "mock inference must complete");
    const auto result = framework.queryInference(value.job_id, allowed);
    expect(result.value && result.value->result &&
               result.value->result->reality == "SIMULATED" &&
               result.value->result->runtime_backend == "mock",
           "mock model output must never claim device reality");
}

void testExternalCallbacksAreUnlockedAndKvUseIsImmediate() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-reentrant");
    auto real_kv =
        std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto kv = std::make_shared<ReentrantKvCache>(real_kv);
    auto model = std::make_shared<ReentrantRuntime>(3);
    InferenceFramework framework(clock, ids, model, kv, 1);
    kv->bind(&framework);
    model->bind(&framework);

    CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine, {}, {}, "principal",
        TaskPriority::P1, clock->monotonicNowNs() + 10000000000LL};
    CallContext kv_call{
        CallerModuleId::InferenceFramework, "kv-query", "trace-kv",
        "principal",
        TaskPriority::P1, clock->monotonicNowNs() + 10000000000LL};

    auto warm = request("job-reentrant-warm", TaskPriority::P1, false,
                        clock->monotonicNowNs() + 9000000000LL);
    intent_call.request_id = warm.request_id;
    intent_call.trace_id = warm.trace_id;
    expect(framework.submitInference(warm, intent_call).accepted,
           "reentrant warm-up must be admitted");
    expect(framework.runUntilIdle().ok,
           "reentrant warm-up must complete and publish KV");
    expect(real_kv->queryStatus(kv_call).ready_entries == 1,
           "warm-up must create one reusable KV entry");

    auto hit = request("job-reentrant-hit", TaskPriority::P1, false,
                       clock->monotonicNowNs() + 9000000000LL);
    // Preserve the logical prompt/namespace so the second job is a cache hit.
    hit.prompt = warm.prompt;
    hit.prompt_digest = warm.prompt_digest;
    hit.prompt_segments = warm.prompt_segments;
    hit.session_id = warm.session_id;
    intent_call.request_id = hit.request_id;
    intent_call.trace_id = hit.trace_id;
    kv->failNextCompleteAfterApply();
    expect(framework.submitInference(hit, intent_call).accepted,
           "cache-hit inference must be admitted");
    expect(framework.pumpOne(),
           "cache-hit inference must start");

    // completeUse was applied before startJob returned, even though this test
    // deliberately loses the first response.
    expect(real_kv->queryStatus(kv_call).active_leases == 0,
           "KV hit lease must be returned immediately after simulated import");
    const auto first_reports = kv->completeReports();
    expect(first_reports.size() == 1 &&
               first_reports.front().restore_succeeded,
           "cache hit must report one immediate USED completion");

    expect(framework.pumpOne(),
           "ambiguous completeUse response must be retried");
    const auto replayed_reports = kv->completeReports();
    expect(replayed_reports.size() == 2 &&
               replayed_reports[0].complete_id ==
                   replayed_reports[1].complete_id &&
               replayed_reports[0].lease_id ==
                   replayed_reports[1].lease_id &&
               replayed_reports[0].job_id ==
                   replayed_reports[1].job_id &&
               replayed_reports[0].restore_succeeded ==
                   replayed_reports[1].restore_succeeded,
           "completeUse timeout replay must preserve the exact report");
    expect(framework.runUntilIdle().ok,
           "cache-hit inference must drain after completion replay");
    expect(real_kv->queryStatus(kv_call).active_leases == 0,
           "completion replay must not recreate or leak a lease");
    expect(model->probesPassed() && kv->probesPassed() &&
               model->sizingCalls() == 2 &&
               model->inferCalls() == 2,
           "runtime sizing/infer and KV acquire/publish/complete callbacks "
           "must all re-enter without observing framework mutex ownership");
}

void testCancelDuringKvAcquireReleasesLateLease() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("inference-acquire-cancel");
    auto real_kv =
        std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto kv =
        std::make_shared<CancellingAcquireKvCache>(real_kv);
    auto model = std::make_shared<MockModelRuntime>(2);
    InferenceFramework framework(clock, ids, model, kv, 1);

    CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine, {}, {}, "principal",
        TaskPriority::P1,
        clock->monotonicNowNs() + 10'000'000'000LL};
    CallContext kv_call{
        CallerModuleId::InferenceFramework, "kv-query", "trace-kv",
        "principal",
        TaskPriority::P1,
        clock->monotonicNowNs() + 10'000'000'000LL};

    auto warm = request("job-acquire-warm", TaskPriority::P1, false,
                        clock->monotonicNowNs() + 9'000'000'000LL);
    intent_call.request_id = warm.request_id;
    intent_call.trace_id = warm.trace_id;
    expect(framework.submitInference(warm, intent_call).accepted,
           "KV acquire cancellation warm-up must be admitted");
    expect(framework.runUntilIdle().ok &&
               real_kv->queryStatus(kv_call).ready_entries == 1,
           "warm-up must create one reusable KV entry");

    auto target = request(
        "job-cancel-inside-acquire", TaskPriority::P1, false,
        clock->monotonicNowNs() + 9'000'000'000LL);
    target.prompt = warm.prompt;
    target.prompt_digest = warm.prompt_digest;
    target.prompt_segments = warm.prompt_segments;
    target.session_id = warm.session_id;
    intent_call.request_id = target.request_id;
    intent_call.trace_id = target.trace_id;
    kv->arm(&framework, target.job_id);

    expect(framework.submitInference(target, intent_call).accepted,
           "acquire-race target must be admitted");
    expect(framework.pumpOne(),
           "target must execute the re-entrant KV acquire");
    const auto cancelled =
        framework.queryInference(target.job_id, intent_call);
    expect(kv->observedActiveLease() && kv->cancelSucceeded(),
           "KV callback must observe a real active lease and cancel");
    expect(cancelled.value &&
               cancelled.value->state ==
                   InferenceJobState::Cancelled &&
               cancelled.value->replica_id.empty() &&
               !cancelled.value->result,
           "cancellation must win over the late KV acquire result");
    expect(real_kv->queryStatus(kv_call).active_leases == 0,
           "late KV HIT lease must be completed after cancellation");
    const auto events = framework.events();
    expect(std::any_of(events.begin(), events.end(),
                       [&target](const auto& event) {
                           return event.job_id == target.job_id &&
                                  event.event_type ==
                                      "RESOURCES_RELEASED" &&
                                  event.resource_released;
                       }),
           "lease cleanup must emit a released-resource fact");
}

void testReentrantCancelWinsOverLateRuntimeOutput() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-late-output");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<CancellingRuntime>();
    InferenceFramework framework(clock, ids, model, kv, 1);
    model->bind(&framework);

    auto victim = request("job-runtime-cancel", TaskPriority::P1, false,
                          clock->monotonicNowNs() + 5000000000LL);
    CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine, victim.request_id,
        victim.trace_id, victim.admission.principal_id, TaskPriority::P1,
        victim.deadline_mono_ns};
    expect(framework.submitInference(victim, intent_call).accepted,
           "late-output victim must be admitted");
    expect(framework.pumpOne(), "victim must acquire the replica");
    expect(framework.pumpOne(),
           "runtime callback must cancel and return a late output");
    const auto cancelled =
        framework.queryInference(victim.job_id, intent_call);
    expect(model->cancelSucceeded() && cancelled.value &&
               cancelled.value->state ==
                   InferenceJobState::Cancelled &&
               !cancelled.value->result &&
               cancelled.value->replica_id.empty(),
           "reentrant cancel must win and late output must release replica");

    auto successor = request(
        "job-after-runtime-cancel", TaskPriority::P1, false,
        clock->monotonicNowNs() + 5000000000LL);
    intent_call.request_id = successor.request_id;
    intent_call.trace_id = successor.trace_id;
    intent_call.deadline_mono_ns = successor.deadline_mono_ns;
    expect(framework.submitInference(successor, intent_call).accepted,
           "successor must be admitted after cancellation");
    expect(framework.runUntilIdle().ok,
           "released replica must execute the successor");
    const auto completed =
        framework.queryInference(successor.job_id, intent_call);
    expect(completed.value &&
               completed.value->state ==
                   InferenceJobState::Completed,
           "late runtime output must not strand the only replica");
}

void testDeadlineIsRecheckedAfterRuntimeCallback() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-deadline-fence");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<DeadlineAdvancingRuntime>(clock);
    InferenceFramework framework(clock, ids, model, kv, 1);

    auto value = request("job-runtime-deadline", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    CallContext intent_call{
        CallerModuleId::IntentRecognitionEngine, value.request_id,
        value.trace_id, value.admission.principal_id, TaskPriority::P1,
        value.deadline_mono_ns};
    expect(framework.submitInference(value, intent_call).accepted,
           "deadline fence job must be admitted");
    expect(framework.pumpOne(), "deadline fence job must start");
    expect(framework.pumpOne(),
           "runtime callback must return after advancing the clock");
    auto query_call = intent_call;
    query_call.deadline_mono_ns =
        clock->monotonicNowNs() + 1'000'000'000LL;
    const auto failed =
        framework.queryInference(value.job_id, query_call);
    expect(failed.value &&
               failed.value->state == InferenceJobState::Failed &&
               failed.value->last_error &&
               failed.value->last_error->code ==
                   "INFERENCE_DEADLINE_EXPIRED" &&
               !failed.value->result,
           "post-runtime deadline fence must reject late model output");
    CallContext kv_call{
        CallerModuleId::InferenceFramework, "kv-query", "trace-kv",
        "principal",
        TaskPriority::P1, clock->monotonicNowNs() + 1000000000LL};
    expect(kv->queryStatus(kv_call).ready_entries == 0,
           "late runtime output must not be published to KV cache");
}

void testRuntimeInvocationSealRejectsStaleCallbacks() {
    const auto run =
        [](SealTamperingRuntime::Field field,
           const std::string& suffix,
           const std::string& expected_error) {
            auto clock = std::make_shared<ManualRuntimeClock>();
            auto ids = std::make_shared<IdGenerator>(
                "inference-seal-" + suffix);
            auto kv =
                std::make_shared<kv_cache::KvCacheManager>(
                    clock, ids);
            auto model =
                std::make_shared<SealTamperingRuntime>(field);
            InferenceFramework framework(
                clock, ids, model, kv, 1);
            auto value = request(
                "job-seal-" + suffix, TaskPriority::P1, false,
                clock->monotonicNowNs() + 5'000'000'000LL);
            CallContext call{
                CallerModuleId::IntentRecognitionEngine,
                value.request_id, value.trace_id,
                value.admission.principal_id, TaskPriority::P1,
                value.deadline_mono_ns};
            expect(framework.submitInference(value, call).accepted,
                   "seal test job must be admitted");
            expect(framework.runUntilIdle().ok,
                   "stale callback must become a deterministic failure");
            const auto snapshot =
                framework.queryInference(value.job_id, call);
            expect(snapshot.value &&
                       snapshot.value->state ==
                           InferenceJobState::Failed &&
                       snapshot.value->last_error &&
                       snapshot.value->last_error->code ==
                           expected_error &&
                       !snapshot.value->result,
                   "stale Runtime seal must not commit a model output");
        };

    run(SealTamperingRuntime::Field::ReplicaEpoch, "epoch",
        "INFERENCE_STALE_REPLICA_EPOCH");
    run(SealTamperingRuntime::Field::Lease, "lease",
        "INFERENCE_STALE_LEASE");
    run(SealTamperingRuntime::Field::Fence, "fence",
        "INFERENCE_STALE_LEASE");
    run(SealTamperingRuntime::Field::Operation, "operation",
        "INFERENCE_OUTPUT_IDENTITY_MISMATCH");
}

void testReplicaRebuildAdvancesEpochAndTerminalEventSeal() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "inference-replica-rebuild");
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(1);
    InferenceFramework framework(clock, ids, model, kv, 1);

    auto first = request(
        "job-before-rebuild", TaskPriority::P1, false,
        clock->monotonicNowNs() + 5'000'000'000LL);
    CallContext intent{
        CallerModuleId::IntentRecognitionEngine, first.request_id,
        first.trace_id, first.admission.principal_id,
        TaskPriority::P1, first.deadline_mono_ns};
    expect(framework.submitInference(first, intent).accepted &&
               framework.runUntilIdle().ok,
           "pre-rebuild inference must complete");
    const auto first_snapshot =
        framework.queryInference(first.job_id, intent);
    expect(first_snapshot.value && first_snapshot.value->result &&
               first_snapshot.value->replica_epoch == 1,
           "initial Replica generation must be epoch 1");

    CallContext rebuild{
        CallerModuleId::AgentService, "rebuild-request",
        "trace-rebuild", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 5'000'000'000LL};
    expect(framework.rebuildReplica(
               "mock-replica-1", rebuild)
               .ok,
           "drained Replica must rebuild and fence epoch 1");

    auto second = request(
        "job-after-rebuild", TaskPriority::P1, false,
        clock->monotonicNowNs() + 5'000'000'000LL);
    intent.request_id = second.request_id;
    intent.trace_id = second.trace_id;
    intent.deadline_mono_ns = second.deadline_mono_ns;
    expect(framework.submitInference(second, intent).accepted &&
               framework.runUntilIdle().ok,
           "post-rebuild inference must complete");
    const auto snapshot =
        framework.queryInference(second.job_id, intent);
    expect(snapshot.value && snapshot.value->result &&
               snapshot.value->state ==
                   InferenceJobState::Completed &&
               snapshot.value->replica_epoch == 2 &&
               snapshot.value->result->replica_epoch == 2 &&
               snapshot.value->fencing_token > 0 &&
               snapshot.value->result->fencing_token ==
                   snapshot.value->fencing_token &&
               snapshot.value->result->lease_id ==
                   snapshot.value->lease_id,
           "new output must bind Replica epoch 2 and its exact lease");
    const auto events = framework.events();
    const auto terminal = std::find_if(
        events.begin(), events.end(),
        [&second](const auto& event) {
            return event.job_id == second.job_id &&
                   event.event_type == "COMPLETED";
        });
    expect(terminal != events.end() &&
               terminal->replica_epoch == 2 &&
               terminal->lease_id == snapshot.value->lease_id &&
               terminal->fencing_token ==
                   snapshot.value->fencing_token &&
               terminal->result &&
               terminal->result->invocation_id ==
                   snapshot.value->result->invocation_id,
           "terminal event must retain the complete invocation seal");
}

/// A permanently failing cleanup for Job A must not monopolize pumpOne.
/// Jobs B/C deliberately leave two more leases pending, so successful
/// completion proves that the cleanup cursor rotates between owners.
void testKvCleanupRoundRobinDoesNotHeadOfLineBlock() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "inference-kv-cleanup-rotation");
    auto kv = std::make_shared<RotatingFailureKvCache>();
    kv->failForever("job-cleanup-a");
    kv->failTimes("job-cleanup-b", 1U);
    kv->failTimes("job-cleanup-c", 2U);
    auto model = std::make_shared<MockModelRuntime>(1);
    InferenceFramework framework(clock, ids, model, kv, 3);

    const auto deadline =
        clock->monotonicNowNs() + 10'000'000'000LL;
    auto job_a = request("job-cleanup-a", TaskPriority::P1,
                         false, deadline);
    auto job_b = request("job-cleanup-b", TaskPriority::P1,
                         false, deadline);
    auto job_c = request("job-cleanup-c", TaskPriority::P1,
                         false, deadline);
    const auto call_a = intentCall(job_a);
    const auto call_b = intentCall(job_b);
    const auto call_c = intentCall(job_c);
    expect(framework.submitInference(job_a, call_a).accepted &&
               framework.submitInference(job_b, call_b).accepted &&
               framework.submitInference(job_c, call_c).accepted,
           "all cleanup-rotation jobs must be admitted");

    // A never cleans up, so runUntilIdle is intentionally inappropriate.
    // A bounded number of deterministic pumps is enough for B/C to rotate
    // through their transient failures, execute and seal their outputs.
    for (std::size_t step = 0; step < 30U; ++step) {
        (void)framework.pumpOne();
    }

    const auto snapshot_a =
        framework.queryInference(job_a.job_id, call_a);
    const auto snapshot_b =
        framework.queryInference(job_b.job_id, call_b);
    const auto snapshot_c =
        framework.queryInference(job_c.job_id, call_c);
    expect(snapshot_a.value &&
               snapshot_a.value->state ==
                   InferenceJobState::Running &&
               snapshot_a.value->stage ==
                   "KV_RELEASE_PENDING" &&
               snapshot_b.value &&
               snapshot_b.value->state ==
                   InferenceJobState::Completed &&
               snapshot_b.value->result &&
               snapshot_c.value &&
               snapshot_c.value->state ==
                   InferenceJobState::Completed &&
               snapshot_c.value->result,
           "A cleanup failure must not starve B/C execution");

    const auto reports = kv->completionReports();
    std::map<std::string, std::size_t> report_counts;
    std::map<std::string, std::set<std::string>> complete_ids;
    std::size_t owner_transitions = 0;
    for (std::size_t index = 0; index < reports.size(); ++index) {
        ++report_counts[reports[index].job_id];
        complete_ids[reports[index].job_id].insert(
            reports[index].complete_id);
        if (index > 0U &&
            reports[index - 1U].job_id !=
                reports[index].job_id) {
            ++owner_transitions;
        }
    }
    expect(report_counts[job_a.job_id] >= 3U &&
               report_counts[job_b.job_id] >= 2U &&
               report_counts[job_c.job_id] >= 3U &&
               complete_ids[job_a.job_id].size() == 1U &&
               complete_ids[job_b.job_id].size() == 1U &&
               complete_ids[job_c.job_id].size() == 1U &&
               owner_transitions >= 4U,
           "cleanup retries must round-robin multiple leases with stable IDs");

    CallContext kv_call{
        CallerModuleId::InferenceFramework, "kv-status",
        "trace-kv-status", "principal", TaskPriority::P1,
        deadline};
    expect(kv->queryStatus(kv_call).active_leases == 1U,
           "only A's permanently failing synthetic lease may remain");
}

void testPromptProtocolVersionPartitionsKvFingerprint() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "inference-protocol-fingerprint");
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(1);
    InferenceFramework framework(clock, ids, model, kv, 1);
    const auto deadline =
        clock->monotonicNowNs() + 10'000'000'000LL;

    auto protocol_v1 = request(
        "job-protocol-v1", TaskPriority::P1, false, deadline);
    protocol_v1.prompt_protocol_version = "prompt-protocol-v1";
    auto call_v1 = intentCall(protocol_v1);
    expect(framework.submitInference(protocol_v1, call_v1).accepted &&
               framework.runUntilIdle().ok,
           "protocol-v1 warm-up must complete");

    CallContext kv_call{
        CallerModuleId::InferenceFramework, "kv-protocol-status",
        "trace-kv-protocol", "principal", TaskPriority::P1,
        deadline};
    const auto after_v1 = kv->queryStatus(kv_call);
    expect(after_v1.ready_entries == 1U &&
               after_v1.hit_count == 0U &&
               after_v1.miss_count == 1U,
           "first protocol version must populate one cache entry");

    auto protocol = request(
        "job-protocol-v2", TaskPriority::P1, false, deadline);
    protocol.prompt = protocol_v1.prompt;
    protocol.prompt_digest = protocol_v1.prompt_digest;
    protocol.prompt_segments = protocol_v1.prompt_segments;
    protocol.session_id = protocol_v1.session_id;
    protocol.model = protocol_v1.model;
    protocol.prompt_protocol_version =
        "prompt-protocol-v2";
    auto call = intentCall(protocol);
    expect(framework.submitInference(protocol, call).accepted &&
               framework.runUntilIdle().ok,
           "protocol-v2 inference must complete after a safe KV miss");
    const auto after = kv->queryStatus(kv_call);
    expect(after.ready_entries == 2U &&
               after.hit_count == 0U &&
               after.miss_count == 2U,
           "same segments/model with a different prompt protocol must miss");

    auto protocol_reuse = request(
        "job-protocol-v2-reuse", TaskPriority::P1, false,
        deadline);
    protocol_reuse.prompt = protocol.prompt;
    protocol_reuse.prompt_digest = protocol.prompt_digest;
    protocol_reuse.prompt_segments =
        protocol.prompt_segments;
    protocol_reuse.session_id = protocol.session_id;
    protocol_reuse.model = protocol.model;
    protocol_reuse.prompt_protocol_version =
        protocol.prompt_protocol_version;
    auto call_reuse = intentCall(protocol_reuse);
    expect(framework
               .submitInference(protocol_reuse,
                                call_reuse)
               .accepted &&
               framework.runUntilIdle().ok,
           "identical protocol-v2 request must complete");
    const auto after_reuse = kv->queryStatus(kv_call);
    expect(after_reuse.hit_count == 1U &&
               after_reuse.miss_count == 2U,
           "same protocol must hit, proving protocol version caused the miss");
}

void testMaliciousRuntimeOutputBoundsFailClosed() {
    const auto run =
        [](MaliciousOutputRuntime::Mutation mutation,
           const std::string& suffix) {
            auto clock =
                std::make_shared<ManualRuntimeClock>();
            auto ids = std::make_shared<IdGenerator>(
                "inference-hostile-output-" + suffix);
            auto kv = std::make_shared<
                kv_cache::KvCacheManager>(clock, ids);
            auto runtime =
                std::make_shared<MaliciousOutputRuntime>(
                    mutation);
            InferenceFramework framework(
                clock, ids, runtime, kv, 1);
            auto hostile = request(
                "job-hostile-" + suffix, TaskPriority::P1,
                false,
                clock->monotonicNowNs() +
                    5'000'000'000LL);
            const auto call = intentCall(hostile);
            expect(framework
                       .submitInference(hostile, call)
                       .accepted &&
                       framework.runUntilIdle().ok,
                   "hostile Runtime result must terminalize");
            const auto snapshot = framework.queryInference(
                hostile.job_id, call);
            expect(snapshot.value &&
                       snapshot.value->state ==
                           InferenceJobState::Failed &&
                       snapshot.value->last_error &&
                       snapshot.value->last_error->code ==
                           "INFERENCE_OUTPUT_CONTRACT_VIOLATION" &&
                       !snapshot.value->result,
                   "invalid Runtime output must fail without storing result");

            CallContext kv_call{
                CallerModuleId::InferenceFramework,
                "kv-hostile-status-" + suffix,
                "trace-kv-hostile", "principal",
                TaskPriority::P1,
                clock->monotonicNowNs() +
                    1'000'000'000LL};
            expect(kv->queryStatus(kv_call).ready_entries == 0U,
                   "invalid Runtime output must not enter KV");
        };

    run(MaliciousOutputRuntime::Mutation::OversizedRawOutput,
        "oversized-raw");
    run(MaliciousOutputRuntime::Mutation::InvalidUtf8,
        "invalid-utf8");
    run(MaliciousOutputRuntime::Mutation::ControlCharacter,
        "control-character");
    run(MaliciousOutputRuntime::Mutation::OversizedMetadata,
        "oversized-metadata");
}

/// Splits text on UTF-8 boundaries so a chunk is never half a code point.
/// Real tokenizers emit whole tokens; a test that split mid-sequence would be
/// exercising invalid UTF-8 handling rather than the streaming contract.
std::vector<std::string> utf8Chunks(const std::string& text,
                                    std::size_t target) {
    std::vector<std::string> chunks;
    std::size_t index = 0;
    while (index < text.size()) {
        std::size_t take = std::min(target, text.size() - index);
        while (index + take < text.size() &&
               (static_cast<unsigned char>(text[index + take]) & 0xC0U) ==
                   0x80U) {
            ++take;
        }
        chunks.push_back(text.substr(index, take));
        index += take;
    }
    if (chunks.empty()) chunks.push_back(std::string{});
    return chunks;
}

/// Wraps the mock runtime and replays its output as chunks, with switches for
/// each way a runtime can break the streaming contract.
class StreamingRuntime final : public IModelRuntime {
public:
    enum class Mode {
        Faithful,
        /// Streams text that is not what it finally returns.
        Diverge,
        /// Never sets final.
        NoFinal,
        /// Repeats an index.
        OutOfOrder,
        /// Stamps a foreign invocation_id.
        BadInvocation,
        /// Sends a chunk after the final one.
        AfterFinal
    };

    explicit StreamingRuntime(Mode mode) : mode_(mode) {}

    std::uint32_t requiredWorkUnits(
        const InferenceRequest& request) const override {
        return delegate_.requiredWorkUnits(request);
    }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override {
        return delegate_.infer(request, seal);
    }

    bool supportsStreaming() const override { return true; }

    Result<InferenceOutput> inferStream(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal,
        const InferenceStreamSink& sink) override {
        auto output = delegate_.infer(request, seal);
        if (!output.status.ok || !output.value) return output;

        const std::string streamed =
            mode_ == Mode::Diverge
                ? std::string("{\"outcome\":\"REPLY\",\"reply\":\"x\"}")
                : output.value->raw_output;
        const auto chunks = utf8Chunks(streamed, 16);
        std::uint32_t index = 0;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            InferenceChunk chunk;
            chunk.invocation_id = mode_ == Mode::BadInvocation
                                      ? std::string("forged-invocation")
                                      : seal.invocation_id;
            chunk.chunk_index = index;
            chunk.delta = chunks[i];
            const bool last = i + 1 == chunks.size();
            chunk.final = last && mode_ != Mode::NoFinal;
            if (chunk.final) chunk.finish_reason = "stop";
            if (mode_ == Mode::OutOfOrder && i == 1) {
                chunk.chunk_index = 0;
            }
            ++delivered_;
            if (sink && sink(chunk) == StreamControl::Abort) {
                aborted_at_ = index;
                // A cooperative runtime stops generating and returns what it
                // has. Truncating raw_output means re-sealing the digest,
                // exactly as a real runtime must.
                std::string partial;
                for (std::size_t k = 0; k <= i; ++k) partial += chunks[k];
                output.value->raw_output = partial;
                output.value->finish_reason = "abort";
                output.value->generated_token_count = 1;
                output.value->output_digest =
                    inferenceOutputDigest(*output.value);
                return output;
            }
            ++index;
        }
        if (mode_ == Mode::AfterFinal && sink) {
            InferenceChunk extra;
            extra.invocation_id = seal.invocation_id;
            extra.chunk_index = index;
            extra.delta = "!";
            ++delivered_;
            (void)sink(extra);
        }
        return output;
    }

    std::uint32_t delivered() const { return delivered_; }
    int abortedAt() const { return aborted_at_; }

private:
    Mode mode_;
    MockModelRuntime delegate_{1};
    std::uint32_t delivered_ = 0;
    int aborted_at_ = -1;
};

/// Drives one streamed job to a terminal state and returns its snapshot.
struct StreamRun {
    std::optional<InferenceJobSnapshot> snapshot;
    std::vector<InferenceChunk> observed;
    Status attach_status = Status::Ok();
};

StreamRun runStreamedJob(
    StreamingRuntime::Mode mode,
    const std::function<StreamControl(const InferenceChunk&)>& on_chunk =
        nullptr) {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-stream");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<StreamingRuntime>(mode);
    InferenceFramework framework(clock, ids, model, kv, 1);

    auto value = request("job-stream", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    const auto call = intentCall(value);
    StreamRun run;
    expect(framework.submitInference(value, call).accepted,
           "streamed job must be admitted");

    auto observed = std::make_shared<std::vector<InferenceChunk>>();
    run.attach_status = framework.attachStreamSink(
        value.job_id, 1,
        [observed, on_chunk](const InferenceChunk& chunk) {
            observed->push_back(chunk);
            return on_chunk ? on_chunk(chunk) : StreamControl::Continue;
        },
        call);
    expect(run.attach_status.ok,
           "owner must be allowed to attach a presentation sink");

    expect(framework.runUntilIdle().ok,
           "streamed job must reach a terminal state");
    const auto result = framework.queryInference(value.job_id, call);
    run.snapshot = result.value;
    run.observed = *observed;
    return run;
}

void testStreamedChunksReachSinkAndMatchSealedOutput() {
    const auto run = runStreamedJob(StreamingRuntime::Mode::Faithful);
    expect(run.snapshot && run.snapshot->result,
           "faithful streaming must still commit an output");
    const auto& out = *run.snapshot->result;
    expect(out.stream_integrity == StreamIntegrity::Verified,
           "framework must independently verify streamed text");
    expect(run.observed.size() > 1,
           "output must arrive as several chunks, not one blob");
    expect(out.streamed_chunk_count == run.observed.size(),
           "committed chunk count must match what the sink observed");
    expect(out.first_chunk_mono_ns > 0,
           "first-token latency must be observable");

    std::string rebuilt;
    for (const auto& chunk : run.observed) rebuilt += chunk.delta;
    expect(rebuilt == out.raw_output,
           "concatenated chunks must equal the committed output");
    expect(run.observed.back().final &&
               !run.observed.back().finish_reason.empty(),
           "the last chunk must be marked final and carry a reason");

    // The digest is the point: a caller can prove the text it presented is the
    // same text the decision path committed.
    expect(out.output_digest == inferenceOutputDigest(out),
           "streaming must not disturb the output seal");
}

void testStreamedOutputDivergingFromSealFailsClosed() {
    const auto run = runStreamedJob(StreamingRuntime::Mode::Diverge);
    expect(run.snapshot && !run.snapshot->result,
           "text shown to a user that the seal never covered must not commit");
    expect(run.snapshot->state == InferenceJobState::Failed &&
               run.snapshot->last_error &&
               run.snapshot->last_error->code ==
                   "INFERENCE_STREAM_OUTPUT_DIVERGED",
           "divergence must be attributed, not silently tolerated");
}

void testStreamChunkProtocolViolationsFailClosed() {
    struct Case {
        StreamingRuntime::Mode mode;
        std::string code;
    };
    const std::vector<Case> cases = {
        {StreamingRuntime::Mode::OutOfOrder,
         "INFERENCE_STREAM_CHUNK_OUT_OF_ORDER"},
        {StreamingRuntime::Mode::BadInvocation,
         "INFERENCE_STREAM_INVOCATION_MISMATCH"},
        {StreamingRuntime::Mode::AfterFinal,
         "INFERENCE_STREAM_CHUNK_AFTER_FINAL"},
        {StreamingRuntime::Mode::NoFinal,
         "INFERENCE_STREAM_OUTPUT_DIVERGED"}};
    for (const auto& item : cases) {
        const auto run = runStreamedJob(item.mode);
        expect(run.snapshot && !run.snapshot->result &&
                   run.snapshot->state == InferenceJobState::Failed,
               "a runtime breaking the chunk contract must not commit");
        expect(run.snapshot->last_error &&
                   run.snapshot->last_error->code == item.code,
               "chunk contract violation must report " + item.code);
    }
}

void testSinkAbortStopsRuntimeAndStillCommits() {
    // Barge-in: the user starts talking over the assistant. The sink says stop.
    const auto run = runStreamedJob(
        StreamingRuntime::Mode::Faithful,
        [](const InferenceChunk&) { return StreamControl::Abort; });
    expect(run.observed.size() == 1,
           "Abort on the first chunk must stop further delivery");
    expect(run.snapshot && run.snapshot->result,
           "a cooperative abort must not destroy the partial result");
    const auto& out = *run.snapshot->result;
    expect(out.stream_integrity == StreamIntegrity::Aborted,
           "abort must be recorded rather than reported as verified");
    expect(out.raw_output == run.observed.front().delta,
           "committed output must be exactly the text already presented");
}

void testNonStreamingRuntimeIsUnaffected() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-nostream");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(1);
    InferenceFramework framework(clock, ids, model, kv, 1);
    auto value = request("job-nostream", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    const auto call = intentCall(value);
    expect(framework.submitInference(value, call).accepted,
           "non-streaming job must be admitted");
    expect(framework.runUntilIdle().ok, "non-streaming job must complete");
    const auto result = framework.queryInference(value.job_id, call);
    expect(result.value && result.value->result,
           "non-streaming path must still commit");
    expect(result.value->result->stream_integrity ==
               StreamIntegrity::NotStreamed &&
           result.value->result->streamed_chunk_count == 0,
           "a runtime that does not stream must not be described as verified");
    expect(result.value->result->output_digest ==
               inferenceOutputDigest(*result.value->result),
           "adding stream fields must not change existing output digests");
}

void testStreamingWithoutAnyAttachedSinkStillVerifies() {
    // The default path: the runtime streams, nobody is listening. Verification
    // is the framework's own business, so it must happen either way.
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-nosink");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<StreamingRuntime>(
        StreamingRuntime::Mode::Faithful);
    InferenceFramework framework(clock, ids, model, kv, 1);
    auto value = request("job-nosink", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    const auto call = intentCall(value);
    expect(framework.submitInference(value, call).accepted,
           "job must be admitted");
    expect(framework.runUntilIdle().ok, "job must complete without a sink");
    const auto result = framework.queryInference(value.job_id, call);
    expect(result.value && result.value->result,
           "an unobserved stream must still commit");
    expect(result.value->result->stream_integrity ==
               StreamIntegrity::Verified,
           "verification must not depend on someone listening");
    expect(result.value->result->streamed_chunk_count > 1,
           "chunks must be counted even with no presentation sink");

    const auto diverged = [&]() {
        auto bad_ids = std::make_shared<IdGenerator>("inference-nosink-bad");
        auto bad_kv =
            std::make_shared<kv_cache::KvCacheManager>(clock, bad_ids);
        auto bad_model = std::make_shared<StreamingRuntime>(
            StreamingRuntime::Mode::Diverge);
        InferenceFramework bad(clock, bad_ids, bad_model, bad_kv, 1);
        auto bad_value = request("job-nosink-bad", TaskPriority::P1, false,
                                 clock->monotonicNowNs() + 1000000000LL);
        const auto bad_call = intentCall(bad_value);
        expect(bad.submitInference(bad_value, bad_call).accepted, "admitted");
        expect(bad.runUntilIdle().ok, "terminal");
        return bad.queryInference(bad_value.job_id, bad_call).value;
    }();
    expect(diverged && !diverged->result &&
               diverged->state == InferenceJobState::Failed,
           "a lying runtime must be caught even when no sink is attached");
}

void testStreamSinkAttachIsGoverned() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("inference-sink-auth");
    auto kv = std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<StreamingRuntime>(
        StreamingRuntime::Mode::Faithful);
    InferenceFramework framework(clock, ids, model, kv, 1);
    auto value = request("job-sink-auth", TaskPriority::P1, false,
                         clock->monotonicNowNs() + 1000000000LL);
    const auto call = intentCall(value);
    expect(framework.submitInference(value, call).accepted,
           "job must be admitted");

    const auto sink = [](const InferenceChunk&) {
        return StreamControl::Continue;
    };

    auto foreign = call;
    foreign.caller = CallerModuleId::MemoryService;
    expect(!framework.attachStreamSink(value.job_id, 1, sink, foreign).ok,
           "a non-owning module must not tap another owner's model output");

    auto forged = call;
    forged.trace_id = "trace-other";
    expect(!framework.attachStreamSink(value.job_id, 1, sink, forged).ok,
           "sink attach must be bound to the target job identity");

    expect(!framework.attachStreamSink("job-absent", 1, sink, call).ok,
           "unknown job must be rejected");
    expect(!framework.attachStreamSink(value.job_id, 0, sink, call).ok,
           "a zero control epoch must be rejected");
    expect(!framework.attachStreamSink(value.job_id, 1, nullptr, call).ok,
           "an empty sink must be rejected rather than stored");

    expect(framework.attachStreamSink(value.job_id, 1, sink, call).ok,
           "the owner must be allowed");
    expect(framework.runUntilIdle().ok, "job must complete");
    expect(!framework.attachStreamSink(value.job_id, 1, sink, call).ok,
           "a terminal job must not accept a new sink");
}

void testInferenceOwnerModuleIsolation() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("inference-owner-isolation");
    auto kv =
        std::make_shared<kv_cache::KvCacheManager>(clock, ids);
    auto model = std::make_shared<MockModelRuntime>(8);
    InferenceFramework framework(clock, ids, model, kv, 1);
    auto value = request(
        "job-owner-isolation", TaskPriority::P2, false,
        clock->monotonicNowNs() + 5'000'000'000LL);
    const auto owner = intentCall(value);
    expect(framework.submitInference(value, owner).accepted &&
               framework.pumpOne(),
           "owner-isolation job must be admitted and running");

    CallContext memory{
        CallerModuleId::MemoryService, value.request_id,
        value.trace_id, value.admission.principal_id,
        value.priority,
        clock->monotonicNowNs() + 1'000'000'000LL};
    const auto cross_query =
        framework.queryInference(value.job_id, memory);
    expect(!cross_query.status.ok &&
               cross_query.status.error.code ==
                   "INFERENCE_QUERY_OWNER_MISMATCH",
           "same request/trace/principal must not let Memory query an "
           "Intent-owned job");
    const auto cross_cancel =
        framework.cancelInference(value.job_id, 1, memory);
    expect(!cross_cancel.ok &&
               cross_cancel.error.code ==
                   "INFERENCE_CANCEL_OWNER_MISMATCH",
           "same request/trace/principal must not let Memory cancel an "
           "Intent-owned job");

    CallContext dispatch{
        CallerModuleId::AgentDispatch, value.request_id,
        value.trace_id, value.admission.principal_id,
        TaskPriority::P0,
        clock->monotonicNowNs() + 1'000'000'000LL,
        {}, 0, "trusted-safety:owner-isolation"};
    const auto cross_preempt = framework.requestPreempt(
        value.job_id, TaskPriority::P0, 2, dispatch);
    expect(!cross_preempt.ok &&
               cross_preempt.error.code ==
                   "INFERENCE_PREEMPT_OWNER_MISMATCH",
           "AgentDispatch may preempt only SubAgent-owned child jobs");
    expect(framework.queryInference(value.job_id, owner).status.ok,
           "the authenticated owner must retain access");
}

}  // namespace

int main() {
    testSubAgentInferenceFailsClosedWithoutLineageValidator();
    testSubAgentInferenceRejectsForgedParentLineageMatrix();
    testRunningDispatchChildInferenceCompletesAndReleasesParent();
    testParentLeasePinnedAcrossPhysicalModelInvocation();
    testRuntimeExceptionStillReleasesParentReservation();
    testP0PreemptsP2AndKvLeaseCloses();
    testAdmissionAndMockReality();
    testExternalCallbacksAreUnlockedAndKvUseIsImmediate();
    testCancelDuringKvAcquireReleasesLateLease();
    testReentrantCancelWinsOverLateRuntimeOutput();
    testDeadlineIsRecheckedAfterRuntimeCallback();
    testRuntimeInvocationSealRejectsStaleCallbacks();
    testReplicaRebuildAdvancesEpochAndTerminalEventSeal();
    testKvCleanupRoundRobinDoesNotHeadOfLineBlock();
    testPromptProtocolVersionPartitionsKvFingerprint();
    testMaliciousRuntimeOutputBoundsFailClosed();
    testInferenceOwnerModuleIsolation();
    testStreamedChunksReachSinkAndMatchSealedOutput();
    testStreamedOutputDivergingFromSealFailsClosed();
    testStreamChunkProtocolViolationsFailClosed();
    testSinkAbortStopsRuntimeAndStillCommits();
    testNonStreamingRuntimeIsUnaffected();
    testStreamingWithoutAnyAttachedSinkStillVerifies();
    testStreamSinkAttachIsGoverned();
    return 0;
}
