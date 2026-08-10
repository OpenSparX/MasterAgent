/**
 * @file test_observability.cpp
 * @brief Verifies redaction, durability reporting, audit integrity, and trace queries.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/agent_service/agent_service.h"
#include "master_agent/common/types.h"
#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "test_support.h"

using namespace master_agent;
using namespace master_agent::data_log;
using namespace master_agent::exception;
using master_agent::test_support::ScopedTempDirectory;
using master_agent::test_support::expect;

namespace {

std::vector<std::string> readNonEmptyLines(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string readAllBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void writeAllBytes(const std::filesystem::path& path,
                   const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(),
                 static_cast<std::streamsize>(bytes.size()));
    expect(output.good(), "test journal rewrite must succeed");
}

void appendAllBytes(const std::filesystem::path& path,
                    const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(bytes.data(),
                 static_cast<std::streamsize>(bytes.size()));
    expect(output.good(), "test journal append must succeed");
}

std::uint32_t crc32ForTest(const std::string& bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

/// Reference-runtime fault-injection seam for a platform TPM/HSM/remote witness.
class InMemoryTamperEvidenceProvider final
    : public ITamperEvidenceProvider {
public:
    enum class CommitBehavior {
        Normal,
        AcknowledgeWithoutStore,
        AcknowledgeWrongAnchor
    };

    Result<TamperKeyMaterial> currentKey() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (key_unavailable_) {
            return Result<TamperKeyMaterial>::Failure(
                Status::Error(
                    "test_tamper", "TEST_D4_KEY_UNAVAILABLE",
                    "injected key outage", true));
        }
        return Result<TamperKeyMaterial>::Success(
            {"host-test-key-generation-1",
             "host-test-deterministic-secret-material"});
    }

    Result<std::vector<TamperKeyMaterial>>
    verificationKeys() override {
        const auto current = currentKey();
        if (!current.status.ok || !current.value) {
            return Result<std::vector<TamperKeyMaterial>>::Failure(
                current.status);
        }
        return Result<std::vector<TamperKeyMaterial>>::Success(
            {*current.value});
    }

    Result<AuditAnchorSnapshot> loadAnchor() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return Result<AuditAnchorSnapshot>::Success(anchor_);
    }

    Status commitAnchor(
        const AuditAnchorSnapshot& anchor) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (throw_on_commit_) {
            throw std::runtime_error(
                "injected anchor commit exception");
        }
        if (fail_next_commit_) {
            fail_next_commit_ = false;
            return Status::Error(
                "test_tamper", "TEST_D4_ANCHOR_COMMIT_FAILED",
                "injected anchor commit failure", false,
                SideEffectState::Unknown);
        }
        if (commit_behavior_ ==
            CommitBehavior::AcknowledgeWithoutStore) {
            return Status::Ok();
        }
        if (commit_behavior_ ==
            CommitBehavior::AcknowledgeWrongAnchor) {
            anchor_ = anchor;
            anchor_.hash_chain_head =
                secureDigest("injected-wrong-anchor");
            return Status::Ok();
        }
        if (!anchor.present ||
            anchor.generation <= anchor_.generation) {
            return Status::Error(
                "test_tamper", "TEST_D4_ANCHOR_ROLLBACK",
                "anchor generation must advance monotonically");
        }
        anchor_ = anchor;
        return Status::Ok();
    }

    void failNextCommit() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_commit_ = true;
    }

    void setKeyUnavailable(bool unavailable) {
        std::lock_guard<std::mutex> lock(mutex_);
        key_unavailable_ = unavailable;
    }

    void setCommitBehavior(CommitBehavior behavior) {
        std::lock_guard<std::mutex> lock(mutex_);
        commit_behavior_ = behavior;
    }

    AuditAnchorSnapshot anchor() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return anchor_;
    }

private:
    mutable std::mutex mutex_;
    AuditAnchorSnapshot anchor_;
    bool key_unavailable_ = false;
    bool fail_next_commit_ = false;
    bool throw_on_commit_ = false;
    CommitBehavior commit_behavior_ = CommitBehavior::Normal;
};

/// DataLog wrapper used to prove that ExceptionManager performs the
/// post-commit observation call without its state mutex, with its own caller
/// identity, and without making authoritative exception state depend on the
/// projection.
class ControllableDataLog final : public IDataLogService {
public:
    explicit ControllableDataLog(
        std::shared_ptr<IDataLogService> delegate)
        : delegate_(std::move(delegate)) {}

    void rejectNextAppend() { reject_next_append_ = true; }

    void setRejectAll(bool reject) { reject_all_ = reject; }

    void throwNextAppend() { throw_next_append_ = true; }

    void setProbe(std::function<void()> probe) {
        probe_ = std::move(probe);
    }

    bool probePassed() const { return probe_passed_; }

    LogEventBatch lastBatch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_batch_;
    }

    AuditBatch lastAuditBatch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_audit_batch_;
    }

    CallContext lastCall() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_call_;
    }

    std::size_t appendCount() const {
        return append_count_.load();
    }

    Result<LogAppendResult> appendEvents(
        const LogEventBatch& batch,
        const CallContext& call) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_batch_ = batch;
            last_call_ = call;
        }
        ++append_count_;
        runProbe();
        if (throw_next_append_.exchange(false)) {
            throw std::runtime_error("injected DataLog exception");
        }
        if (reject_all_.load() ||
            reject_next_append_.exchange(false)) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_INJECTED_REJECTION",
                "test rejected durable journal append", true,
                SideEffectState::NotStarted));
        }
        return delegate_->appendEvents(batch, call);
    }

    Result<AuditAppendResult> appendAudit(
        const AuditBatch& batch,
        const CallContext& call) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_audit_batch_ = batch;
            last_call_ = call;
        }
        ++append_count_;
        runProbe();
        if (throw_next_append_.exchange(false)) {
            throw std::runtime_error(
                "injected DataLog audit exception");
        }
        if (reject_all_.load() ||
            reject_next_append_.exchange(false)) {
            return Result<AuditAppendResult>::Failure(
                Status::Error(
                    "data_log", "LOG_INJECTED_REJECTION",
                    "test rejected durable audit append", true,
                    SideEffectState::NotStarted));
        }
        return delegate_->appendAudit(batch, call);
    }

    Result<TracePage> queryTrace(
        const TraceQuery& query,
        const CallContext& call) const override {
        return delegate_->queryTrace(query, call);
    }

    Status flush(const CallContext& call) override {
        return delegate_->flush(call);
    }

    LogHealth getHealth(const CallContext& call) const override {
        return delegate_->getHealth(call);
    }

private:
    void runProbe() {
        if (!probe_) return;
        auto completion = std::make_shared<std::promise<void>>();
        auto ready = completion->get_future();
        auto probe = probe_;
        std::thread([probe = std::move(probe), completion]() {
            probe();
            try {
                completion->set_value();
            } catch (...) {
            }
        }).detach();
        probe_passed_ =
            ready.wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready;
    }

    std::shared_ptr<IDataLogService> delegate_;
    std::atomic<bool> reject_next_append_{false};
    std::atomic<bool> reject_all_{false};
    std::atomic<bool> throw_next_append_{false};
    std::atomic<bool> probe_passed_{false};
    std::atomic<std::size_t> append_count_{0};
    std::function<void()> probe_;
    mutable std::mutex mutex_;
    LogEventBatch last_batch_;
    AuditBatch last_audit_batch_;
    CallContext last_call_;
};

/// Forces two AgentService turns to reach the same downstream failure point
/// before either returns. The injected StructuredError deliberately embeds
/// the raw request in every untrusted free-form field.
class BarrierFailingPreprocess final
    : public preprocess::IPreprocessEngine {
public:
    explicit BarrierFailingPreprocess(std::size_t parties)
        : parties_(parties) {}

    Result<preprocess::PreprocessResult> process(
        const interaction::StandardRequest& request,
        const CallContext&) const override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++arrivals_;
            if (arrivals_ >= parties_) {
                released_ = true;
                condition_.notify_all();
            } else if (!condition_.wait_for(
                           lock, std::chrono::seconds(2),
                           [this] { return released_; })) {
                timed_out_ = true;
            }
        }
        return Result<preprocess::PreprocessResult>::Failure(
            Status::Error(
                "preprocess/" + request.text,
                "ILLEGAL CODE:" + request.text,
                "downstream echoed user input: " + request.text,
                true, SideEffectState::NotStarted));
    }

    Result<preprocess::StateQueryResult> queryRuntimeState(
        const preprocess::StateQuery&,
        const CallContext&) const override {
        return Result<preprocess::StateQueryResult>::Failure(
            Status::Error("test", "UNREACHABLE",
                          "queryRuntimeState is not used"));
    }

    Result<std::vector<preprocess::StateCapability>> getCapabilities(
        const CallContext&) const override {
        return Result<std::vector<preprocess::StateCapability>>::Failure(
            Status::Error("test", "UNREACHABLE",
                          "getCapabilities is not used"));
    }

    bool completedWithoutTimeout() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return released_ && arrivals_ == parties_ && !timed_out_;
    }

private:
    const std::size_t parties_;
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::size_t arrivals_ = 0;
    mutable bool released_ = false;
    mutable bool timed_out_ = false;
};

/// Non-null AgentService dependencies that must remain unreachable because
/// BarrierFailingPreprocess terminates the turn first.
class UnreachableAgentDependencies final
    : public memory::IMemoryService,
      public intent::IIntentEngine {
public:
    Result<memory::MemoryContext> getContext(
        const interaction::StandardRequest&, const std::string&,
        const CallContext&) override {
        return Result<memory::MemoryContext>::Failure(unreachable());
    }

    Status writeTurn(
        const memory::CompletedTurn&, const CallContext&) override {
        return unreachable();
    }

    intent::IntentJobAcceptance submit(
        const interaction::StandardRequest&,
        const intent::IntentContext&, const std::string&,
        const CallContext&) override {
        intent::IntentJobAcceptance acceptance;
        acceptance.reject_code =
            "UNREACHABLE_DEPENDENCY";
        return acceptance;
    }

    Result<intent::IntentJob> getResult(
        const std::string&, const CallContext&) const override {
        return Result<intent::IntentJob>::Failure(unreachable());
    }

    Status cancel(
        const std::string&, const std::string&, std::uint64_t,
        const CallContext&) override {
        return unreachable();
    }

    Result<intent::IntentOrchestrationResult> process(
        const interaction::StandardRequest&,
        const intent::IntentContext&, const CallContext&) override {
        return Result<intent::IntentOrchestrationResult>::Failure(
            unreachable());
    }

    Status reloadRules(
        const intent::RuleSetArtifactRef&, std::uint64_t,
        const CallContext&) override {
        return unreachable();
    }

private:
    static Status unreachable() {
        return Status::Error(
            "test", "UNREACHABLE_DEPENDENCY",
            "preprocess failure must terminate the turn first");
    }
};

class UnreachableOrchestrator final
    : public orchestrator::IOrchestrator {
public:
    orchestrator::ValidationResult validateDAG(
        const orchestrator::IntentDAG&,
        const orchestrator::AdmissionContext&,
        const CallContext&) const override {
        return {};
    }

    orchestrator::PlanCommitResult submit(
        const orchestrator::OrchestratorSubmitRequest&,
        const CallContext&) override {
        return {};
    }

    orchestrator::CapacityAck updateDispatchCapacity(
        const orchestrator::DispatchCapacity&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ExecutionEventAck ackDispatch(
        const orchestrator::DispatchAcceptance&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ExecutionEventAck ackExecutionStarted(
        const orchestrator::ExecutionStarted&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ExecutionCommitResult completeExecution(
        const orchestrator::NodeExecutionResult&,
        const CallContext&) override {
        return {};
    }

    orchestrator::SignalCommitResult publishSignal(
        const orchestrator::SignalEvent&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ControlCommitResult suspend(
        const orchestrator::PlanControlRequest&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ControlCommitResult resume(
        const orchestrator::PlanControlRequest&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ControlCommitResult cancelPlan(
        const orchestrator::PlanCancelRequest&,
        const CallContext&) override {
        return {};
    }

    orchestrator::ControlCommitResult cancelActivation(
        const orchestrator::ActivationCancelRequest&,
        const CallContext&) override {
        return {};
    }

    orchestrator::CompensationCommitResult rollback(
        const orchestrator::RollbackRequest&,
        const CallContext&) override {
        return {};
    }

    Result<orchestrator::TaskPlanSnapshot> getPlan(
        const std::string&, const CallContext&) const override {
        return Result<orchestrator::TaskPlanSnapshot>::Failure(
            unreachable());
    }

    Result<orchestrator::NodeRuntimeSnapshot> getNodeStatus(
        const std::string&, const std::string&,
        const CallContext&) const override {
        return Result<orchestrator::NodeRuntimeSnapshot>::Failure(
            unreachable());
    }

    Result<orchestrator::TaskEventPage> subscribeEvents(
        const orchestrator::EventSubscriptionRequest&,
        const CallContext&) const override {
        return Result<orchestrator::TaskEventPage>::Failure(
            unreachable());
    }

    Status ackEvents(
        const orchestrator::EventAckRequest&,
        const CallContext&) override {
        return unreachable();
    }

    orchestrator::OrchestratorHealth health(
        orchestrator::HealthDetailLevel,
        const CallContext&) const override {
        return {};
    }

    bool pumpOne() override { return false; }

    Status runUntilPlanTerminal(
        const std::string&, std::size_t) override {
        return unreachable();
    }

    std::vector<orchestrator::TaskEvent> events() const override {
        return {};
    }

private:
    static Status unreachable() {
        return Status::Error(
            "test", "UNREACHABLE_DEPENDENCY",
            "preprocess failure must terminate the turn first");
    }
};

class UnreachableAtomic final
    : public atomic_service::IAtomicServiceManager {
public:
    Result<std::vector<atomic_service::McpToolDefinition>> listTools(
        const CallContext&) const override {
        return Result<std::vector<atomic_service::McpToolDefinition>>::
            Failure(unreachable());
    }

    Result<atomic_service::McpToolCatalogSnapshot>
    getToolCatalogSnapshot(const CallContext&) const override {
        return Result<atomic_service::McpToolCatalogSnapshot>::Failure(
            unreachable());
    }

    atomic_service::DispatchAcceptance callTool(
        const atomic_service::AtomicMcpCallEnvelope&,
        const CallContext&) override {
        return {};
    }

    Result<atomic_service::AtomicExecutionSnapshot> queryExecution(
        const std::string&, const CallContext&) const override {
        return Result<atomic_service::AtomicExecutionSnapshot>::Failure(
            unreachable());
    }

    Status requestPreempt(
        const std::string&, TaskPriority, std::uint64_t,
        const CallContext&) override {
        return unreachable();
    }

    Result<atomic_service::AtomicReconcileResult> reconcileExecution(
        const std::string&, const CallContext&) override {
        return Result<atomic_service::AtomicReconcileResult>::Failure(
            unreachable());
    }

    bool pumpOne() override { return false; }

    Status runUntilIdle(std::size_t) override {
        return unreachable();
    }

    std::vector<atomic_service::AtomicExecutionEvent> events()
        const override {
        return {};
    }

private:
    static Status unreachable() {
        return Status::Error(
            "test", "UNREACHABLE_DEPENDENCY",
            "preprocess failure must terminate the turn first");
    }
};

ExceptionOccurrence makeExceptionOccurrence(
    const std::string& occurrence_id,
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    ExceptionOccurrence occurrence;
    occurrence.occurrence_id = occurrence_id;
    occurrence.domain = "atomic_service";
    occurrence.code = "PROVIDER_RESPONSE_LOST";
    occurrence.source_module = "AtomicServiceManager";
    occurrence.source_interface = "callTool";
    occurrence.operation = "provider-call";
    occurrence.bounded_detail_summary = "provider-response-lost";
    occurrence.occurred_at_utc_ms = clock->utcNowMs();
    occurrence.occurred_at_mono_ns = clock->monotonicNowNs();
    return occurrence;
}

/// Freeze the authenticated producer identity before computing the checksum.
/// This mirrors AgentService's integration responsibility: ExceptionManager
/// validates and may normalize missing transport fields, but callers must not
/// ask it to checksum a different pre-normalized envelope.
///
void sealExceptionReport(
    ExceptionReportRequest& report,
    const CallContext& call) {
    report.source_redaction_proof =
        "producer-redacted:v1";
    for (auto& occurrence : report.occurrences) {
        if (occurrence.privacy_labels.empty()) {
            occurrence.privacy_labels = {
                "EXCEPTION_METADATA"};
        }
        if (occurrence.producer_endpoint_id.empty()) {
            occurrence.producer_endpoint_id =
                call.caller_endpoint_id;
        }
        if (occurrence.producer_epoch == 0) {
            occurrence.producer_epoch =
                call.caller_process_epoch;
        }
        if (occurrence.context.producer_endpoint_id.empty()) {
            occurrence.context.producer_endpoint_id =
                occurrence.producer_endpoint_id;
            occurrence.context.producer_epoch =
                occurrence.producer_epoch;
            occurrence.context.producer_sequence =
                occurrence.producer_sequence;
        }
    }
    report.batch_checksum =
        exceptionBatchChecksum(report);
}

void refreshJournalFrame(nlohmann::json& frame) {
    const auto payload = frame.at("payload").dump();
    frame["frame_length"] = payload.size();
    frame["payload_hash"] = secureDigest(payload);
    frame["crc32"] = crc32ForTest(payload);
}

void testLogIdempotencyPrivacyAndAuditChain() {
    ScopedTempDirectory temp("master-agent-log");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("log-test");
    auto tamper =
        std::make_shared<InMemoryTamperEvidenceProvider>();
    auto log =
        std::make_shared<DataLogService>(
            temp.path(), clock, ids,
            DataLogService::DurabilitySync{}, tamper);
    expect(log->initialize().ok, "log service must initialize");
    CallContext call{CallerModuleId::AgentService, "request", "trace",
                     "principal", TaskPriority::P1,
                     clock->monotonicNowNs() + 1000000000LL,
                     "observability-test-proxy", 1,
                     "trusted-observability-proxy"};

    LogEvent event;
    event.event_id = "event-1";
    event.event_type = "TEST";
    event.module = "AgentService";
    event.interface_name = "test";
    event.operation = "append";
    event.context.request_id = "request";
    event.context.trace_id = "trace";
    event.context.producer_endpoint_id = "producer";
    event.context.producer_sequence = 1;
    event.occurred_at_utc_ms = clock->utcNowMs();
    event.occurred_at_mono_ns = clock->monotonicNowNs();
    event.requested_durability = DurabilityClass::D3Fsynced;
    LogEventBatch batch;
    batch.batch_id = "batch-1";
    batch.producer_endpoint_id = "producer";
    batch.first_sequence = 1;
    batch.last_sequence = 1;
    batch.records = {event};
    auto spoofed_call = call;
    spoofed_call.caller_endpoint_id = "untrusted-writer";
    spoofed_call.authorization_ref.clear();
    const auto spoofed =
        log->appendEvents(batch, spoofed_call);
    expect(!spoofed.status.ok &&
               spoofed.status.error.code ==
                   "LOG_PRODUCER_IDENTITY_MISMATCH",
           "Event producer endpoint/epoch must bind to the authenticated "
           "caller or explicit AgentService proxy lane");
    auto appended = log->appendEvents(batch, call);
    expect(appended.status.ok && appended.value &&
               appended.value->disposition ==
                   AppendDisposition::Accepted &&
               appended.value->achieved_durability ==
                   DurabilityClass::D3Fsynced &&
               appended.value->durability_ack_id &&
               !appended.value->durability_ack_id->empty(),
           "D3 event must return an explicit fsync durability ack");
    auto replay = log->appendEvents(batch, call);
    expect(replay.value &&
               replay.value->disposition == AppendDisposition::Duplicate,
           "batch replay must not append twice");
    auto event_id_replay = batch;
    event_id_replay.batch_id = "batch-event-id-replay";
    const auto event_id_duplicate =
        log->appendEvents(event_id_replay, call);
    expect(event_id_duplicate.status.ok && event_id_duplicate.value &&
               event_id_duplicate.value->disposition ==
                   AppendDisposition::Duplicate,
           "same event_id in a new batch must remain one logical event");
    auto event_id_conflict = event_id_replay;
    event_id_conflict.batch_id = "batch-event-id-conflict";
    event_id_conflict.records.front().outcome = "changed";
    const auto conflicting_event =
        log->appendEvents(event_id_conflict, call);
    expect(!conflicting_event.status.ok &&
               conflicting_event.status.error.code ==
                   "LOG_EVENT_IDEMPOTENCY_CONFLICT",
           "event_id must bind the complete event content");
    expect(log->flush(call).ok,
           "event journal must flush before physical verification");
    const auto event_lines =
        readNonEmptyLines(temp.path() / "events.jsonl");
    TraceQuery trace_query;
    trace_query.trace_id = "trace";
    const auto trace = log->queryTrace(trace_query, call);
    expect(event_lines.size() == 1 && trace.status.ok && trace.value &&
               trace.value->events.size() == 1,
           "D3 replay must exist exactly once on disk and in trace view");

    auto valid_d3 = event;
    valid_d3.event_id = "event-d3-valid-in-rejected-batch";
    valid_d3.context.producer_sequence = 2;
    auto invalid_d3 = valid_d3;
    invalid_d3.event_id = "event-d3-private-in-rejected-batch";
    invalid_d3.context.producer_sequence = 3;
    invalid_d3.payload_summary_json = "{\"prompt\":\"secret\"}";
    LogEventBatch atomic_d3_batch;
    atomic_d3_batch.batch_id = "batch-d3-all-or-none";
    atomic_d3_batch.producer_endpoint_id = "producer";
    atomic_d3_batch.first_sequence = 2;
    atomic_d3_batch.last_sequence = 3;
    atomic_d3_batch.records = {valid_d3, invalid_d3};
    const auto atomic_rejection =
        log->appendEvents(atomic_d3_batch, call);
    expect(atomic_rejection.status.ok && atomic_rejection.value &&
               atomic_rejection.value->disposition ==
                   AppendDisposition::Rejected &&
               atomic_rejection.value->accepted_count == 0 &&
               atomic_rejection.value->rejected_count == 2,
           "a D3 batch with one invalid record must reject every record");
    expect(readNonEmptyLines(temp.path() / "events.jsonl").size() == 1,
           "rejected D3 batch must write no physical prefix");

    auto forbidden_d4_event = event;
    forbidden_d4_event.event_id = "event-d4-forbidden";
    forbidden_d4_event.context.producer_sequence = 2;
    forbidden_d4_event.requested_durability =
        DurabilityClass::D4TamperEvident;
    LogEventBatch forbidden_d4_batch;
    forbidden_d4_batch.batch_id = "batch-event-d4-forbidden";
    forbidden_d4_batch.producer_endpoint_id = "producer";
    forbidden_d4_batch.first_sequence = 2;
    forbidden_d4_batch.last_sequence = 2;
    forbidden_d4_batch.records = {forbidden_d4_event};
    const auto forbidden_d4 =
        log->appendEvents(forbidden_d4_batch, call);
    expect(!forbidden_d4.status.ok &&
               forbidden_d4.status.error.code ==
                   "LOG_EVENT_D4_REQUIRES_AUDIT",
           "D4 must be rejected on Event and routed through AuditRecord");

    auto duplicate_id_first = event;
    duplicate_id_first.event_id = "event-same-batch-duplicate";
    duplicate_id_first.context.producer_sequence = 2;
    auto duplicate_id_second = duplicate_id_first;
    duplicate_id_second.context.producer_sequence = 3;
    LogEventBatch duplicate_id_batch;
    duplicate_id_batch.batch_id = "batch-same-event-id";
    duplicate_id_batch.producer_endpoint_id = "producer";
    duplicate_id_batch.first_sequence = 2;
    duplicate_id_batch.last_sequence = 3;
    duplicate_id_batch.records = {
        duplicate_id_first, duplicate_id_second};
    const auto same_batch_duplicate =
        log->appendEvents(duplicate_id_batch, call);
    expect(!same_batch_duplicate.status.ok &&
               same_batch_duplicate.status.error.code ==
                   "LOG_EVENT_IDEMPOTENCY_CONFLICT",
           "one batch must not contain the same event_id twice");

    event.event_id = "event-private";
    event.context.producer_sequence = 2;
    event.payload_summary_json = "{\"prompt\":\"secret\"}";
    batch.batch_id = "batch-private";
    batch.first_sequence = 2;
    batch.last_sequence = 2;
    batch.records = {event};
    const auto rejected = log->appendEvents(batch, call);
    expect(rejected.value &&
               rejected.value->disposition == AppendDisposition::Rejected,
           "full Prompt must be rejected by second redaction gate");

    AuditRecord audit;
    audit.audit_id = "audit-1";
    audit.audit_type = "SideEffect";
    audit.context = event.context;
    audit.context.producer_endpoint_id = "agent-service";
    audit.context.producer_epoch = 1;
    audit.context.producer_sequence = 1;
    audit.actor_id_hash = "actor";
    audit.actor_role = "system";
    audit.subject_id_hash = "vehicle";
    audit.action = "climate.set";
    audit.interface_name = "tools/call";
    audit.decision = "allow";
    audit.policy_id = "policy";
    audit.policy_version = "1";
    audit.side_effect_state = SideEffectState::Committed;
    audit.occurred_at_utc_ms = clock->utcNowMs();
    audit.requested_durability = DurabilityClass::D4TamperEvident;
    AuditBatch audits;
    audits.batch_id = "audit-batch";
    audits.producer_endpoint_id = "agent-service";
    audits.first_sequence = 1;
    audits.last_sequence = 1;
    audits.records = {audit};
    const auto audit_result = log->appendAudit(audits, call);
    expect(audit_result.status.ok && audit_result.value &&
               audit_result.value->disposition ==
                   AppendDisposition::Accepted &&
               audit_result.value->achieved_durability ==
                   DurabilityClass::D4TamperEvident &&
               !audit_result.value->durability_ack_id.empty() &&
               audit_result.value->hash_chain_head.size() == 64,
           "D4 audit must return an explicit tamper-evident durability ack");
    const auto audit_replay = log->appendAudit(audits, call);
    expect(audit_replay.status.ok && audit_replay.value &&
               audit_replay.value->disposition ==
                   AppendDisposition::Duplicate,
           "D4 batch replay must return the original commit");
    auto audit_id_replay = audits;
    audit_id_replay.batch_id = "audit-batch-new-transport";
    const auto audit_id_duplicate =
        log->appendAudit(audit_id_replay, call);
    expect(audit_id_duplicate.status.ok && audit_id_duplicate.value &&
               audit_id_duplicate.value->disposition ==
                   AppendDisposition::Duplicate,
           "same audit_id in a new batch must not extend the hash chain");
    auto audit_id_conflict = audit_id_replay;
    audit_id_conflict.batch_id = "audit-batch-conflict";
    audit_id_conflict.records.front().decision = "deny";
    const auto conflicting_audit =
        log->appendAudit(audit_id_conflict, call);
    expect(!conflicting_audit.status.ok &&
               conflicting_audit.status.error.code ==
                   "AUDIT_IDEMPOTENCY_CONFLICT",
           "audit_id must bind the complete audit content");

    auto expired_audit = audit;
    expired_audit.audit_id = "audit-expired";
    expired_audit.context.producer_sequence = 2;
    AuditBatch expired_audits;
    expired_audits.batch_id = "audit-batch-expired";
    expired_audits.producer_endpoint_id = "agent-service";
    expired_audits.first_sequence = 2;
    expired_audits.last_sequence = 2;
    expired_audits.records = {expired_audit};
    auto expired_call = call;
    expired_call.deadline_mono_ns = clock->monotonicNowNs();
    const auto expired_result =
        log->appendAudit(expired_audits, expired_call);
    expect(!expired_result.status.ok &&
               expired_result.status.error.code ==
                   "AUDIT_CALL_EXPIRED",
           "Audit must honor the caller's monotonic deadline");

    auto valid_audit_in_rejected_batch = audit;
    valid_audit_in_rejected_batch.audit_id =
        "audit-valid-in-rejected-batch";
    valid_audit_in_rejected_batch.context.producer_sequence = 2;
    auto invalid_audit_in_rejected_batch =
        valid_audit_in_rejected_batch;
    invalid_audit_in_rejected_batch.audit_id =
        "audit-invalid-in-rejected-batch";
    invalid_audit_in_rejected_batch.context.producer_sequence = 3;
    invalid_audit_in_rejected_batch.requested_durability =
        DurabilityClass::D2Journaled;
    AuditBatch invalid_atomic_audits;
    invalid_atomic_audits.batch_id = "audit-batch-all-or-none";
    invalid_atomic_audits.producer_endpoint_id = "agent-service";
    invalid_atomic_audits.first_sequence = 2;
    invalid_atomic_audits.last_sequence = 3;
    invalid_atomic_audits.records = {
        valid_audit_in_rejected_batch,
        invalid_audit_in_rejected_batch};
    const auto invalid_atomic_audit =
        log->appendAudit(invalid_atomic_audits, call);
    expect(!invalid_atomic_audit.status.ok &&
               invalid_atomic_audit.status.error.code ==
                   "AUDIT_RECORD_INVALID" &&
               readNonEmptyLines(temp.path() / "audit.jsonl").size() ==
                   1,
           "invalid Audit batch must not commit its valid physical prefix");
    expect(log->flush(call).ok,
           "audit journal must flush before hash verification");
    const auto audit_lines =
        readNonEmptyLines(temp.path() / "audit.jsonl");
    expect(audit_lines.size() == 1,
           "D4 replay must not append a second physical record");
    const auto persisted_batch =
        nlohmann::json::parse(audit_lines.front());
    expect(persisted_batch.at("_journal_kind") == "audit_batch" &&
               persisted_batch.at("records").size() == 1,
           "one physical frame must contain the complete Audit batch");
    auto persisted_audit =
        persisted_batch.at("records").front();
    const auto stored_hash =
        persisted_audit.at("hash").get<std::string>();
    expect(persisted_audit.at("previous_hash") == "GENESIS",
           "first D4 record must bind the GENESIS head");
    const auto anchor = tamper->anchor();
    expect(stored_hash ==
               audit_result.value->hash_chain_head &&
               audit_result.value->key_generation ==
                   "host-test-key-generation-1" &&
               persisted_audit.at("_key_generation") ==
                   audit_result.value->key_generation &&
               persisted_audit.at("_anchor_generation") == 1 &&
               anchor.present && anchor.generation == 1 &&
               anchor.hash_chain_head == stored_hash &&
               !anchor.authentication_code.empty(),
           "D4 ACK must bind HMAC key generation and trusted anchor");
}

void testLogRestartRecoveryAndAuditContinuation() {
    ScopedTempDirectory temp("master-agent-log-recovery");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto tamper =
        std::make_shared<InMemoryTamperEvidenceProvider>();
    CallContext call{CallerModuleId::AgentService,
                     "request-log-recovery", "trace-log-recovery",
                     "principal", TaskPriority::P1,
                     clock->monotonicNowNs() + 1'000'000'000LL,
                     "observability-test-proxy", 1,
                     "trusted-observability-proxy"};

    LogEvent event;
    event.event_id = "event-recovery";
    event.event_type = "RECOVERY_TEST";
    event.module = "AgentService";
    event.interface_name = "IDataLogService";
    event.operation = "appendEvents";
    event.context.request_id = call.request_id;
    event.context.trace_id = call.trace_id;
    event.context.span_id = "span-recovery";
    event.context.session_id = "session-recovery";
    event.context.plan_id = "plan-recovery";
    event.context.execution_id = "execution-recovery";
    event.context.producer_endpoint_id = "recovery-producer";
    event.context.producer_epoch = 9;
    event.context.producer_sequence = 1;
    event.context.task_priority = TaskPriority::P1;
    event.context.deadline_mono_ns = call.deadline_mono_ns;
    event.outcome = "success";
    event.occurred_at_utc_ms = clock->utcNowMs();
    event.occurred_at_mono_ns = clock->monotonicNowNs();
    event.requested_durability = DurabilityClass::D3Fsynced;
    event.privacy_labels = {"diagnostic"};
    event.payload_summary_json = "{\"recovered\":true}";
    LogEventBatch event_batch;
    event_batch.batch_id = "event-batch-recovery";
    event_batch.producer_endpoint_id =
        event.context.producer_endpoint_id;
    event_batch.producer_epoch = event.context.producer_epoch;
    event_batch.first_sequence = 1;
    event_batch.last_sequence = 1;
    event_batch.checksum = "event-checksum";
    event_batch.redaction_proof = "event-redaction-proof";
    event_batch.records = {event};

    AuditRecord audit;
    audit.audit_id = "audit-recovery-1";
    audit.audit_type = "Authorization";
    audit.context.request_id = call.request_id;
    audit.context.trace_id = call.trace_id;
    audit.context.producer_endpoint_id = "recovery-audit";
    audit.context.producer_epoch = 4;
    audit.context.producer_sequence = 1;
    audit.context.task_priority = TaskPriority::P1;
    audit.context.deadline_mono_ns = call.deadline_mono_ns;
    audit.actor_id_hash = "actor";
    audit.actor_role = "system";
    audit.subject_id_hash = "subject";
    audit.action = "authorize";
    audit.interface_name = "admission";
    audit.decision = "allow";
    audit.policy_id = "policy";
    audit.policy_version = "1";
    audit.occurred_at_utc_ms = clock->utcNowMs();
    audit.occurred_at_mono_ns = clock->monotonicNowNs();
    audit.requested_durability =
        DurabilityClass::D4TamperEvident;
    AuditBatch audit_batch;
    audit_batch.batch_id = "audit-batch-recovery-1";
    audit_batch.producer_endpoint_id =
        audit.context.producer_endpoint_id;
    audit_batch.producer_epoch = audit.context.producer_epoch;
    audit_batch.first_sequence = 1;
    audit_batch.last_sequence = 1;
    audit_batch.checksum = "audit-checksum-1";
    audit_batch.redaction_proof = "audit-redaction-proof";
    audit_batch.records = {audit};

    std::string first_chain_head;
    {
        auto log = std::make_shared<DataLogService>(
            temp.path(), clock,
            std::make_shared<IdGenerator>("log-recovery-first"),
            DataLogService::DurabilitySync{}, tamper);
        expect(log->initialize().ok,
               "first DataLog process must initialize");
        const auto event_result =
            log->appendEvents(event_batch, call);
        const auto audit_result =
            log->appendAudit(audit_batch, call);
        expect(event_result.status.ok && audit_result.value,
               "seed Event and Audit must be durable");
        first_chain_head = audit_result.value->hash_chain_head;
    }

    const auto event_path = temp.path() / "events.jsonl";
    const auto audit_path = temp.path() / "audit.jsonl";
    const auto event_lines_before =
        readNonEmptyLines(event_path).size();
    const auto audit_lines_before =
        readNonEmptyLines(audit_path).size();
    {
        auto recovered = std::make_shared<DataLogService>(
            temp.path(), clock,
            std::make_shared<IdGenerator>("log-recovery-second"),
            DataLogService::DurabilitySync{}, tamper);
        expect(recovered->initialize().ok,
               "DataLog restart must validate and rebuild indexes");
        TraceQuery query;
        query.trace_id = call.trace_id;
        const auto trace = recovered->queryTrace(query, call);
        const auto event_replay =
            recovered->appendEvents(event_batch, call);
        const auto audit_replay =
            recovered->appendAudit(audit_batch, call);
        expect(trace.value && trace.value->events.size() == 1 &&
                   trace.value->events.front().context.session_id ==
                       event.context.session_id &&
                   trace.value->events.front().privacy_labels ==
                       event.privacy_labels &&
                   event_replay.value &&
                   event_replay.value->disposition ==
                       AppendDisposition::Duplicate &&
                   audit_replay.value &&
                   audit_replay.value->disposition ==
                       AppendDisposition::Duplicate &&
                   readNonEmptyLines(event_path).size() ==
                       event_lines_before &&
                   readNonEmptyLines(audit_path).size() ==
                       audit_lines_before,
               "restart must restore trace and BatchID/RecordID idempotency without reappend");

        auto next_audit = audit;
        next_audit.audit_id = "audit-recovery-2";
        next_audit.context.producer_sequence = 2;
        next_audit.action = "continue-chain";
        AuditBatch next_batch;
        next_batch.batch_id = "audit-batch-recovery-2";
        next_batch.producer_endpoint_id =
            next_audit.context.producer_endpoint_id;
        next_batch.producer_epoch =
            next_audit.context.producer_epoch;
        next_batch.first_sequence = 2;
        next_batch.last_sequence = 2;
        next_batch.checksum = "audit-checksum-2";
        next_batch.redaction_proof = "audit-redaction-proof";
        next_batch.records = {next_audit};
        const auto continued =
            recovered->appendAudit(next_batch, call);
        expect(continued.status.ok && continued.value &&
                   continued.value->hash_chain_head !=
                       first_chain_head,
               "new Audit after restart must continue the recovered chain");
        const auto audit_lines = readNonEmptyLines(audit_path);
        expect(audit_lines.size() == audit_lines_before + 1 &&
                   nlohmann::json::parse(audit_lines.back())
                           .at("records")
                           .front()
                           .at("previous_hash") ==
                       first_chain_head,
               "continued Audit frame must reference the pre-restart head");
    }

    auto corrupted = readAllBytes(audit_path);
    const auto decision = corrupted.rfind("\"decision\":\"allow\"");
    expect(decision != std::string::npos,
           "persisted Audit must contain the test decision");
    corrupted[decision + std::string{"\"decision\":\""}.size()] =
        'x';
    writeAllBytes(audit_path, corrupted);
    DataLogService integrity_check(
        temp.path(), clock,
        std::make_shared<IdGenerator>("log-recovery-corrupt"),
        DataLogService::DurabilitySync{}, tamper);
    const auto integrity = integrity_check.initialize();
    expect(!integrity.ok &&
               integrity.error.code ==
                   "LOG_AUDIT_JOURNAL_INTEGRITY" &&
               !integrity_check.getHealth(call).ready,
           "committed Audit tampering must keep DataLog out of Ready");
}

void testD4RequiresKeyedAnchorAndDetectsRollback() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    CallContext call{
        CallerModuleId::AgentService, "request-d4",
        "trace-d4", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "observability-test-proxy", 1,
        "trusted-observability-proxy"};
    const auto make_batch = [&](std::uint64_t sequence,
                                const std::string& suffix) {
        AuditRecord audit;
        audit.audit_id = "audit-d4-" + suffix;
        audit.audit_type = "Authorization";
        audit.context.request_id = call.request_id;
        audit.context.trace_id = call.trace_id;
        audit.context.producer_endpoint_id = "d4-producer";
        audit.context.producer_epoch = 81;
        audit.context.producer_sequence = sequence;
        audit.actor_id_hash = "actor";
        audit.actor_role = "system";
        audit.subject_id_hash = "subject";
        audit.action = "authorize";
        audit.interface_name = "admission";
        audit.decision = "allow";
        audit.policy_id = "policy";
        audit.policy_version = "contract-v2";
        audit.requested_durability =
            DurabilityClass::D4TamperEvident;
        AuditBatch batch;
        batch.batch_id = "audit-d4-batch-" + suffix;
        batch.producer_endpoint_id =
            audit.context.producer_endpoint_id;
        batch.producer_epoch =
            audit.context.producer_epoch;
        batch.first_sequence = sequence;
        batch.last_sequence = sequence;
        batch.records = {audit};
        return batch;
    };

    {
        ScopedTempDirectory temp(
            "master-agent-d4-no-provider");
        DataLogService log(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "d4-no-provider"));
        expect(log.initialize().ok,
               "D3 DataLog must initialize without a D4 provider");
        expect(!log.getHealth(call).d4_ready,
               "general Ready must not imply D4 capability");
        const auto denied =
            log.appendAudit(make_batch(1, "no-provider"), call);
        expect(!denied.status.ok &&
                   denied.status.error.code ==
                       "AUDIT_D4_PROVIDER_UNAVAILABLE" &&
                   readNonEmptyLines(
                       temp.path() / "audit.jsonl")
                       .empty(),
               "D4 must never ACK or write without a key/anchor provider");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-d4-key-unavailable");
        auto provider =
            std::make_shared<InMemoryTamperEvidenceProvider>();
        provider->setKeyUnavailable(true);
        DataLogService log(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "d4-key-unavailable"),
            DataLogService::DurabilitySync{}, provider);
        const auto initialized = log.initialize();
        expect(!initialized.ok &&
                   initialized.error.code ==
                       "LOG_D4_PROVIDER_UNAVAILABLE",
               "configured D4 provider key outage must keep Ready closed");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-d4-anchor-failure");
        auto provider =
            std::make_shared<InMemoryTamperEvidenceProvider>();
        const auto batch =
            make_batch(1, "anchor-failure");
        {
            DataLogService log(
                temp.path(), clock,
                std::make_shared<IdGenerator>(
                    "d4-anchor-failure-first"),
                DataLogService::DurabilitySync{}, provider);
            expect(log.initialize().ok,
                   "D4 anchor-failure fixture must initialize");
            provider->failNextCommit();
            const auto failed =
                log.appendAudit(batch, call);
            const auto retry =
                log.appendAudit(batch, call);
            expect(!failed.status.ok &&
                       failed.status.error.code ==
                           "TEST_D4_ANCHOR_COMMIT_FAILED" &&
                       failed.status.error.side_effect_state ==
                           SideEffectState::Unknown &&
                       !retry.status.ok &&
                       readNonEmptyLines(
                           temp.path() / "audit.jsonl")
                               .size() == 1 &&
                       !log.getHealth(call).ready,
                   "anchor failure after fsync must be stable UNKNOWN");
        }
        DataLogService recovery(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "d4-anchor-failure-recovery"),
            DataLogService::DurabilitySync{}, provider);
        const auto recovered = recovery.initialize();
        expect(!recovered.ok &&
                   recovered.error.code ==
                       "LOG_AUDIT_ANCHOR_MISSING",
               "journal-ahead-of-anchor crash window must fail closed");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-d4-rollback");
        auto provider =
            std::make_shared<InMemoryTamperEvidenceProvider>();
        std::string generation_one_frame;
        {
            DataLogService log(
                temp.path(), clock,
                std::make_shared<IdGenerator>(
                    "d4-rollback-first"),
                DataLogService::DurabilitySync{}, provider);
            expect(log.initialize().ok,
                   "D4 rollback fixture must initialize");
            expect(log.getHealth(call).d4_ready &&
                       log.getHealth(call).active_key_generation ==
                           "host-test-key-generation-1",
                   "health must expose active D4 capability and key generation");
            const auto first =
                log.appendAudit(make_batch(1, "generation-1"), call);
            const auto first_lines = readNonEmptyLines(
                temp.path() / "audit.jsonl");
            expect(first.value &&
                       first.value->key_generation ==
                           "host-test-key-generation-1" &&
                       first_lines.size() == 1,
                   "first D4 generation must be anchored");
            generation_one_frame = first_lines.front();
            const auto second =
                log.appendAudit(make_batch(2, "generation-2"), call);
            expect(second.value &&
                       provider->anchor().generation == 2,
                   "second D4 generation must advance the trusted anchor");
        }
        writeAllBytes(
            temp.path() / "audit.jsonl",
            generation_one_frame + "\n");
        DataLogService rolled_back(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "d4-rollback-recovery"),
            DataLogService::DurabilitySync{}, provider);
        const auto initialized =
            rolled_back.initialize();
        expect(!initialized.ok &&
                   initialized.error.code ==
                       "LOG_AUDIT_ROLLBACK_DETECTED" &&
                   !rolled_back.getHealth(call).ready,
               "trusted anchor must detect a valid old journal prefix");
    }
}

/// A successful provider return is not itself a D4 acknowledgement: the
/// exact anchor must be observable through the independent readback path.
void testD4AnchorReadbackRejectsFalseAcknowledgement() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    CallContext call{
        CallerModuleId::AgentService, "request-d4-readback",
        "trace-d4-readback", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "observability-test-proxy", 1,
        "trusted-observability-proxy"};

    const auto exercise =
        [&](InMemoryTamperEvidenceProvider::CommitBehavior behavior,
            const std::string& suffix) {
            ScopedTempDirectory temp(
                "master-agent-d4-false-ack-" + suffix);
            auto provider =
                std::make_shared<InMemoryTamperEvidenceProvider>();
            DataLogService log(
                temp.path(), clock,
                std::make_shared<IdGenerator>(
                    "d4-false-ack-" + suffix),
                DataLogService::DurabilitySync{}, provider);
            expect(log.initialize().ok,
                   "D4 false-ack fixture must initialize");

            AuditRecord audit;
            audit.audit_id = "audit-d4-false-ack-" + suffix;
            audit.audit_type = "SideEffect";
            audit.context.request_id = call.request_id;
            audit.context.trace_id = call.trace_id;
            audit.context.producer_endpoint_id =
                "d4-false-ack-producer";
            audit.context.producer_epoch = 82;
            audit.context.producer_sequence = 1;
            audit.context.task_priority = TaskPriority::P1;
            audit.context.deadline_mono_ns =
                call.deadline_mono_ns;
            audit.actor_id_hash = "actor";
            audit.actor_role = "system";
            audit.subject_id_hash = "subject";
            audit.action = "verify-anchor";
            audit.interface_name = "ITamperEvidenceProvider";
            audit.decision = "allow";
            audit.policy_id = "d4-policy";
            audit.policy_version = "contract-v2";
            audit.side_effect_state = SideEffectState::Committed;
            audit.occurred_at_utc_ms = clock->utcNowMs();
            audit.occurred_at_mono_ns =
                clock->monotonicNowNs();
            audit.requested_durability =
                DurabilityClass::D4TamperEvident;

            AuditBatch batch;
            batch.batch_id =
                "audit-d4-false-ack-batch-" + suffix;
            batch.producer_endpoint_id =
                audit.context.producer_endpoint_id;
            batch.producer_epoch =
                audit.context.producer_epoch;
            batch.first_sequence = 1;
            batch.last_sequence = 1;
            batch.records = {audit};

            provider->setCommitBehavior(behavior);
            const auto result = log.appendAudit(batch, call);
            expect(!result.status.ok &&
                       result.status.error.code ==
                           "AUDIT_D4_ANCHOR_READBACK_MISMATCH" &&
                       result.status.error.side_effect_state ==
                           SideEffectState::Unknown,
                   "D4 must return UNKNOWN when commitAnchor reports OK "
                   "without an exact observable anchor");

            const auto health = log.getHealth(call);
            expect(!health.ready &&
                       health.audit_integrity_degraded &&
                       !health.d4_ready,
                   "an unconfirmed D4 commit must degrade health and "
                   "fence further trusted audit acknowledgements");
        };

    exercise(
        InMemoryTamperEvidenceProvider::CommitBehavior::
            AcknowledgeWithoutStore,
        "unchanged");
    exercise(
        InMemoryTamperEvidenceProvider::CommitBehavior::
            AcknowledgeWrongAnchor,
        "wrong-anchor");
}

/// Payload summaries are structured, bounded JSON. Forbidden raw fields
/// cannot be hidden through case changes, JSON escapes, or nesting.
void testLogRejectsStructuredPrivacyBypasses() {
    ScopedTempDirectory temp(
        "master-agent-log-privacy-bypasses");
    auto clock = std::make_shared<ManualRuntimeClock>();
    DataLogService log(
        temp.path(), clock,
        std::make_shared<IdGenerator>("privacy-bypass"));
    expect(log.initialize().ok,
           "privacy-bypass DataLog fixture must initialize");

    CallContext call{
        CallerModuleId::AgentService, "request-privacy",
        "trace-privacy", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "privacy-producer", 73};
    const auto append =
        [&](const std::string& suffix,
            const std::string& event_type,
            const std::string& payload,
            std::uint64_t sequence) {
        LogEvent event;
        event.event_id = "privacy-event-" + suffix;
        event.event_type = event_type;
        event.module = "AgentService";
        event.interface_name = "IDataLogService";
        event.operation = "appendEvents";
        event.context.request_id = call.request_id;
        event.context.trace_id = call.trace_id;
        event.context.producer_endpoint_id =
            call.caller_endpoint_id;
        event.context.producer_epoch =
            call.caller_process_epoch;
        event.context.producer_sequence = sequence;
        event.context.task_priority = call.priority;
        event.context.deadline_mono_ns =
            call.deadline_mono_ns;
        event.occurred_at_utc_ms = clock->utcNowMs();
        event.occurred_at_mono_ns =
            clock->monotonicNowNs();
        event.requested_durability =
            DurabilityClass::D3Fsynced;
        event.payload_summary_json = payload;

        LogEventBatch batch;
        batch.batch_id = "privacy-batch-" + suffix;
        batch.producer_endpoint_id =
            event.context.producer_endpoint_id;
        batch.producer_epoch =
            event.context.producer_epoch;
        batch.first_sequence = sequence;
        batch.last_sequence = sequence;
        batch.records = {event};
        return log.appendEvents(batch, call);
    };

    const std::vector<std::pair<std::string, std::string>>
        rejected_payloads{
            {"invalid-json", R"({"safe":)"},
            {"mixed-case", R"({"RaW_OuTpUt":"secret"})"},
            {"escaped-key",
             R"({"raw\u005foutput":"secret"})"},
            {"nested-key",
             R"({"outer":{"items":[{"raw_output":"secret"}]}})"},
            {"user-input",
             R"({"user_input":"raw-user-input"})"},
            {"user-query",
             R"({"user_query":"raw-user-query"})"},
            {"query", R"({"query":"raw-query"})"},
            {"message", R"({"message":"raw-message"})"},
            {"raw-prompt",
             R"({"raw_prompt":"raw-prompt"})"},
            {"location",
             R"({"location":"raw-user-location"})"},
            {"arbitrary-content",
             R"({"content":"raw-user-content","request_linked":true})"},
            {"duplicate-json-key",
             R"({"content":"redacted","content":"redacted","request_linked":true})"}};

    for (const auto& [suffix, payload] : rejected_payloads) {
        const auto result =
            append(suffix, "PRIVACY_GATE_TEST", payload, 1);
        expect(result.status.ok && result.value &&
                   result.value->disposition ==
                       AppendDisposition::Rejected &&
                   result.value->accepted_count == 0 &&
                   result.value->rejected_count == 1,
               "invalid or privacy-bearing JSON must be rejected "
               "before any D3 record is committed");
        expect(readNonEmptyLines(
                   temp.path() / "events.jsonl")
                   .empty(),
               "every schema/privacy rejection must leave zero "
               "physical Event frames");
    }

    const std::vector<
        std::tuple<std::string, std::string, std::string>>
        accepted_payloads{
            {"empty-object", "SCHEMA_EMPTY", "{}"},
            {"agent-service-redacted", "TURN_ACCEPTED",
             R"({"content":"redacted","request_linked":true})"},
            {"exception-occurrence",
             "EXCEPTION_OCCURRENCE_ACCEPTED",
             R"({"domain":"preprocess","code":"PREPROCESS_FAILED","exception_id":"exception-1","transaction_id":"exception-report-1"})"},
            {"exception-lifecycle",
             "EXCEPTION_LIFECYCLE_MUTATED",
             R"({"exception_id":"exception-1","actor_id_hash":"operator-hash","actor_role":"ops","reason_code":"ACKNOWLEDGED","verification_evidence_refs":["event:1"],"resolution_waiver_id":"","transaction_id":"exception-mutation-1"})"}};

    std::uint64_t sequence = 1;
    for (const auto& [suffix, event_type, payload] :
         accepted_payloads) {
        const auto result =
            append(suffix, event_type, payload, sequence++);
        expect(result.status.ok && result.value &&
                   result.value->disposition ==
                       AppendDisposition::Accepted &&
                   result.value->accepted_count == 1 &&
                   result.value->rejected_count == 0,
               "the frozen empty, AgentService-redacted and Exception "
               "payload schemas must remain admissible");
    }
    expect(readNonEmptyLines(
               temp.path() / "events.jsonl")
               .size() == accepted_payloads.size(),
           "only the four explicitly accepted payload schemas may "
           "create physical Event frames");
}

/// Audit accepts only frozen decision facts and stable identifiers. Moving
/// raw user text into nominal hash/action/reference fields, or omitting the
/// producer redaction contract, must reject the complete batch before the
/// Audit chain or producer watermark advances.
void testAuditPrivacyContractIsAtomicAndRecoverable() {
    ScopedTempDirectory temp(
        "master-agent-audit-privacy-contract");
    auto clock = std::make_shared<ManualRuntimeClock>();
    const auto storage = temp.path() / "data-log";
    const auto audit_path = storage / "audit.jsonl";
    constexpr std::uint64_t producer_epoch = 801;
    const std::string producer = "audit-privacy-producer";
    const std::string raw_user_input =
        u8"请把我的完整家庭住址发送给客服 raw user query";
    CallContext writer{
        CallerModuleId::AgentService,
        "audit-privacy-request", "audit-privacy-trace",
        "audit-privacy-principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        producer, producer_epoch};
    CallContext observer{
        CallerModuleId::AgentService,
        "audit-observer-request", "audit-observer-trace",
        "audit-observer-principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};

    const auto make_record =
        [&](const std::string& suffix,
            std::uint64_t sequence) {
            AuditRecord audit;
            audit.audit_id =
                "audit-privacy-" + suffix + "-" +
                std::to_string(sequence);
            audit.audit_type = "Authorization";
            audit.context.request_id = writer.request_id;
            audit.context.trace_id = writer.trace_id;
            audit.context.span_id =
                "audit-span-" + std::to_string(sequence);
            audit.context.producer_endpoint_id = producer;
            audit.context.producer_epoch = producer_epoch;
            audit.context.producer_sequence = sequence;
            audit.context.task_priority = writer.priority;
            audit.context.deadline_mono_ns =
                writer.deadline_mono_ns;
            audit.actor_id_hash = "actor-hash-001";
            audit.actor_role = "system";
            audit.delegated_by_hash = "delegate-hash-001";
            audit.subject_id_hash = "subject-hash-001";
            audit.action = "authorize";
            audit.interface_name = "admission-v2";
            audit.capability_id = "vehicle.climate.write";
            audit.object_refs = {"vehicle:climate"};
            audit.object_versions = {"version:1"};
            audit.decision = "allow";
            audit.policy_id = "policy-audit-v2";
            audit.policy_version = "v1";
            audit.evidence_hashes = {"sha256:evidence001"};
            audit.before_fact_summary = "state:before";
            audit.after_fact_summary = "state:after";
            audit.side_effect_state =
                SideEffectState::NotStarted;
            audit.privacy_labels = {"AUDIT_METADATA"};
            audit.redaction_policy_version = "redaction-v1";
            audit.retention_class = "AUDIT_STANDARD";
            audit.legal_hold_id = "hold:review001";
            audit.occurred_at_utc_ms = clock->utcNowMs();
            audit.occurred_at_mono_ns =
                clock->monotonicNowNs();
            audit.requested_durability =
                DurabilityClass::D3Fsynced;
            return audit;
        };
    const auto make_batch =
        [&](const std::string& suffix) {
            AuditBatch batch;
            batch.batch_id = "audit-privacy-batch-" + suffix;
            batch.producer_endpoint_id = producer;
            batch.producer_epoch = producer_epoch;
            batch.first_sequence = 1;
            batch.last_sequence = 2;
            batch.checksum = "checksum:" + suffix;
            batch.redaction_proof = "proof:redaction-v1";
            batch.records = {
                make_record(suffix, 1),
                make_record(suffix, 2)};
            return batch;
        };

    using InvalidAuditMutation =
        std::function<void(AuditBatch&)>;
    const std::vector<
        std::pair<std::string, InvalidAuditMutation>>
        invalid_cases{
            {"actor-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().actor_id_hash =
                     raw_user_input;
             }},
            {"subject-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().subject_id_hash =
                     raw_user_input;
             }},
            {"audit-type-json",
             [&](AuditBatch& batch) {
                 batch.records.back().audit_type =
                     R"({"query":"raw"})";
             }},
            {"actor-role-whitespace",
             [](AuditBatch& batch) {
                 batch.records.back().actor_role =
                     "system operator";
             }},
            {"action-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().action = raw_user_input;
             }},
            {"interface-json",
             [](AuditBatch& batch) {
                 batch.records.back().interface_name =
                     R"({"interface":"raw"})";
             }},
            {"decision-prose",
             [](AuditBatch& batch) {
                 batch.records.back().decision =
                     "allow because the user requested it";
             }},
            {"policy-id-whitespace",
             [](AuditBatch& batch) {
                 batch.records.back().policy_id =
                     "policy snapshot v1";
             }},
            {"empty-policy-version",
             [](AuditBatch& batch) {
                 batch.records.back().policy_version.clear();
             }},
            {"empty-redaction-proof",
             [](AuditBatch& batch) {
                 batch.redaction_proof.clear();
             }},
            {"raw-redaction-proof",
             [&](AuditBatch& batch) {
                 batch.redaction_proof = raw_user_input;
             }},
            {"empty-privacy-labels",
             [](AuditBatch& batch) {
                 batch.records.back().privacy_labels.clear();
             }},
            {"raw-privacy-label",
             [&](AuditBatch& batch) {
                 batch.records.back().privacy_labels = {
                     raw_user_input};
             }},
            {"empty-redaction-policy",
             [](AuditBatch& batch) {
                 batch.records.back()
                     .redaction_policy_version.clear();
             }},
            {"delegated-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().delegated_by_hash =
                     raw_user_input;
             }},
            {"capability-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().capability_id =
                     raw_user_input;
             }},
            {"object-ref-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().object_refs = {
                     raw_user_input};
             }},
            {"object-version-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().object_versions = {
                     raw_user_input};
             }},
            {"object-version-cardinality",
             [](AuditBatch& batch) {
                 batch.records.back().object_versions = {
                     "version:1", "version:2"};
             }},
            {"evidence-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().evidence_hashes = {
                     raw_user_input};
             }},
            {"before-fact-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().before_fact_summary =
                     raw_user_input;
             }},
            {"after-fact-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().after_fact_summary =
                     raw_user_input;
             }},
            {"retention-prose",
             [](AuditBatch& batch) {
                 batch.records.back().retention_class =
                     "retain for customer request";
             }},
            {"legal-hold-user-input",
             [&](AuditBatch& batch) {
                 batch.records.back().legal_hold_id =
                     raw_user_input;
             }}};

    AuditBatch accepted_batch = make_batch("accepted");
    std::string accepted_chain_head;
    {
        DataLogService log(
            storage, clock,
            std::make_shared<IdGenerator>(
                "audit-privacy-first"));
        expect(log.initialize().ok,
               "Audit privacy fixture must initialize");
        for (const auto& [suffix, mutate] : invalid_cases) {
            auto invalid = make_batch(suffix);
            mutate(invalid);
            const auto rejected =
                log.appendAudit(invalid, writer);
            const auto health = log.getHealth(observer);
            expect(!rejected.status.ok &&
                       !rejected.status.error.code.empty(),
                   "privacy-invalid Audit batch must fail closed: " +
                       suffix);
            expect(readNonEmptyLines(audit_path).empty() &&
                       health.audit_records == 0 &&
                       health.hash_chain_head == "GENESIS",
                   "privacy-invalid Audit batch must not append a "
                   "physical prefix or advance the chain: " +
                       suffix);
        }

        const auto accepted =
            log.appendAudit(accepted_batch, writer);
        const auto health = log.getHealth(observer);
        expect(accepted.status.ok && accepted.value &&
                   accepted.value->disposition ==
                       AppendDisposition::Accepted &&
                   accepted.value->accepted_count == 2 &&
                   accepted.value->achieved_durability ==
                       DurabilityClass::D3Fsynced &&
                   health.audit_records == 2 &&
                   readNonEmptyLines(audit_path).size() == 1,
               "valid Audit sequence 1-2 must succeed after every "
               "rejection, proving the watermark never advanced");
        accepted_chain_head =
            accepted.value->hash_chain_head;
        const auto persisted =
            nlohmann::json::parse(
                readNonEmptyLines(audit_path).front());
        expect(persisted.at("records").size() == 2,
               "valid Audit batch must persist atomically in one frame");
        for (const auto& record : persisted.at("records")) {
            expect(record.at("privacy_labels") ==
                       nlohmann::json::array(
                           {"AUDIT_METADATA"}) &&
                       record.at("redaction_policy_version") ==
                           "redaction-v1" &&
                       record.at("retention_class") ==
                           "AUDIT_STANDARD" &&
                       record.at("delegated_by_hash") ==
                           "delegate-hash-001" &&
                       record.at("capability_id") ==
                           "vehicle.climate.write" &&
                       record.at("object_refs") ==
                           nlohmann::json::array(
                               {"vehicle:climate"}) &&
                       record.at("object_versions") ==
                           nlohmann::json::array(
                               {"version:1"}) &&
                       record.at("evidence_hashes") ==
                           nlohmann::json::array(
                               {"sha256:evidence001"}) &&
                       record.at("legal_hold_id") ==
                           "hold:review001",
                   "the complete frozen Audit privacy/retention "
                   "contract must be persisted");
        }
        expect(readAllBytes(audit_path).find(raw_user_input) ==
                   std::string::npos,
               "rejected user plaintext must never reach Audit storage");
    }

    {
        DataLogService recovered(
            storage, clock,
            std::make_shared<IdGenerator>(
                "audit-privacy-recovered"));
        expect(recovered.initialize().ok,
               "valid frozen Audit contract must recover after restart");
        const auto recovered_health =
            recovered.getHealth(observer);
        expect(recovered_health.ready &&
                   recovered_health.audit_records == 2 &&
                   recovered_health.hash_chain_head ==
                       accepted_chain_head,
               "restart must restore the Audit records and chain head");
        const auto replay =
            recovered.appendAudit(accepted_batch, writer);
        expect(replay.status.ok && replay.value &&
                   replay.value->disposition ==
                       AppendDisposition::Duplicate &&
                   replay.value->hash_chain_head ==
                       accepted_chain_head &&
                   readNonEmptyLines(audit_path).size() == 1,
               "recovered Audit BatchID replay must not append");

        auto continuation = make_batch("continuation");
        continuation.batch_id =
            "audit-privacy-batch-continuation";
        continuation.first_sequence = 3;
        continuation.last_sequence = 3;
        continuation.records.resize(1);
        continuation.records.front() =
            make_record("continuation", 3);
        const auto continued =
            recovered.appendAudit(continuation, writer);
        const auto lines = readNonEmptyLines(audit_path);
        expect(continued.status.ok && continued.value &&
                   continued.value->accepted_count == 1 &&
                   lines.size() == 2 &&
                   nlohmann::json::parse(lines.back())
                           .at("records")
                           .front()
                           .at("previous_hash") ==
                       accepted_chain_head &&
                   recovered.getHealth(observer).audit_records == 3,
               "post-restart sequence 3 must continue the recovered "
               "watermark and hash chain");
    }
}

/// A downstream module is an untrusted error-detail source. Concurrent
/// failures from independent sessions must be redacted at AgentService and
/// must commit gap-free producer sequences to both observability journals.
void testAgentServiceSanitizesDownstreamErrorsAndConcurrentProducers() {
    ScopedTempDirectory temp(
        "master-agent-agent-service-concurrent-observability");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "agent-service-concurrent-observability");
    const auto data_log_directory = temp.path() / "data-log";
    auto log = std::make_shared<DataLogService>(
        data_log_directory, clock, ids);
    expect(log->initialize().ok,
           "concurrent AgentService DataLog must initialize");
    const auto exception_directory = temp.path() / "exceptions";
    auto exceptions = std::make_shared<ExceptionManager>(
        exception_directory, clock, ids, log);
    expect(exceptions->initialize().ok,
           "concurrent AgentService ExceptionManager must initialize");

    auto preprocess =
        std::make_shared<BarrierFailingPreprocess>(2);
    auto unused =
        std::make_shared<UnreachableAgentDependencies>();
    auto orchestrator =
        std::make_shared<UnreachableOrchestrator>();
    auto atomic = std::make_shared<UnreachableAtomic>();
    auto service =
        std::make_shared<agent_service::AgentService>(
            clock, ids, preprocess,
            std::static_pointer_cast<memory::IMemoryService>(
                unused),
            std::static_pointer_cast<intent::IIntentEngine>(
                unused),
            orchestrator, atomic, log, exceptions);

    const std::vector<std::string> raw_inputs{
        "TOP_SECRET_USER_QUERY_ALPHA_7f31",
        "TOP_SECRET_USER_QUERY_BRAVO_9a42"};
    const auto make_request =
        [&](std::size_t index) {
            interaction::StandardRequest request;
            request.request_id =
                "concurrent-request-" + std::to_string(index);
            request.trace_id =
                "concurrent-trace-" + std::to_string(index);
            request.text = raw_inputs.at(index);
            request.timestamp_utc_ms = clock->utcNowMs();
            request.deadline_mono_ns =
                clock->monotonicNowNs() + 1'000'000'000LL;
            request.user_id =
                "concurrent-user-" + std::to_string(index);
            request.session_id =
                "concurrent-session-" + std::to_string(index);
            request.turn_id = 1;
            request.priority = TaskPriority::P1;
            return request;
        };
    const std::vector<interaction::StandardRequest> requests{
        make_request(0), make_request(1)};
    const auto make_call =
        [](const interaction::StandardRequest& request) {
            return CallContext{
                CallerModuleId::InteractionIngress,
                request.request_id, request.trace_id,
                "concurrent-principal", request.priority,
                request.deadline_mono_ns};
        };

    auto first = std::async(
        std::launch::async,
        [service, &requests, &make_call] {
            return service->runTurn(
                requests.at(0), make_call(requests.at(0)));
        });
    auto second = std::async(
        std::launch::async,
        [service, &requests, &make_call] {
            return service->runTurn(
                requests.at(1), make_call(requests.at(1)));
        });
    expect(first.wait_for(std::chrono::seconds(3)) ==
                   std::future_status::ready &&
               second.wait_for(std::chrono::seconds(3)) ==
                   std::future_status::ready,
           "both session turns must terminate without a producer-lane "
           "deadlock");
    const std::vector<agent_service::TurnResult> results{
        first.get(), second.get()};
    expect(preprocess->completedWithoutTimeout(),
           "the mock barrier must prove both sessions overlapped in "
           "the downstream module");

    for (const auto& result : results) {
        expect(!result.success && !result.pending &&
                   result.error_code ==
                       "UNTRUSTED_MODULE_FAILURE" &&
                   result.error_message ==
                       "request could not be completed",
               "untrusted downstream code/message must be replaced by "
               "the AgentService canonical failure envelope");
        const auto client_surface =
            result.reply + "|" + result.error_code + "|" +
            result.error_message + "|" + result.turn_summary;
        for (const auto& raw_input : raw_inputs) {
            expect(client_surface.find(raw_input) ==
                       std::string::npos,
                   "TurnResult must not echo any raw user input");
        }
    }

    CallContext observer{
        CallerModuleId::AgentService, "observer-request",
        "observer-trace", "observer-principal",
        TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};
    std::vector<LogEvent> observed_events;
    for (const auto& request : requests) {
        TraceQuery query;
        query.trace_id = request.trace_id;
        query.max_records = 10;
        const auto page = log->queryTrace(query, observer);
        expect(page.status.ok && page.value &&
                   page.value->complete_for_requested_range &&
                   page.value->events.size() == 3,
               "each failed session must retain TURN_ACCEPTED, "
               "Exception and TURN_FAILED observations");
        std::set<std::string> event_types;
        for (const auto& event : page.value->events) {
            event_types.insert(event.event_type);
            observed_events.push_back(event);
        }
        expect(event_types ==
                   std::set<std::string>{
                       "TURN_ACCEPTED",
                       "EXCEPTION_OCCURRENCE_ACCEPTED",
                       "TURN_FAILED"},
               "each concurrent trace must be complete without "
               "cross-session loss");
    }
    std::set<std::string> event_ids;
    for (const auto& event : observed_events) {
        event_ids.insert(event.event_id);
        expect(event.payload_summary_json.find("TOP_SECRET") ==
                   std::string::npos,
               "queryable log payloads must not expose user text");
        if (event.event_type == "TURN_FAILED") {
            expect(event.error_ref &&
                       *event.error_ref ==
                           "UNTRUSTED_MODULE_FAILURE",
                   "TURN_FAILED must expose only the normalized error "
                   "reference");
        }
        if (event.event_type ==
            "EXCEPTION_OCCURRENCE_ACCEPTED") {
            const auto payload = nlohmann::json::parse(
                event.payload_summary_json);
            expect(payload.at("domain") == "preprocess" &&
                       payload.at("code") ==
                           "UNTRUSTED_MODULE_FAILURE",
                   "Exception projection must contain only normalized "
                   "domain/code metadata");
        }
    }
    expect(event_ids.size() == 6,
           "concurrent traces must retain six unique events");
    const auto health = log->getHealth(observer);
    expect(health.ready && health.buffered_events == 0 &&
               health.persisted_events == 6,
           "DataLog health must account for every concurrent "
           "AgentService and Exception event");

    const auto event_journal =
        data_log_directory / "events.jsonl";
    const auto event_lines = readNonEmptyLines(event_journal);
    expect(event_lines.size() == 6,
           "six accepted observations must create six physical "
           "Event batch frames");
    std::vector<std::uint64_t> agent_log_sequences;
    std::vector<std::uint64_t> exception_log_sequences;
    std::set<std::uint64_t> agent_log_epochs;
    std::set<std::uint64_t> exception_log_epochs;
    for (const auto& line : event_lines) {
        const auto frame = nlohmann::json::parse(line);
        expect(frame.at("_journal_kind") == "event_batch" &&
                   frame.at("records").is_array() &&
                   frame.at("records").size() == 1,
               "each producer append must remain one committed "
               "physical frame");
        const auto& record = frame.at("records").front();
        const auto endpoint =
            record.at("producer_endpoint_id").get<std::string>();
        const auto epoch =
            record.at("producer_epoch").get<std::uint64_t>();
        const auto sequence =
            record.at("producer_sequence").get<std::uint64_t>();
        if (endpoint == "agent-service/data-log") {
            agent_log_epochs.insert(epoch);
            agent_log_sequences.push_back(sequence);
        } else if (endpoint == "ExceptionManager") {
            exception_log_epochs.insert(epoch);
            exception_log_sequences.push_back(sequence);
        }
    }
    expect(agent_log_epochs.size() == 1 &&
               *agent_log_epochs.begin() != 0 &&
               agent_log_sequences ==
                   std::vector<std::uint64_t>{1, 2, 3, 4},
           "AgentService Event producer sequence must be physically "
           "gap-free and monotonic under concurrent turns");
    expect(exception_log_epochs.size() == 1 &&
               *exception_log_epochs.begin() != 0 &&
               exception_log_sequences ==
                   std::vector<std::uint64_t>{1, 2},
           "Exception projection producer sequence must be physically "
           "gap-free and monotonic under concurrent reports");

    const auto exception_journal =
        exception_directory / "journal" / "active" /
        "exception.jsonl";
    const auto exception_lines =
        readNonEmptyLines(exception_journal);
    expect(exception_lines.size() == 4,
           "two exception reports and two observation ACKs must be "
           "durably journaled; observed " +
               std::to_string(exception_lines.size()) +
               " physical frames");
    std::vector<std::uint64_t> occurrence_sequences;
    std::set<std::uint64_t> occurrence_epochs;
    std::uint64_t expected_journal_sequence = 1;
    std::size_t report_count = 0;
    for (const auto& line : exception_lines) {
        const auto frame = nlohmann::json::parse(line);
        const auto& payload = frame.at("payload");
        expect(payload.at("journal_sequence")
                       .get<std::uint64_t>() ==
                   expected_journal_sequence++,
               "Exception journal transaction sequence must never "
               "rollback or skip under concurrency");
        if (payload.at("kind") != "report") continue;
        ++report_count;
        expect(payload.at("occurrence_entries").size() == 1 &&
                   payload.at("groups_after").size() == 1,
               "each failed turn must durably retain one normalized "
               "exception occurrence");
        const auto& entry =
            payload.at("occurrence_entries").front();
        occurrence_sequences.push_back(
            entry.at("effective_sequence")
                .get<std::uint64_t>());
        occurrence_epochs.insert(
            entry.at("producer_epoch").get<std::uint64_t>());
        const auto& group = payload.at("groups_after").front();
        expect(group.at("domain") == "preprocess" &&
                   group.at("code") ==
                       "UNTRUSTED_MODULE_FAILURE",
               "authoritative Exception groups must store only "
               "normalized domain/code values");
    }
    expect(report_count == 2 &&
               occurrence_epochs.size() == 1 &&
               *occurrence_epochs.begin() != 0 &&
               occurrence_sequences ==
                   std::vector<std::uint64_t>{1, 2},
           "AgentService Exception producer sequence must be "
           "gap-free and retain both concurrent reports");

    const auto durable_bytes =
        readAllBytes(event_journal) + "\n" +
        readAllBytes(exception_journal);
    for (const auto& raw_input : raw_inputs) {
        expect(durable_bytes.find(raw_input) ==
                   std::string::npos,
               "neither DataLog nor Exception journal may persist "
               "downstream-echoed user plaintext");
    }
}

/// Trace selectors and health snapshots cross an observability trust
/// boundary and therefore fail closed on malformed or spoofed callers.
void testTraceQueryAndHealthAuthorizationBounds() {
    ScopedTempDirectory temp(
        "master-agent-trace-auth-bounds");
    auto clock = std::make_shared<ManualRuntimeClock>();
    DataLogService log(
        temp.path(), clock,
        std::make_shared<IdGenerator>("trace-auth-bounds"));
    expect(log.initialize().ok,
           "trace authorization fixture must initialize");

    CallContext writer{
        CallerModuleId::AgentService, "request-visible",
        "trace-visible", "writer-principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "query-producer", 91};
    LogEvent event;
    event.event_id = "query-visible-event";
    event.event_type = "QUERY_AUTH_TEST";
    event.module = "AgentService";
    event.interface_name = "IDataLogService";
    event.operation = "queryTrace";
    event.context.request_id = writer.request_id;
    event.context.trace_id = writer.trace_id;
    event.context.producer_endpoint_id =
        writer.caller_endpoint_id;
    event.context.producer_epoch =
        writer.caller_process_epoch;
    event.context.producer_sequence = 1;
    event.context.task_priority = writer.priority;
    event.context.deadline_mono_ns =
        writer.deadline_mono_ns;
    event.occurred_at_utc_ms = clock->utcNowMs();
    event.occurred_at_mono_ns = clock->monotonicNowNs();
    event.requested_durability =
        DurabilityClass::D2Journaled;
    event.payload_summary_json = "{}";
    LogEventBatch batch;
    batch.batch_id = "query-visible-batch";
    batch.producer_endpoint_id =
        event.context.producer_endpoint_id;
    batch.producer_epoch = event.context.producer_epoch;
    batch.first_sequence = 1;
    batch.last_sequence = 1;
    batch.records = {event};
    const auto appended = log.appendEvents(batch, writer);
    expect(appended.status.ok && appended.value &&
               appended.value->disposition ==
                   AppendDisposition::Accepted,
           "trace authorization fixture must persist one visible event");

    CallContext observer{
        CallerModuleId::AgentService, "request-observer",
        "trace-observer", "observer-principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};
    TraceQuery valid_query;
    valid_query.trace_id = writer.trace_id;
    const auto visible = log.queryTrace(valid_query, observer);
    expect(visible.status.ok && visible.value &&
               visible.value->events.size() == 1,
           "canonical AgentService observer must retrieve the trace");

    TraceQuery empty_selector;
    const auto empty_result =
        log.queryTrace(empty_selector, observer);
    auto oversized_query = valid_query;
    oversized_query.max_records = 1001;
    const auto oversized_result =
        log.queryTrace(oversized_query, observer);
    expect(!empty_result.status.ok &&
               empty_result.status.error.code ==
                   "TRACE_QUERY_INVALID" &&
               !oversized_result.status.ok &&
               oversized_result.status.error.code ==
                   "TRACE_QUERY_INVALID",
           "trace query must require a selector and a bounded page size");

    auto spoofed = observer;
    spoofed.caller_endpoint_id =
        "inproc:TaskOrchestrationEngine";
    spoofed.authorization_ref.clear();
    const auto unauthorized =
        log.queryTrace(valid_query, spoofed);
    expect(!unauthorized.status.ok &&
               unauthorized.status.error.code ==
                   "TRACE_CALLER_NOT_ALLOWED" &&
               !unauthorized.value,
           "a forged endpoint must not receive trace contents");

    const auto hidden_health = log.getHealth(spoofed);
    expect(!hidden_health.ready &&
               hidden_health.buffered_events == 0 &&
               hidden_health.persisted_events == 0 &&
               hidden_health.audit_records == 0 &&
               hidden_health.emergency_ring_records == 0 &&
               hidden_health.hash_chain_head.empty() &&
               !hidden_health.d4_ready &&
               !hidden_health.audit_integrity_degraded &&
               hidden_health.active_key_generation.empty() &&
               hidden_health.audit_anchor_generation == 0,
           "unauthorized health probes must receive no operational "
           "metadata");

    const auto visible_health = log.getHealth(observer);
    expect(visible_health.ready &&
               visible_health.persisted_events == 1,
           "authorized health probes must retain the real snapshot");
}

void testLogBatchFrameAndD1FlushRecovery() {
    ScopedTempDirectory temp(
        "master-agent-log-batch-frame");
    auto clock = std::make_shared<ManualRuntimeClock>();
    CallContext call{
        CallerModuleId::AgentService, "request-batch-frame",
        "trace-batch-frame", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "observability-test-proxy", 1,
        "trusted-observability-proxy"};

    const auto make_event =
        [&](std::string id, std::string endpoint,
            std::uint64_t epoch, std::uint64_t sequence,
            DurabilityClass durability) {
            LogEvent event;
            event.event_id = std::move(id);
            event.event_type = "BATCH_FRAME_TEST";
            event.module = "AgentService";
            event.interface_name = "IDataLogService";
            event.operation = "appendEvents";
            event.context.request_id = call.request_id;
            event.context.trace_id = call.trace_id;
            event.context.producer_endpoint_id =
                std::move(endpoint);
            event.context.producer_epoch = epoch;
            event.context.producer_sequence = sequence;
            event.context.task_priority = TaskPriority::P1;
            event.context.deadline_mono_ns =
                call.deadline_mono_ns;
            event.outcome = "accepted";
            event.occurred_at_utc_ms = clock->utcNowMs();
            event.occurred_at_mono_ns =
                clock->monotonicNowNs();
            event.requested_durability = durability;
            return event;
        };

    LogEventBatch durable_batch;
    durable_batch.batch_id = "multi-record-d3";
    durable_batch.producer_endpoint_id =
        "multi-record-producer";
    durable_batch.producer_epoch = 41;
    durable_batch.first_sequence = 1;
    durable_batch.last_sequence = 2;
    durable_batch.checksum = "multi-record-checksum";
    durable_batch.records = {
        make_event("multi-event-1",
                   durable_batch.producer_endpoint_id, 41, 1,
                   DurabilityClass::D3Fsynced),
        make_event("multi-event-2",
                   durable_batch.producer_endpoint_id, 41, 2,
                   DurabilityClass::D3Fsynced)};

    const auto event_path =
        temp.path() / "events.jsonl";
    {
        DataLogService log(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "batch-frame-first"));
        expect(log.initialize().ok,
               "batch-frame DataLog must initialize");
        const auto appended =
            log.appendEvents(durable_batch, call);
        const auto lines = readNonEmptyLines(event_path);
        expect(appended.status.ok && appended.value &&
                   appended.value->accepted_count == 2 &&
                   lines.size() == 1,
               "a multi-record D3 batch must commit as one physical frame");
        const auto frame = nlohmann::json::parse(lines.front());
        expect(frame.at("_journal_kind") == "event_batch" &&
                   frame.at("_batch_frame_count") == 2 &&
                   frame.at("records").size() == 2,
               "the physical frame must bind the complete logical batch");
    }

    {
        DataLogService recovered(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "batch-frame-second"));
        expect(recovered.initialize().ok,
               "multi-record batch must recover atomically");
        TraceQuery query;
        query.trace_id = call.trace_id;
        const auto trace = recovered.queryTrace(query, call);
        const auto replay =
            recovered.appendEvents(durable_batch, call);
        expect(trace.value && trace.value->events.size() == 2 &&
                   replay.value &&
                   replay.value->disposition ==
                       AppendDisposition::Duplicate &&
                   readNonEmptyLines(event_path).size() == 1,
               "recovery must rebuild both records and the BatchID ledger");

        LogEventBatch buffered_batch;
        buffered_batch.batch_id = "buffered-d1";
        buffered_batch.producer_endpoint_id =
            "buffered-producer";
        buffered_batch.producer_epoch = 51;
        buffered_batch.first_sequence = 1;
        buffered_batch.last_sequence = 1;
        buffered_batch.checksum = "buffered-checksum";
        buffered_batch.records = {
            make_event("buffered-event-1",
                       buffered_batch.producer_endpoint_id,
                       51, 1,
                       DurabilityClass::D1Buffered)};
        const auto buffered =
            recovered.appendEvents(buffered_batch, call);
        const auto before_flush = recovered.getHealth(call);
        expect(buffered.value &&
                   buffered.value->achieved_durability ==
                       DurabilityClass::D1Buffered &&
                   !buffered.value->durability_ack_id &&
                   before_flush.buffered_events == 1 &&
                   before_flush.persisted_events == 2,
               "D1 must enter the process buffer without claiming a durable ack");
        expect(recovered.flush(call).ok,
               "flush must durably advance buffered D1 records");
        const auto after_flush = recovered.getHealth(call);
        expect(after_flush.buffered_events == 0 &&
                   after_flush.persisted_events == 3,
               "health must distinguish buffered from persisted records");
    }

    // A crash can leave an unterminated physical frame.  Recovery truncates
    // only that tail and keeps all previously committed batch frames.
    appendAllBytes(event_path, "{\"unterminated_batch\":");
    DataLogService third(
        temp.path(), clock,
        std::make_shared<IdGenerator>(
            "batch-frame-third"));
    expect(third.initialize().ok,
           "unterminated batch tail must be repaired on restart");
    TraceQuery query;
    query.trace_id = call.trace_id;
    const auto trace = third.queryTrace(query, call);
    expect(trace.value && trace.value->events.size() == 3,
           "tail repair must preserve the two-record D3 batch and flushed D1");

    LogEventBatch rollback;
    rollback.batch_id = "buffered-sequence-rollback";
    rollback.producer_endpoint_id = "buffered-producer";
    rollback.producer_epoch = 51;
    rollback.first_sequence = 0;
    rollback.last_sequence = 0;
    rollback.records = {
        make_event("buffered-event-rollback",
                   rollback.producer_endpoint_id, 51, 0,
                   DurabilityClass::D2Journaled)};
    const auto rejected = third.appendEvents(rollback, call);
    expect(!rejected.status.ok &&
               rejected.status.error.code ==
                   "LOG_PRODUCER_SEQUENCE_ROLLBACK",
           "recovered producer watermark must reject sequence rollback");
}

void testLogIngressValidationAndMixedDurabilityPromotion() {
    ScopedTempDirectory temp(
        "master-agent-log-ingress-validation");
    auto clock = std::make_shared<ManualRuntimeClock>();
    CallContext call{
        CallerModuleId::AgentService, "request-validation",
        "trace-validation", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL,
        "observability-test-proxy", 1,
        "trusted-observability-proxy"};

    auto make_event = [&](const std::string& id,
                          std::uint64_t sequence,
                          DurabilityClass durability) {
        LogEvent event;
        event.event_id = id;
        event.event_type = "VALIDATION_TEST";
        event.module = "AgentService";
        event.interface_name = "IDataLogService";
        event.operation = "appendEvents";
        event.context.request_id = call.request_id;
        event.context.trace_id = call.trace_id;
        event.context.producer_endpoint_id =
            "validation-producer";
        event.context.producer_epoch = 71;
        event.context.producer_sequence = sequence;
        event.context.deadline_mono_ns =
            call.deadline_mono_ns;
        event.requested_durability = durability;
        return event;
    };

    LogEventBatch mixed;
    mixed.batch_id = "mixed-d0-d2";
    mixed.producer_endpoint_id = "validation-producer";
    mixed.producer_epoch = 71;
    mixed.first_sequence = 1;
    mixed.last_sequence = 2;
    mixed.records = {
        make_event("mixed-d0", 1,
                   DurabilityClass::D0Volatile),
        make_event("mixed-d2", 2,
                   DurabilityClass::D2Journaled)};

    {
        DataLogService log(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "validation-first"));
        expect(log.initialize().ok,
               "validation DataLog must initialize");
        DataLogService contender(
            temp.path(), clock,
            std::make_shared<IdGenerator>(
                "validation-contender"));
        const auto lease_denied =
            contender.initialize();
        expect(!lease_denied.ok &&
                   lease_denied.error.code ==
                       "LOG_WRITER_LEASE_HELD",
               "a second instance must not share one journal directory");
        const auto promoted = log.appendEvents(mixed, call);
        expect(promoted.value &&
                   promoted.value->accepted_count == 2 &&
                   promoted.value->achieved_durability ==
                       DurabilityClass::D2Journaled &&
                   promoted.value->durability_ack_id,
               "mixed batch must promote every accepted record to D2");
        const auto physical = readNonEmptyLines(
            temp.path() / "events.jsonl");
        expect(physical.size() == 1 &&
                   nlohmann::json::parse(physical.front())
                           .at("records")
                           .size() == 2,
               "mixed D0/D2 records must share one physical D2 commit");

        auto volatile_duplicate =
            make_event("edge-volatile-duplicate", 1,
                       DurabilityClass::D0Volatile);
        volatile_duplicate.context.producer_endpoint_id =
            "validation-edge-producer";
        volatile_duplicate.context.producer_epoch = 76;
        LogEventBatch volatile_only;
        volatile_only.batch_id = "edge-volatile-original";
        volatile_only.producer_endpoint_id =
            "validation-edge-producer";
        volatile_only.producer_epoch = 76;
        volatile_only.first_sequence = 1;
        volatile_only.last_sequence = 1;
        volatile_only.records = {volatile_duplicate};
        expect(log.appendEvents(volatile_only, call).status.ok,
               "standalone D0 record must be accepted in memory");

        auto edge_d2 =
            make_event("edge-d2", 2,
                       DurabilityClass::D2Journaled);
        edge_d2.context.producer_endpoint_id =
            "validation-edge-producer";
        edge_d2.context.producer_epoch = 76;
        LogEventBatch promote_duplicate;
        promote_duplicate.batch_id =
            "edge-promote-duplicate";
        promote_duplicate.producer_endpoint_id =
            "validation-edge-producer";
        promote_duplicate.producer_epoch = 76;
        promote_duplicate.first_sequence = 1;
        promote_duplicate.last_sequence = 2;
        promote_duplicate.records = {
            volatile_duplicate, edge_d2};
        const auto promoted_duplicate =
            log.appendEvents(promote_duplicate, call);
        const auto promoted_lines = readNonEmptyLines(
            temp.path() / "events.jsonl");
        expect(promoted_duplicate.value &&
                   promoted_duplicate.value->achieved_durability ==
                       DurabilityClass::D2Journaled &&
                   promoted_lines.size() == 2 &&
                   nlohmann::json::parse(
                       promoted_lines.back())
                           .at("records")
                           .size() == 2,
               "unpersisted D0 duplicate must be promoted with a new D2 peer");

        auto invalid_schema = mixed;
        invalid_schema.batch_id = "invalid-schema";
        invalid_schema.producer_epoch = 72;
        invalid_schema.first_sequence = 1;
        invalid_schema.last_sequence = 1;
        invalid_schema.records = {
            make_event("invalid-schema-event", 1,
                       DurabilityClass::D3Fsynced)};
        invalid_schema.records.front().schema_version = 0;
        invalid_schema.records.front()
            .context.producer_epoch = 72;
        const auto schema_result =
            log.appendEvents(invalid_schema, call);
        expect(!schema_result.status.ok &&
                   schema_result.status.error.code ==
                       "LOG_EVENT_SCHEMA_INVALID",
               "unsupported Event schema must be rejected before I/O");

        auto invalid_enum = invalid_schema;
        invalid_enum.batch_id = "invalid-enum";
        invalid_enum.producer_epoch = 73;
        invalid_enum.records.front().event_id =
            "invalid-enum-event";
        invalid_enum.records.front().schema_version = 1;
        invalid_enum.records.front()
            .context.producer_epoch = 73;
        invalid_enum.records.front().requested_durability =
            static_cast<DurabilityClass>(255);
        const auto enum_result =
            log.appendEvents(invalid_enum, call);
        expect(!enum_result.status.ok &&
                   enum_result.status.error.code ==
                       "LOG_EVENT_SCHEMA_INVALID",
               "unknown durability enum must be rejected before I/O");

        AuditRecord invalid_audit;
        invalid_audit.audit_id = "invalid-audit-schema";
        invalid_audit.schema_version = 0;
        invalid_audit.audit_type = "Authorization";
        invalid_audit.context.producer_endpoint_id =
            "validation-audit";
        invalid_audit.context.producer_epoch = 74;
        invalid_audit.context.producer_sequence = 1;
        invalid_audit.requested_durability =
            DurabilityClass::D3Fsynced;
        AuditBatch invalid_audit_batch;
        invalid_audit_batch.batch_id =
            "invalid-audit-batch";
        invalid_audit_batch.producer_endpoint_id =
            "validation-audit";
        invalid_audit_batch.producer_epoch = 74;
        invalid_audit_batch.first_sequence = 1;
        invalid_audit_batch.last_sequence = 1;
        invalid_audit_batch.records = {invalid_audit};
        const auto audit_result =
            log.appendAudit(invalid_audit_batch, call);
        expect(!audit_result.status.ok &&
                   audit_result.status.error.code ==
                       "AUDIT_RECORD_INVALID",
               "unsupported Audit schema must be rejected before I/O");

        CallContext no_deadline{
            CallerModuleId::AgentService, "no-deadline",
            "trace-validation", "principal", TaskPriority::P1, 0,
            "observability-test-proxy", 1,
            "trusted-observability-proxy"};
        auto deadline_batch = invalid_schema;
        deadline_batch.batch_id = "no-deadline-batch";
        deadline_batch.producer_epoch = 75;
        deadline_batch.records.front().event_id =
            "no-deadline-event";
        deadline_batch.records.front().schema_version = 1;
        deadline_batch.records.front()
            .context.producer_epoch = 75;
        const auto deadline_append =
            log.appendEvents(deadline_batch, no_deadline);
        TraceQuery query;
        query.trace_id = call.trace_id;
        expect(!deadline_append.status.ok &&
                   deadline_append.status.error.code ==
                       "LOG_CALL_EXPIRED" &&
                   !log.queryTrace(query, no_deadline).status.ok &&
                   !log.flush(no_deadline).ok,
               "DataLog I/O entrypoints require a live absolute deadline");
    }

    DataLogService recovered(
        temp.path(), clock,
        std::make_shared<IdGenerator>(
            "validation-second"));
    expect(recovered.initialize().ok,
           "rejected poison records must not break restart recovery");
    TraceQuery query;
    query.trace_id = call.trace_id;
    const auto trace = recovered.queryTrace(query, call);
    expect(trace.value && trace.value->events.size() == 4,
           "all mixed and duplicate-promoted records must survive restart");

    ScopedTempDirectory reordered_temp(
        "master-agent-log-reordered-frames");
    const auto make_reordered_batch =
        [&](std::uint64_t sequence) {
            auto event = make_event(
                "reordered-event-" +
                    std::to_string(sequence),
                sequence, DurabilityClass::D2Journaled);
            event.context.producer_endpoint_id =
                "reordered-producer";
            event.context.producer_epoch = 91;
            LogEventBatch batch;
            batch.batch_id =
                "reordered-batch-" +
                std::to_string(sequence);
            batch.producer_endpoint_id =
                event.context.producer_endpoint_id;
            batch.producer_epoch = 91;
            batch.first_sequence = sequence;
            batch.last_sequence = sequence;
            batch.records = {event};
            return batch;
        };
    {
        DataLogService ordered(
            reordered_temp.path(), clock,
            std::make_shared<IdGenerator>(
                "reordered-first"));
        expect(ordered.initialize().ok &&
                   ordered.appendEvents(
                       make_reordered_batch(1), call)
                       .status.ok &&
                   ordered.appendEvents(
                       make_reordered_batch(2), call)
                       .status.ok,
               "ordered recovery fixture must persist two batches");
    }
    const auto reordered_path =
        reordered_temp.path() / "events.jsonl";
    const auto ordered_lines =
        readNonEmptyLines(reordered_path);
    expect(ordered_lines.size() == 2,
           "reorder fixture must contain two complete frames");
    writeAllBytes(
        reordered_path,
        ordered_lines[1] + "\n" +
            ordered_lines[0] + "\n");
    DataLogService reordered(
        reordered_temp.path(), clock,
        std::make_shared<IdGenerator>(
            "reordered-second"));
    const auto reordered_status =
        reordered.initialize();
    expect(!reordered_status.ok &&
               reordered_status.error.code ==
                   "LOG_EVENT_JOURNAL_INTEGRITY",
           "valid frames reordered within one producer epoch must fail recovery");

    ScopedTempDirectory backfill_temp(
        "master-agent-log-volatile-backfill");
    auto backfill_event =
        [&](std::uint64_t sequence,
            DurabilityClass durability) {
            auto event = make_event(
                "backfill-event-" +
                    std::to_string(sequence),
                sequence, durability);
            event.context.producer_endpoint_id =
                "backfill-producer";
            event.context.producer_epoch = 92;
            return event;
        };
    auto single_batch =
        [&](const LogEvent& event,
            const std::string& suffix) {
            LogEventBatch batch;
            batch.batch_id =
                "backfill-batch-" + suffix;
            batch.producer_endpoint_id =
                event.context.producer_endpoint_id;
            batch.producer_epoch =
                event.context.producer_epoch;
            batch.first_sequence =
                event.context.producer_sequence;
            batch.last_sequence =
                event.context.producer_sequence;
            batch.records = {event};
            return batch;
        };
    {
        DataLogService log(
            backfill_temp.path(), clock,
            std::make_shared<IdGenerator>(
                "backfill-first"));
        expect(log.initialize().ok,
               "backfill fixture must initialize");
        const auto volatile_event =
            backfill_event(
                1, DurabilityClass::D0Volatile);
        const auto durable_event =
            backfill_event(
                2, DurabilityClass::D2Journaled);
        expect(log.appendEvents(
                       single_batch(
                           volatile_event, "volatile"),
                       call)
                       .status.ok &&
                   log.appendEvents(
                       single_batch(
                           durable_event, "durable"),
                       call)
                       .status.ok,
               "volatile then durable fixture must be accepted");
        LogEventBatch late_promotion;
        late_promotion.batch_id =
            "backfill-batch-late-promotion";
        late_promotion.producer_endpoint_id =
            "backfill-producer";
        late_promotion.producer_epoch = 92;
        late_promotion.first_sequence = 1;
        late_promotion.last_sequence = 3;
        late_promotion.records = {
            volatile_event, durable_event,
            backfill_event(
                3, DurabilityClass::D2Journaled)};
        const auto rejected =
            log.appendEvents(late_promotion, call);
        expect(!rejected.status.ok &&
                   rejected.status.error.code ==
                       "LOG_EVENT_BACKFILL_ORDER_INVALID" &&
                   readNonEmptyLines(
                       backfill_temp.path() /
                       "events.jsonl")
                           .size() == 1,
               "late D0 backfill behind a durable sequence must fail closed");
    }
    DataLogService backfill_recovery(
        backfill_temp.path(), clock,
        std::make_shared<IdGenerator>(
            "backfill-second"));
    expect(backfill_recovery.initialize().ok,
           "rejected volatile backfill must not poison restart");
}

void testLogAmbiguousDurabilityFailureIsStable() {
    auto clock = std::make_shared<ManualRuntimeClock>();
    CallContext call{CallerModuleId::AgentService, "request-fault",
                     "trace-fault", "principal", TaskPriority::P1,
                     clock->monotonicNowNs() + 1'000'000'000LL,
                     "observability-test-proxy", 1,
                     "trusted-observability-proxy"};

    ScopedTempDirectory replay_event_temp(
        "master-agent-log-event-replay");
    auto replay_event_syncs =
        std::make_shared<std::atomic<std::size_t>>(0);
    auto replay_event_service =
        std::make_shared<std::weak_ptr<DataLogService>>();
    auto replay_event_log = std::make_shared<DataLogService>(
        replay_event_temp.path(), clock,
        std::make_shared<IdGenerator>("event-replay"),
        [replay_event_syncs,
         replay_event_service](const std::filesystem::path&) {
            const auto call_no = ++(*replay_event_syncs);
            if (const auto service = replay_event_service->lock()) {
                (void)service->getHealth(CallContext{});
            }
            return call_no == 1
                       ? Status::Ok()
                       : Status::Error(
                             "data_log", "UNEXPECTED_REPLAY_FSYNC",
                             "record replay must not fsync", false,
                             SideEffectState::Unknown);
        });
    *replay_event_service = replay_event_log;
    expect(replay_event_log->initialize().ok,
           "record replay event log must initialize");
    LogEvent replay_event;
    replay_event.event_id = "event-durable-replay";
    replay_event.event_type = "TERMINAL";
    replay_event.module = "TaskOrchestrationEngine";
    replay_event.interface_name = "event";
    replay_event.operation = "commit";
    replay_event.context.request_id = call.request_id;
    replay_event.context.trace_id = call.trace_id;
    replay_event.context.producer_endpoint_id = "orchestrator";
    replay_event.context.producer_epoch = 2;
    replay_event.context.producer_sequence = 1;
    replay_event.requested_durability =
        DurabilityClass::D3Fsynced;
    LogEventBatch replay_event_batch;
    replay_event_batch.batch_id = "event-durable-original";
    replay_event_batch.producer_endpoint_id = "orchestrator";
    replay_event_batch.producer_epoch = 2;
    replay_event_batch.first_sequence = 1;
    replay_event_batch.last_sequence = 1;
    replay_event_batch.records = {replay_event};
    expect(replay_event_log->appendEvents(replay_event_batch, call)
               .status.ok,
           "original D3 Event must be durable");
    replay_event_batch.batch_id = "event-durable-new-batch";
    const auto replay_event_result =
        replay_event_log->appendEvents(replay_event_batch, call);
    expect(replay_event_result.status.ok && replay_event_result.value &&
               replay_event_result.value->disposition ==
                   AppendDisposition::Duplicate &&
               replay_event_syncs->load() == 1 &&
               replay_event_log->getHealth(call).ready,
           "pure EventID replay must not perform another fsync");

    ScopedTempDirectory replay_audit_temp(
        "master-agent-log-audit-replay");
    auto replay_audit_syncs =
        std::make_shared<std::atomic<std::size_t>>(0);
    auto replay_tamper =
        std::make_shared<InMemoryTamperEvidenceProvider>();
    auto replay_audit_log = std::make_shared<DataLogService>(
        replay_audit_temp.path(), clock,
        std::make_shared<IdGenerator>("audit-replay"),
        [replay_audit_syncs](const std::filesystem::path&) {
            const auto call_no = ++(*replay_audit_syncs);
            return call_no == 1
                       ? Status::Ok()
                       : Status::Error(
                             "data_log", "UNEXPECTED_REPLAY_FSYNC",
                             "audit replay must not fsync", false,
                             SideEffectState::Unknown);
        },
        replay_tamper);
    expect(replay_audit_log->initialize().ok,
           "record replay audit log must initialize");
    AuditRecord replay_audit;
    replay_audit.audit_id = "audit-durable-replay";
    replay_audit.audit_type = "Authorization";
    replay_audit.context.request_id = call.request_id;
    replay_audit.context.trace_id = call.trace_id;
    replay_audit.context.producer_endpoint_id = "agent-service";
    replay_audit.context.producer_epoch = 2;
    replay_audit.context.producer_sequence = 1;
    replay_audit.actor_id_hash = "actor";
    replay_audit.actor_role = "system";
    replay_audit.subject_id_hash = "subject";
    replay_audit.action = "authorize";
    replay_audit.interface_name = "admission";
    replay_audit.decision = "allow";
    replay_audit.policy_id = "policy";
    replay_audit.policy_version = "1";
    replay_audit.requested_durability =
        DurabilityClass::D4TamperEvident;
    AuditBatch replay_audit_batch;
    replay_audit_batch.batch_id = "audit-durable-original";
    replay_audit_batch.producer_endpoint_id = "agent-service";
    replay_audit_batch.producer_epoch = 2;
    replay_audit_batch.first_sequence = 1;
    replay_audit_batch.last_sequence = 1;
    replay_audit_batch.records = {replay_audit};
    expect(replay_audit_log->appendAudit(replay_audit_batch, call)
               .status.ok,
           "original D4 Audit must be durable");
    replay_audit_batch.batch_id = "audit-durable-new-batch";
    const auto replay_audit_result =
        replay_audit_log->appendAudit(replay_audit_batch, call);
    expect(replay_audit_result.status.ok && replay_audit_result.value &&
               replay_audit_result.value->disposition ==
                   AppendDisposition::Duplicate &&
               replay_audit_syncs->load() == 1 &&
               replay_audit_log->getHealth(call).ready,
           "pure AuditID replay must not fsync or degrade the chain");

    ScopedTempDirectory event_temp("master-agent-log-event-fault");
    auto event_sync_calls = std::make_shared<std::atomic<std::size_t>>(0);
    auto event_log = std::make_shared<DataLogService>(
        event_temp.path(), clock,
        std::make_shared<IdGenerator>("event-fault"),
        [event_sync_calls](const std::filesystem::path&) {
            ++(*event_sync_calls);
            return Status::Error(
                "data_log", "LOG_TEST_FSYNC_FAILED",
                "injected durability failure", true,
                SideEffectState::Unknown);
        });
    expect(event_log->initialize().ok,
           "fault-injection event log must initialize");
    LogEvent event;
    event.event_id = "event-ambiguous";
    event.event_type = "TERMINAL";
    event.module = "TaskOrchestrationEngine";
    event.interface_name = "event";
    event.operation = "commit";
    event.context.request_id = call.request_id;
    event.context.trace_id = call.trace_id;
    event.context.producer_endpoint_id = "orchestrator";
    event.context.producer_epoch = 7;
    event.context.producer_sequence = 1;
    event.requested_durability = DurabilityClass::D3Fsynced;
    LogEventBatch event_batch;
    event_batch.batch_id = "event-batch-ambiguous";
    event_batch.producer_endpoint_id = "orchestrator";
    event_batch.producer_epoch = 7;
    event_batch.first_sequence = 1;
    event_batch.last_sequence = 1;
    event_batch.records = {event};
    const auto event_failed =
        event_log->appendEvents(event_batch, call);
    expect(!event_failed.status.ok &&
               event_failed.status.error.side_effect_state ==
                   SideEffectState::Unknown &&
               readNonEmptyLines(
                   event_temp.path() / "events.jsonl")
                       .size() == 1,
           "failed fsync must expose UNKNOWN after one physical write");
    const auto event_retry =
        event_log->appendEvents(event_batch, call);
    auto event_new_transport = event_batch;
    event_new_transport.batch_id = "event-batch-new-transport";
    const auto event_id_retry =
        event_log->appendEvents(event_new_transport, call);
    expect(!event_retry.status.ok && !event_id_retry.status.ok &&
               event_id_retry.status.error.code ==
                   "LOG_EVENT_COMMIT_UNKNOWN" &&
               event_sync_calls->load() == 1 &&
               readNonEmptyLines(
                   event_temp.path() / "events.jsonl")
                       .size() == 1,
           "ambiguous Event batch/EventID retries must not write twice");

    TraceQuery event_query;
    event_query.trace_id = call.trace_id;
    const auto unacknowledged =
        event_log->queryTrace(event_query, call);
    expect(unacknowledged.value &&
               unacknowledged.value->events.empty(),
           "unacknowledged ambiguous Event must not enter committed trace");

    ScopedTempDirectory throwing_sync_temp(
        "master-agent-log-throwing-sync");
    auto throwing_sync_calls =
        std::make_shared<std::atomic<std::size_t>>(0);
    auto throwing_sync_log = std::make_shared<DataLogService>(
        throwing_sync_temp.path(), clock,
        std::make_shared<IdGenerator>("throwing-sync"),
        [throwing_sync_calls](const std::filesystem::path&) -> Status {
            ++(*throwing_sync_calls);
            throw std::runtime_error("injected sync exception");
        });
    expect(throwing_sync_log->initialize().ok,
           "throwing-sync log must initialize");
    auto throwing_event = event;
    throwing_event.event_id = "event-throwing-sync";
    throwing_event.context.producer_endpoint_id =
        "throwing-sync-producer";
    throwing_event.context.producer_epoch = 1;
    throwing_event.context.producer_sequence = 1;
    LogEventBatch throwing_batch;
    throwing_batch.batch_id = "event-batch-throwing-sync";
    throwing_batch.producer_endpoint_id =
        throwing_event.context.producer_endpoint_id;
    throwing_batch.producer_epoch =
        throwing_event.context.producer_epoch;
    throwing_batch.first_sequence = 1;
    throwing_batch.last_sequence = 1;
    throwing_batch.records = {throwing_event};
    const auto throwing_failed =
        throwing_sync_log->appendEvents(throwing_batch, call);
    const auto throwing_retry =
        throwing_sync_log->appendEvents(throwing_batch, call);
    expect(!throwing_failed.status.ok &&
               throwing_failed.status.error.code ==
                   "LOG_DURABILITY_SYNC_EXCEPTION" &&
               throwing_failed.status.error.side_effect_state ==
                   SideEffectState::Unknown &&
               !throwing_retry.status.ok &&
               throwing_sync_calls->load() == 1 &&
               readNonEmptyLines(
                   throwing_sync_temp.path() / "events.jsonl")
                       .size() == 1,
           "throwing durability adapter must become stable UNKNOWN without reappend");

    ScopedTempDirectory audit_temp("master-agent-log-audit-fault");
    auto audit_sync_calls = std::make_shared<std::atomic<std::size_t>>(0);
    auto audit_tamper =
        std::make_shared<InMemoryTamperEvidenceProvider>();
    auto audit_log = std::make_shared<DataLogService>(
        audit_temp.path(), clock,
        std::make_shared<IdGenerator>("audit-fault"),
        [audit_sync_calls](const std::filesystem::path&) {
            ++(*audit_sync_calls);
            return Status::Error(
                "data_log", "AUDIT_TEST_FSYNC_FAILED",
                "injected audit durability failure", true,
                SideEffectState::Unknown);
        },
        audit_tamper);
    expect(audit_log->initialize().ok,
           "fault-injection audit log must initialize");
    AuditRecord audit;
    audit.audit_id = "audit-ambiguous";
    audit.audit_type = "Authorization";
    audit.context.request_id = call.request_id;
    audit.context.trace_id = call.trace_id;
    audit.context.producer_endpoint_id = "agent-service";
    audit.context.producer_epoch = 3;
    audit.context.producer_sequence = 1;
    audit.actor_id_hash = "actor";
    audit.actor_role = "system";
    audit.subject_id_hash = "subject";
    audit.action = "authorize";
    audit.interface_name = "admission";
    audit.decision = "allow";
    audit.policy_id = "policy";
    audit.policy_version = "1";
    audit.requested_durability =
        DurabilityClass::D4TamperEvident;
    AuditBatch audit_batch;
    audit_batch.batch_id = "audit-batch-ambiguous";
    audit_batch.producer_endpoint_id = "agent-service";
    audit_batch.producer_epoch = 3;
    audit_batch.first_sequence = 1;
    audit_batch.last_sequence = 1;
    audit_batch.records = {audit};
    const auto audit_failed =
        audit_log->appendAudit(audit_batch, call);
    const auto audit_retry =
        audit_log->appendAudit(audit_batch, call);
    auto later_audit = audit_batch;
    later_audit.batch_id = "audit-batch-after-degraded";
    later_audit.first_sequence = 2;
    later_audit.last_sequence = 2;
    later_audit.records.front().audit_id = "audit-after-degraded";
    later_audit.records.front().context.producer_sequence = 2;
    const auto fenced =
        audit_log->appendAudit(later_audit, call);
    const auto audit_health = audit_log->getHealth(call);
    expect(!audit_failed.status.ok && !audit_retry.status.ok &&
               !fenced.status.ok &&
               fenced.status.error.code ==
                   "LOG_AUDIT_INTEGRITY_DEGRADED" &&
               audit_sync_calls->load() == 1 &&
               readNonEmptyLines(
                   audit_temp.path() / "audit.jsonl")
                       .size() == 1 &&
               audit_health.hash_chain_head == "GENESIS" &&
               !audit_health.ready,
           "ambiguous Audit commit must fence the chain and never reappend");
}

/// Exception ingress accepts only a producer-redacted, privacy-labelled and
/// checksum-sealed batch. Every malformed envelope must fail before either
/// durable journal, the Exception producer watermark, or the DataLog
/// projection changes.
///
void testExceptionPrivacyContractIsAtomicAndRecoverable() {
    ScopedTempDirectory temp(
        "master-agent-exception-privacy-contract");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("exception-privacy");
    const auto data_log_storage =
        temp.path() / "data-log";
    auto durable =
        std::make_shared<DataLogService>(
            data_log_storage, clock, ids);
    expect(durable->initialize().ok,
           "Exception privacy DataLog must initialize");
    auto controlled =
        std::make_shared<ControllableDataLog>(durable);
    const auto exception_storage =
        temp.path() / "exceptions";
    const auto exception_journal =
        exception_storage / "journal" / "active" /
        "exception.jsonl";
    const auto event_journal =
        data_log_storage / "events.jsonl";
    CallContext call{
        CallerModuleId::AgentService,
        "request-exception-privacy",
        "trace-exception-privacy",
        "principal-exception-privacy",
        TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};

    auto occurrence = makeExceptionOccurrence(
        "occurrence-exception-privacy", clock);
    occurrence.producer_sequence = 1;
    occurrence.context.request_id = call.request_id;
    occurrence.context.trace_id = call.trace_id;
    occurrence.context.task_priority = call.priority;
    occurrence.context.deadline_mono_ns =
        call.deadline_mono_ns;
    occurrence.bounded_detail_code =
        "PROVIDER_RESPONSE_LOST";
    occurrence.bounded_detail_summary =
        "provider-response-lost:v2";
    occurrence.privacy_labels = {
        "EXCEPTION_METADATA"};
    ExceptionReportRequest valid_report;
    valid_report.report_id =
        "report-exception-privacy";
    valid_report.occurrences = {occurrence};
    sealExceptionReport(valid_report, call);
    expect(!valid_report.batch_checksum.empty(),
           "exported Exception checksum must seal a complete report");

    using InvalidReportMutation =
        std::function<void(ExceptionReportRequest&)>;
    struct InvalidReportCase {
        std::string name;
        InvalidReportMutation mutate;
        bool refresh_checksum = true;
        std::string error_code;
        std::string forbidden_plaintext;
    };
    const std::string token_summary =
        "access_token=vehicle-owner-secret";
    const std::string key_summary =
        "api key: vehicle-owner-secret";
    const std::string bearer_summary =
        "Bearer eyJhbGciOiJIUzI1NiJ9.payload.signature";
    const std::string json_summary =
        R"({"user_query":"raw vehicle request"})";
    const std::string chinese_summary =
        u8"用户完整原文：请把我的家庭地址发送给服务端";
    const std::vector<InvalidReportCase> invalid_cases{
        {"missing-redaction-proof",
         [](auto& report) {
             report.source_redaction_proof.clear();
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID", {}},
        {"empty-checksum",
         [](auto& report) {
             report.batch_checksum.clear();
         },
         false, "EXM_BATCH_CHECKSUM_INVALID", {}},
        {"forged-checksum",
         [](auto& report) {
             report.batch_checksum =
                 "forged-checksum";
         },
         false, "EXM_BATCH_CHECKSUM_INVALID", {}},
        {"empty-privacy-labels",
         [](auto& report) {
             report.occurrences.front()
                 .privacy_labels.clear();
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID", {}},
        {"unsupported-privacy-label",
         [](auto& report) {
             report.occurrences.front()
                 .privacy_labels = {
                     "RAW_USER_TEXT"};
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         "RAW_USER_TEXT"},
        {"duplicate-privacy-label",
         [](auto& report) {
             report.occurrences.front()
                 .privacy_labels = {
                     "EXCEPTION_METADATA",
                     "EXCEPTION_METADATA"};
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID", {}},
        {"token-summary",
         [token_summary](auto& report) {
             report.occurrences.front()
                 .bounded_detail_summary =
                     token_summary;
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         token_summary},
        {"key-summary",
         [key_summary](auto& report) {
             report.occurrences.front()
                 .bounded_detail_summary =
                     key_summary;
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         key_summary},
        {"bearer-summary",
         [bearer_summary](auto& report) {
             report.occurrences.front()
                 .bounded_detail_summary =
                     bearer_summary;
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         bearer_summary},
        {"json-summary",
         [json_summary](auto& report) {
             report.occurrences.front()
                 .bounded_detail_summary =
                     json_summary;
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         json_summary},
        {"chinese-summary",
         [chinese_summary](auto& report) {
             report.occurrences.front()
                 .bounded_detail_summary =
                     chinese_summary;
         },
         true, "EXM_PRIVACY_CONTRACT_INVALID",
         chinese_summary}};

    std::string accepted_exception_id;
    std::string accepted_durability_ack;
    std::size_t committed_exception_frames = 0;
    std::size_t committed_event_frames = 0;
    std::size_t append_count_after_commit = 0;
    {
        ExceptionManager manager(
            exception_storage, clock, ids, controlled);
        expect(manager.initialize().ok,
               "Exception privacy journal must initialize");
        for (const auto& invalid_case : invalid_cases) {
            auto invalid = valid_report;
            invalid_case.mutate(invalid);
            if (invalid_case.refresh_checksum) {
                invalid.batch_checksum =
                    exceptionBatchChecksum(invalid);
            }
            const auto exception_bytes_before =
                readAllBytes(exception_journal);
            const auto event_bytes_before =
                readAllBytes(event_journal);
            const auto health_before =
                durable->getHealth(call);
            const auto append_before =
                controlled->appendCount();
            const auto rejected =
                manager.report(invalid, call);
            const auto health_after =
                durable->getHealth(call);
            expect(!rejected.status.ok &&
                       rejected.status.error.code ==
                           invalid_case.error_code,
                   "privacy-invalid Exception report must fail closed: " +
                       invalid_case.name);
            expect(
                readAllBytes(exception_journal) ==
                        exception_bytes_before &&
                    readAllBytes(event_journal) ==
                        event_bytes_before &&
                    health_after.persisted_events ==
                        health_before.persisted_events &&
                    controlled->appendCount() ==
                        append_before,
                "privacy rejection must precede Exception/DataLog "
                "journal and projection changes: " +
                    invalid_case.name);
        }

        const auto accepted =
            manager.report(valid_report, call);
        expect(accepted.status.ok && accepted.value &&
                   accepted.value->accepted_count == 1 &&
                   accepted.value->rejected_count == 0 &&
                   accepted.value->results.size() == 1,
               "complete privacy-labelled Exception report must succeed");
        accepted_exception_id =
            accepted.value->results.front().exception_id;
        accepted_durability_ack =
            accepted.value->results.front().durability_ack_id;
        committed_exception_frames =
            readNonEmptyLines(exception_journal).size();
        committed_event_frames =
            readNonEmptyLines(event_journal).size();
        append_count_after_commit =
            controlled->appendCount();
        expect(committed_exception_frames >= 1 &&
                   committed_event_frames == 1 &&
                   durable->getHealth(call)
                           .persisted_events == 1,
               "valid producer sequence 1 must commit after every "
               "rejection, proving the Exception watermark did not move");

        const auto exact_replay =
            manager.report(valid_report, call);
        expect(exact_replay.status.ok &&
                   exact_replay.value &&
                   exact_replay.value->results.front()
                           .exception_id ==
                       accepted_exception_id &&
                   exact_replay.value->results.front()
                           .durability_ack_id ==
                       accepted_durability_ack &&
                   readNonEmptyLines(exception_journal)
                           .size() ==
                       committed_exception_frames &&
                   readNonEmptyLines(event_journal).size() ==
                       committed_event_frames &&
                   controlled->appendCount() ==
                       append_count_after_commit,
               "same ReportID privacy envelope must replay without "
               "journal append or projection");
    }

    const auto exception_bytes =
        readAllBytes(exception_journal);
    const auto event_bytes =
        readAllBytes(event_journal);
    for (const auto& invalid_case : invalid_cases) {
        if (!invalid_case.forbidden_plaintext.empty()) {
            expect(
                exception_bytes.find(
                    invalid_case.forbidden_plaintext) ==
                        std::string::npos &&
                    event_bytes.find(
                        invalid_case.forbidden_plaintext) ==
                        std::string::npos,
                "rejected privacy plaintext must never reach either "
                "journal: " +
                    invalid_case.name);
        }
    }

    {
        ExceptionManager recovered(
            exception_storage, clock,
            std::make_shared<IdGenerator>(
                "exception-privacy-recovered"),
            controlled);
        expect(recovered.initialize().ok,
               "privacy-sealed Exception journal must recover");
        const auto group =
            recovered.getException(
                accepted_exception_id, call);
        const auto append_before_replay =
            controlled->appendCount();
        const auto replay =
            recovered.report(valid_report, call);
        expect(group.status.ok && group.value &&
                   group.value->occurrence_count == 1 &&
                   replay.status.ok && replay.value &&
                   replay.value->results.front()
                           .exception_id ==
                       accepted_exception_id &&
                   replay.value->results.front()
                           .durability_ack_id ==
                       accepted_durability_ack &&
                   readNonEmptyLines(exception_journal)
                           .size() ==
                       committed_exception_frames &&
                   readNonEmptyLines(event_journal).size() ==
                       committed_event_frames &&
                   controlled->appendCount() ==
                       append_before_replay,
               "restart must recover the report ledger and keep exact "
               "privacy-envelope replay idempotent");
    }
}

void testExceptionGroupingAndOccurrenceReplay() {
    ScopedTempDirectory temp("master-agent-exception");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("exception-test");
    auto log =
        std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
    expect(log->initialize().ok, "log service must initialize");
    ExceptionManager manager(
        temp.path() / "exceptions", clock, ids, log);
    expect(manager.initialize().ok,
           "exception journal must initialize before Ready");
    CallContext call{CallerModuleId::AgentService, "request", "trace",
                     "principal", TaskPriority::P1,
                     clock->monotonicNowNs() + 1000000000LL};
    ExceptionOccurrence occurrence;
    occurrence.occurrence_id = "occurrence-1";
    occurrence.domain = "inference";
    occurrence.code = "RUNTIME_LOST";
    occurrence.source_module = "InferenceFramework";
    occurrence.source_interface = "submitInference";
    occurrence.operation = "decode";
    occurrence.bounded_detail_summary = "runtime-lost";
    occurrence.occurred_at_utc_ms = clock->utcNowMs();
    occurrence.side_effect_state = SideEffectState::Unknown;
    ExceptionReportRequest report;
    report.report_id = "report-1";
    report.occurrences = {occurrence};
    sealExceptionReport(report, call);
    auto first = manager.report(report, call);
    expect(first.status.ok,
           "UNKNOWN exception report must be accepted: " +
               first.status.error.code + "/" +
               first.status.error.message);
    expect(first.value && first.value->accepted_count == 1 &&
               first.value->results[0].total_count == 1 &&
               first.value->results[0].escalation ==
                   EscalationKind::SafetyCandidate,
           "UNKNOWN exception must form a safety candidate");
    report.report_id = "report-replay";
    sealExceptionReport(report, call);
    auto replay = manager.report(report, call);
    const auto replay_group = manager.getException(
        first.value->results[0].exception_id, call);
    const auto first_version =
        first.value->results[0].group_version;
    const auto first_durability_ack =
        first.value->results[0].durability_ack_id;
    const auto exact_report_replay = manager.report(report, call);
    const auto group_after_exact_replay = manager.getException(
        first.value->results[0].exception_id, call);
    expect(replay.value &&
               replay.value->results[0].disposition ==
                   ExceptionDisposition::DuplicateOccurrence &&
               replay.value->results[0].total_count == 1 &&
               replay.value->results[0].group_version ==
                   first_version &&
               replay.value->results[0].durability_ack_id ==
                   first_durability_ack &&
               replay_group.value &&
               replay_group.value->version == first_version &&
               replay_group.value->occurrence_count == 1 &&
               replay_group.value->duplicate_replay_count == 1 &&
               exact_report_replay.value &&
               exact_report_replay.value->results[0].group_version ==
                   first_version &&
               exact_report_replay.value->results[0]
                       .durability_ack_id ==
                   first_durability_ack &&
               group_after_exact_replay.value &&
               group_after_exact_replay.value->version ==
                   first_version &&
               group_after_exact_replay.value->duplicate_replay_count ==
                   1,
           "a different ReportID replay must retain the first acceptance "
           "version/durability ACK, increment only replay telemetry, and "
           "remain exactly idempotent by ReportID");
    auto context_conflict = occurrence;
    context_conflict.context.trace_id =
        "different-causal-trace";
    ExceptionReportRequest conflicting_report;
    conflicting_report.report_id =
        "report-occurrence-context-conflict";
    conflicting_report.occurrences = {context_conflict};
    sealExceptionReport(conflicting_report, call);
    const auto conflict =
        manager.report(conflicting_report, call);
    expect(!conflict.status.ok &&
               conflict.status.error.code ==
                   "EXM_OCCURRENCE_CONFLICT",
           "OccurrenceID replay with different causal context must "
           "fail instead of reusing the fingerprint");

    auto invalid_schema_occurrence = occurrence;
    invalid_schema_occurrence.occurrence_id =
        "occurrence-invalid-schema";
    invalid_schema_occurrence.schema_version = 0;
    ExceptionReportRequest invalid_schema_report;
    invalid_schema_report.report_id =
        "report-invalid-schema";
    invalid_schema_report.occurrences = {
        invalid_schema_occurrence};
    sealExceptionReport(invalid_schema_report, call);
    const auto invalid_schema =
        manager.report(invalid_schema_report, call);
    auto invalid_enum_occurrence = occurrence;
    invalid_enum_occurrence.occurrence_id =
        "occurrence-invalid-enum";
    invalid_enum_occurrence.reported_severity =
        static_cast<ExceptionSeverity>(255);
    ExceptionReportRequest invalid_enum_report;
    invalid_enum_report.report_id =
        "report-invalid-enum";
    invalid_enum_report.occurrences = {
        invalid_enum_occurrence};
    sealExceptionReport(invalid_enum_report, call);
    const auto invalid_enum =
        manager.report(invalid_enum_report, call);
    auto unauthenticated_call = call;
    unauthenticated_call.caller_endpoint_id.clear();
    unauthenticated_call.caller_process_epoch = 0;
    auto unauthenticated_report = invalid_enum_report;
    unauthenticated_report.report_id =
        "report-unauthenticated";
    unauthenticated_report.occurrences.front()
        .reported_severity = ExceptionSeverity::Error;
    sealExceptionReport(unauthenticated_report, call);
    const auto unauthenticated =
        manager.report(
            unauthenticated_report,
            unauthenticated_call);
    auto invalid_priority_call = call;
    invalid_priority_call.priority =
        static_cast<TaskPriority>(255);
    auto invalid_priority_report =
        unauthenticated_report;
    invalid_priority_report.report_id =
        "report-invalid-call-priority";
    sealExceptionReport(invalid_priority_report, call);
    const auto invalid_priority =
        manager.report(
            invalid_priority_report,
            invalid_priority_call);
    expect(!invalid_schema.status.ok &&
               invalid_schema.status.error.code ==
                   "EXM_OCCURRENCE_SCHEMA_INVALID" &&
               !invalid_enum.status.ok &&
               invalid_enum.status.error.code ==
                   "EXM_OCCURRENCE_SCHEMA_INVALID" &&
               !unauthenticated.status.ok &&
               unauthenticated.status.error.code ==
                   "EXM_UNAUTHORIZED_SOURCE" &&
               !invalid_priority.status.ok &&
               invalid_priority.status.error.code ==
                   "EXM_UNAUTHORIZED_SOURCE",
           "Exception ingress must reject unknown schema/enums and "
           "unsealed producer identity before journal I/O");

    ExceptionMutationRequest mutation;
    mutation.mutation_id = "mutation-ack";
    mutation.exception_id = first.value->results[0].exception_id;
    mutation.expected_group_version =
        first.value->results[0].group_version;
    mutation.actor_id_hash = "operator-hash";
    mutation.actor_role = "ops";
    mutation.reason_code = "ACKNOWLEDGED_BY_OPERATOR";

    using InvalidMutation =
        std::function<void(ExceptionMutationRequest&)>;
    const std::string chinese_text =
        u8"用户完整输入";
    const std::string whitespace_text =
        "free form user text";
    const std::string json_text =
        R"({"user_query":"raw"})";
    const std::vector<
        std::pair<std::string, InvalidMutation>>
        invalid_mutations{
            {"actor-chinese",
             [&](ExceptionMutationRequest& request) {
                 request.actor_id_hash = chinese_text;
             }},
            {"actor-whitespace",
             [&](ExceptionMutationRequest& request) {
                 request.actor_id_hash = whitespace_text;
             }},
            {"actor-json",
             [&](ExceptionMutationRequest& request) {
                 request.actor_id_hash = json_text;
             }},
            {"reason-chinese",
             [&](ExceptionMutationRequest& request) {
                 request.reason_code = chinese_text;
             }},
            {"reason-whitespace",
             [&](ExceptionMutationRequest& request) {
                 request.reason_code = whitespace_text;
             }},
            {"reason-json",
             [&](ExceptionMutationRequest& request) {
                 request.reason_code = json_text;
             }},
            {"evidence-chinese",
             [&](ExceptionMutationRequest& request) {
                 request.verification_evidence_refs = {
                     chinese_text};
             }},
            {"evidence-whitespace",
             [&](ExceptionMutationRequest& request) {
                 request.verification_evidence_refs = {
                     whitespace_text};
             }},
            {"evidence-json",
             [&](ExceptionMutationRequest& request) {
                 request.verification_evidence_refs = {
                     json_text};
             }},
            {"waiver-chinese",
             [&](ExceptionMutationRequest& request) {
                 request.resolution_waiver_id =
                     chinese_text;
             }},
            {"waiver-whitespace",
             [&](ExceptionMutationRequest& request) {
                 request.resolution_waiver_id =
                     whitespace_text;
             }},
            {"waiver-json",
             [&](ExceptionMutationRequest& request) {
                 request.resolution_waiver_id = json_text;
             }}};
    const auto exception_journal =
        temp.path() / "exceptions" / "journal" / "active" /
        "exception.jsonl";
    const auto journal_bytes_before_invalid_mutations =
        readAllBytes(exception_journal).size();
    for (const auto& [suffix, inject] : invalid_mutations) {
        auto invalid_mutation = mutation;
        invalid_mutation.mutation_id =
            "mutation-invalid-" + suffix;
        inject(invalid_mutation);
        const auto bytes_before =
            readAllBytes(exception_journal).size();
        const auto rejected =
            manager.acknowledge(invalid_mutation, call);
        const auto bytes_after =
            readAllBytes(exception_journal).size();
        expect(!rejected.status.ok &&
                   rejected.status.error.code ==
                       "EXM_MUTATION_INVALID" &&
                   bytes_after == bytes_before,
               "unsafe actor/reason/evidence/waiver metadata must "
               "be rejected before Exception journal I/O: " +
                   suffix);
    }
    const auto group_after_invalid_mutations =
        manager.getException(
            first.value->results[0].exception_id, call);
    expect(readAllBytes(exception_journal).size() ==
                   journal_bytes_before_invalid_mutations &&
               group_after_invalid_mutations.value &&
               group_after_invalid_mutations.value->version ==
                   first_version &&
               group_after_invalid_mutations.value->lifecycle ==
                   ExceptionLifecycle::Open,
           "all unsafe mutation forms must leave journal bytes and "
           "authoritative group state unchanged");

    const auto acknowledged = manager.acknowledge(mutation, call);
    expect(acknowledged.status.ok && acknowledged.value &&
               acknowledged.value->group.lifecycle ==
                   ExceptionLifecycle::Acknowledged &&
               acknowledged.value->group.version ==
                   first_version + 1,
           "the first acceptance version must remain a valid mutation "
           "CAS token after an occurrence transport replay");
    const auto mutation_replay = manager.acknowledge(mutation, call);
    expect(mutation_replay.status.ok && mutation_replay.value &&
               mutation_replay.value->group.version ==
                   acknowledged.value->group.version,
           "same lifecycle mutation must replay idempotently");

    mutation.mutation_id = "mutation-mitigating";
    mutation.expected_group_version =
        acknowledged.value->group.version;
    mutation.reason_code = "MITIGATION_STARTED";
    const auto mitigating = manager.markMitigating(mutation, call);
    expect(mitigating.status.ok && mitigating.value &&
               mitigating.value->group.lifecycle ==
                   ExceptionLifecycle::Mitigating,
           "exception must enter MITIGATING with expected version");

    mutation.mutation_id = "mutation-resolved";
    mutation.expected_group_version =
        mitigating.value->group.version;
    mutation.reason_code = "MITIGATION_VERIFIED";
    mutation.verification_evidence_refs = {
        "verification://mitigation/runtime-probe-1"};

    auto empty_actor_resolution = mutation;
    empty_actor_resolution.mutation_id =
        "mutation-resolved-empty-actor";
    empty_actor_resolution.actor_id_hash.clear();
    const auto empty_actor =
        manager.resolve(empty_actor_resolution, call);

    auto empty_reason_resolution = mutation;
    empty_reason_resolution.mutation_id =
        "mutation-resolved-empty-reason";
    empty_reason_resolution.reason_code.clear();
    const auto empty_reason =
        manager.resolve(empty_reason_resolution, call);

    auto empty_evidence_resolution = mutation;
    empty_evidence_resolution.mutation_id =
        "mutation-resolved-empty-evidence";
    empty_evidence_resolution.verification_evidence_refs.clear();
    empty_evidence_resolution.resolution_waiver_id.clear();
    const auto empty_evidence =
        manager.resolve(empty_evidence_resolution, call);
    expect(!empty_actor.status.ok &&
               empty_actor.status.error.code ==
                   "EXM_MUTATION_INVALID" &&
               !empty_reason.status.ok &&
               empty_reason.status.error.code ==
                   "EXM_MUTATION_INVALID" &&
               !empty_evidence.status.ok &&
               empty_evidence.status.error.code ==
                   "EXM_RESOLUTION_EVIDENCE_REQUIRED",
           "RESOLVED must fail closed without actor, reason, or "
           "verification evidence/policy waiver");

    const auto resolved = manager.resolve(mutation, call);
    expect(resolved.status.ok && resolved.value &&
               resolved.value->group.lifecycle ==
                   ExceptionLifecycle::Resolved,
           "exception must enter RESOLVED only with durable verification "
           "evidence");

    mutation.mutation_id = "mutation-regression";
    mutation.expected_group_version = resolved.value->group.version;
    mutation.reason_code = "INVALID_REGRESSION";
    mutation.verification_evidence_refs.clear();
    const auto regression = manager.acknowledge(mutation, call);
    expect(!regression.status.ok &&
               regression.status.error.code ==
                   "EXCEPTION_LIFECYCLE_REGRESSION",
           "resolved exception lifecycle must not move backwards");
}

/// Reopen is an explicit durable lifecycle state. Duplicate occurrence
/// accounting is durable replay telemetry, but it preserves the first
/// acceptance token and does not invalidate lifecycle mutation CAS.
void testExceptionReopenedLifecycleAndDuplicateVersionRecovery() {
    ScopedTempDirectory temp(
        "master-agent-exception-reopened");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("exception-reopened");
    auto log = std::make_shared<DataLogService>(
        temp.path() / "data-log", clock, ids);
    expect(log->initialize().ok,
           "Reopened recovery DataLog must initialize");
    const auto storage = temp.path() / "exceptions";
    CallContext call{
        CallerModuleId::AgentService, "request-reopened",
        "trace-reopened", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};

    const auto original =
        makeExceptionOccurrence("occurrence-reopened-original", clock);
    ExceptionReportRequest original_report;
    original_report.report_id = "report-reopened-original";
    original_report.occurrences = {original};
    sealExceptionReport(original_report, call);

    std::string exception_id;
    std::uint64_t original_acceptance_version = 0;
    std::string original_durability_ack;
    std::uint64_t reopened_version = 0;
    std::uint64_t final_version = 0;
    ExceptionReportRequest duplicate_after_recovery;

    {
        ExceptionManager manager(storage, clock, ids, log);
        expect(manager.initialize().ok,
               "Reopened fixture must initialize");
        const auto created = manager.report(original_report, call);
        expect(created.status.ok && created.value &&
                   created.value->accepted_count == 1,
               "Reopened fixture must create its first group");
        exception_id =
            created.value->results.front().exception_id;
        const auto created_version =
            created.value->results.front().group_version;
        original_acceptance_version = created_version;
        original_durability_ack =
            created.value->results.front().durability_ack_id;

        auto duplicate_report = original_report;
        duplicate_report.report_id =
            "report-reopened-duplicate";
        sealExceptionReport(duplicate_report, call);
        const auto duplicate =
            manager.report(duplicate_report, call);
        const auto exact_duplicate_replay =
            manager.report(duplicate_report, call);
        const auto after_duplicate =
            manager.getException(exception_id, call);
        expect(duplicate.status.ok && duplicate.value &&
                   duplicate.value->results.front().disposition ==
                       ExceptionDisposition::DuplicateOccurrence &&
                   duplicate.value->results.front().group_version ==
                       created_version &&
                   duplicate.value->results.front().durability_ack_id ==
                       original_durability_ack &&
                   exact_duplicate_replay.value &&
                   exact_duplicate_replay.value->results.front()
                           .group_version ==
                       created_version &&
                   after_duplicate.value &&
                   after_duplicate.value->version ==
                       created_version &&
                   after_duplicate.value->occurrence_count == 1 &&
                   after_duplicate.value->duplicate_replay_count == 1,
               "one new duplicate report must preserve the first "
               "acceptance/CAS version while exact ReportID replay must "
               "not increment replay telemetry again");

        ExceptionMutationRequest resolution;
        resolution.mutation_id =
            "mutation-reopened-resolve";
        resolution.exception_id = exception_id;
        resolution.expected_group_version =
            after_duplicate.value->version;
        resolution.actor_id_hash = "operator";
        resolution.actor_role = "ops";
        resolution.reason_code = "ROOT_CAUSE_VERIFIED";
        resolution.verification_evidence_refs = {
            "verification://reopened/root-cause-fixed"};
        const auto resolved = manager.resolve(resolution, call);
        expect(resolved.status.ok && resolved.value &&
                   resolved.value->group.lifecycle ==
                       ExceptionLifecycle::Resolved,
               "fixture group must reach RESOLVED with evidence");

        clock->advanceMs(1);
        auto reopening_occurrence =
            makeExceptionOccurrence(
                "occurrence-reopened-new", clock);
        ExceptionReportRequest reopening_report;
        reopening_report.report_id =
            "report-reopened-new";
        reopening_report.occurrences = {
            reopening_occurrence};
        sealExceptionReport(reopening_report, call);
        const auto reopened =
            manager.report(reopening_report, call);
        expect(reopened.status.ok && reopened.value &&
                   reopened.value->results.front().disposition ==
                       ExceptionDisposition::Reopened &&
                   reopened.value->results.front().lifecycle ==
                       ExceptionLifecycle::Reopened &&
                   reopened.value->results.front().group_version ==
                       resolved.value->group.version + 1,
               "a new occurrence after RESOLVED must commit the explicit "
               "REOPENED state instead of collapsing to OPEN");

        clock->advanceMs(1);
        auto followup_occurrence =
            makeExceptionOccurrence(
                "occurrence-reopened-followup", clock);
        ExceptionReportRequest followup_report;
        followup_report.report_id =
            "report-reopened-followup";
        followup_report.occurrences = {
            followup_occurrence};
        sealExceptionReport(followup_report, call);
        const auto followup =
            manager.report(followup_report, call);
        const auto before_restart =
            manager.getException(exception_id, call);
        expect(followup.status.ok && followup.value &&
                   followup.value->results.front().disposition ==
                       ExceptionDisposition::Aggregated &&
                   followup.value->results.front().lifecycle ==
                       ExceptionLifecycle::Reopened &&
                   before_restart.value &&
                   before_restart.value->lifecycle ==
                       ExceptionLifecycle::Reopened &&
                   before_restart.value->occurrence_count == 3 &&
                   before_restart.value->duplicate_replay_count == 1,
               "aggregation into a REOPENED group must preserve REOPENED "
               "until an authorized state transition");
        reopened_version = before_restart.value->version;
    }

    {
        ExceptionManager recovered(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-reopened-recovered"),
            log);
        expect(recovered.initialize().ok,
               "REOPENED group must recover from the authoritative journal");
        const auto group =
            recovered.getException(exception_id, call);
        expect(group.value &&
                   group.value->lifecycle ==
                       ExceptionLifecycle::Reopened &&
                   group.value->version == reopened_version &&
                   group.value->occurrence_count == 3 &&
                   group.value->duplicate_replay_count == 1,
               "recovery must preserve REOPENED, counters, and version");

        ExceptionMutationRequest stale_ack;
        stale_ack.mutation_id =
            "mutation-reopened-stale-ack";
        stale_ack.exception_id = exception_id;
        stale_ack.expected_group_version =
            reopened_version - 1;
        stale_ack.actor_id_hash = "operator";
        stale_ack.actor_role = "ops";
        stale_ack.reason_code = "STALE_REOPENED_ACK";
        const auto stale =
            recovered.acknowledge(stale_ack, call);
        expect(!stale.status.ok &&
                   stale.status.error.code ==
                       "EXM_VERSION_CONFLICT",
               "recovered REOPENED version must fence stale mutation CAS");

        auto current_ack = stale_ack;
        current_ack.mutation_id =
            "mutation-reopened-current-ack";
        current_ack.expected_group_version =
            reopened_version;
        current_ack.reason_code = "REOPENED_ACKNOWLEDGED";
        const auto acknowledged =
            recovered.acknowledge(current_ack, call);
        expect(acknowledged.status.ok && acknowledged.value &&
                   acknowledged.value->group.lifecycle ==
                       ExceptionLifecycle::Acknowledged &&
                   acknowledged.value->group.version ==
                       reopened_version + 1,
               "REOPENED -> ACKNOWLEDGED must be an allowed explicit "
               "state-machine edge");

        duplicate_after_recovery = original_report;
        duplicate_after_recovery.report_id =
            "report-reopened-post-recovery-duplicate";
        sealExceptionReport(duplicate_after_recovery, call);
        const auto duplicate = recovered.report(
            duplicate_after_recovery, call);
        const auto after_duplicate =
            recovered.getException(exception_id, call);
        expect(duplicate.status.ok && duplicate.value &&
                   duplicate.value->results.front().disposition ==
                       ExceptionDisposition::DuplicateOccurrence &&
                   duplicate.value->results.front().group_version ==
                       original_acceptance_version &&
                   duplicate.value->results.front().durability_ack_id ==
                       original_durability_ack &&
                   duplicate.value->results.front().lifecycle ==
                       ExceptionLifecycle::Open &&
                   after_duplicate.value &&
                   after_duplicate.value->version ==
                       acknowledged.value->group.version &&
                   after_duplicate.value->occurrence_count == 3 &&
                   after_duplicate.value->duplicate_replay_count == 2,
               "a duplicate after restart must durably increment replay "
               "telemetry without changing occurrence total, lifecycle, "
               "or the recovered group version");
        final_version = after_duplicate.value->version;
    }

    {
        ExceptionManager recovered_again(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-reopened-recovered-again"),
            log);
        expect(recovered_again.initialize().ok,
               "duplicate-version fixture must recover a second time");
        const auto recovered_group =
            recovered_again.getException(exception_id, call);
        const auto exact_duplicate_replay =
            recovered_again.report(
                duplicate_after_recovery, call);
        const auto after_exact_replay =
            recovered_again.getException(exception_id, call);
        expect(recovered_group.value &&
                   recovered_group.value->lifecycle ==
                       ExceptionLifecycle::Acknowledged &&
                   recovered_group.value->version == final_version &&
                   recovered_group.value->occurrence_count == 3 &&
                   recovered_group.value->duplicate_replay_count == 2 &&
                   exact_duplicate_replay.value &&
                   exact_duplicate_replay.value->results.front()
                           .group_version ==
                       original_acceptance_version &&
                   exact_duplicate_replay.value->results.front()
                           .durability_ack_id ==
                       original_durability_ack &&
                   after_exact_replay.value &&
                   after_exact_replay.value->version == final_version &&
                   after_exact_replay.value->duplicate_replay_count == 2,
               "journal recovery and exact ReportID replay must preserve "
               "the final duplicate counter/version without double apply");
    }
}

void testExceptionJournalCommitAndObservationRecovery() {
    ScopedTempDirectory temp("master-agent-exception-journal");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("exception-journal-test");
    auto durable =
        std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
    expect(durable->initialize().ok,
           "exception observation log must initialize");
    auto controlled =
        std::make_shared<ControllableDataLog>(durable);
    const auto exception_storage = temp.path() / "exceptions";
    auto manager = std::make_shared<ExceptionManager>(
        exception_storage, clock, ids, controlled);
    expect(manager->initialize().ok,
           "independent exception journal must initialize");
    ExceptionManager contender(
        exception_storage, clock,
        std::make_shared<IdGenerator>(
            "exception-journal-contender"),
        controlled, {}, false);
    const auto contender_status =
        contender.initialize();
    expect(!contender_status.ok &&
               contender_status.error.code ==
                   "EXM_WRITER_LEASE_HELD",
           "a second ExceptionManager must not share one journal directory");
    CallContext call{CallerModuleId::AgentService, "request-journal",
                     "trace-journal", "principal", TaskPriority::P1,
                     clock->monotonicNowNs() + 1'000'000'000LL};
    std::weak_ptr<ExceptionManager> weak_manager = manager;
    controlled->setProbe([weak_manager, call]() {
        if (const auto current = weak_manager.lock()) {
            (void)current->getException("probe-not-present", call);
        }
    });

    ExceptionOccurrence first_occurrence;
    first_occurrence.occurrence_id = "occurrence-journal-base";
    first_occurrence.domain = "dispatch";
    first_occurrence.code = "PROVIDER_TIMEOUT";
    first_occurrence.source_module = "AgentDispatch";
    first_occurrence.source_interface = "submitDispatch";
    first_occurrence.operation = "dispatch";
    first_occurrence.bounded_detail_summary = "provider-timeout";
    first_occurrence.occurred_at_utc_ms = clock->utcNowMs();
    ExceptionReportRequest first_report;
    first_report.report_id = "report-journal-base";
    first_report.occurrences = {first_occurrence};
    sealExceptionReport(first_report, call);
    const auto created = manager->report(first_report, call);
    expect(created.status.ok && created.value &&
               created.value->accepted_count == 1 &&
               controlled->probePassed() &&
               controlled->lastCall().caller ==
                   CallerModuleId::ExceptionManager,
           "post-commit DataLog call must use ExceptionManager identity without holding the state mutex");
    const auto exception_id =
        created.value->results.front().exception_id;
    const auto original_version =
        created.value->results.front().group_version;

    ExceptionMutationRequest mutation;
    mutation.mutation_id = "mutation-observation-rejected";
    mutation.exception_id = exception_id;
    mutation.expected_group_version = original_version;
    mutation.actor_id_hash = "operator";
    mutation.actor_role = "ops";
    mutation.reason_code = "ACK_DURABLE_BEFORE_OBSERVATION";
    controlled->rejectNextAppend();
    const auto committed_mutation =
        manager->acknowledge(mutation, call);
    const auto after_mutation =
        manager->getException(exception_id, call);
    expect(committed_mutation.status.ok &&
               committed_mutation.value &&
               after_mutation.value &&
               after_mutation.value->version ==
                   original_version + 1 &&
               after_mutation.value->lifecycle ==
                   ExceptionLifecycle::Acknowledged,
           "DataLog rejection must not roll back the independently journaled mutation");

    const auto append_count_before_recovery =
        controlled->appendCount();
    controlled->setProbe({});
    manager.reset();

    auto recovered = std::make_shared<ExceptionManager>(
        exception_storage, clock,
        std::make_shared<IdGenerator>("exception-recovered"),
        controlled);
    expect(recovered->initialize().ok,
           "restart must recover committed exception state and pending observation");
    const auto recovered_group =
        recovered->getException(exception_id, call);
    expect(recovered_group.value &&
               recovered_group.value->version ==
                   original_version + 1 &&
               recovered_group.value->lifecycle ==
                   ExceptionLifecycle::Acknowledged &&
               controlled->appendCount() ==
                   append_count_before_recovery + 1,
           "restart must redeliver exactly the unacknowledged observation");
    const auto lifecycle_audit =
        controlled->lastAuditBatch();
    expect(lifecycle_audit.records.size() == 1 &&
               lifecycle_audit.records.front().audit_type ==
                   "ExceptionLifecycleMutation" &&
               lifecycle_audit.records.front()
                       .requested_durability ==
                   DurabilityClass::D3Fsynced &&
               durable->getHealth(call).audit_records == 1,
           "exception lifecycle mutation must be projected to the "
           "recoverable Audit chain, not an ordinary Event");

    const auto append_count_after_delivery =
        controlled->appendCount();
    recovered.reset();
    ExceptionManager second_restart(
        exception_storage, clock,
        std::make_shared<IdGenerator>("exception-second-restart"),
        controlled);
    expect(second_restart.initialize().ok &&
               controlled->appendCount() ==
                   append_count_after_delivery,
           "durable observation ACK must prevent redelivery on later restarts");
}

void testExceptionRecoveredObservationPreservesCausality() {
    ScopedTempDirectory temp(
        "master-agent-exception-observation-shape");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>(
        "exception-observation-shape");
    auto durable = std::make_shared<DataLogService>(
        temp.path() / "data-log", clock, ids);
    expect(durable->initialize().ok,
           "observation-shape DataLog must initialize");
    auto controlled =
        std::make_shared<ControllableDataLog>(durable);
    controlled->setRejectAll(true);
    const auto storage = temp.path() / "exceptions";
    CallContext call{
        CallerModuleId::AgentService, "request-observation-shape",
        "trace-observation-shape", "principal", TaskPriority::P1,
        clock->monotonicNowNs() + 1'000'000'000LL};
    {
        ExceptionManager manager(storage, clock, ids, controlled);
        auto occurrence = makeExceptionOccurrence(
            "occurrence-observation-shape", clock);
        occurrence.context.request_id = call.request_id;
        occurrence.context.trace_id = call.trace_id;
        occurrence.context.span_id = "span-observation-shape";
        occurrence.context.causal_parent_event_id =
            "event-causal-parent";
        occurrence.context.session_id = "session-observation-shape";
        occurrence.context.plan_id = "plan-observation-shape";
        occurrence.context.pid = "pid-observation-shape";
        occurrence.context.activation_id =
            "activation-observation-shape";
        occurrence.context.execution_id =
            "execution-observation-shape";
        occurrence.context.boot_id = 77;
        ExceptionReportRequest report;
        report.report_id = "report-observation-shape";
        report.occurrences = {occurrence};
        sealExceptionReport(report, call);
        expect(manager.report(report, call).status.ok,
               "authoritative exception report must survive rejected projection");
    }
    const auto original = controlled->lastBatch();
    expect(original.records.size() == 1,
           "rejected projection must expose one pending Event");

    controlled->setRejectAll(false);
    ExceptionManager recovered(
        storage, clock,
        std::make_shared<IdGenerator>(
            "exception-observation-shape-recovered"),
        controlled);
    expect(recovered.initialize().ok,
           "pending observation must recover and redeliver");
    const auto replayed = controlled->lastBatch();
    expect(replayed.records.size() == 1,
           "recovered projection must redeliver one Event");
    const auto& before = original.records.front();
    const auto& after = replayed.records.front();
    expect(
        replayed.batch_id == original.batch_id &&
            replayed.redaction_proof == original.redaction_proof &&
            after.schema_version == before.schema_version &&
            after.context.span_id == before.context.span_id &&
            after.context.causal_parent_event_id ==
                before.context.causal_parent_event_id &&
            after.context.session_id == before.context.session_id &&
            after.context.plan_id == before.context.plan_id &&
            after.context.pid == before.context.pid &&
            after.context.activation_id ==
                before.context.activation_id &&
            after.context.execution_id ==
                before.context.execution_id &&
            after.context.boot_id == before.context.boot_id &&
            after.error_ref == before.error_ref &&
            after.privacy_labels == before.privacy_labels &&
            after.redaction_policy_version ==
                before.redaction_policy_version,
        "journaled outbox recovery must preserve causal, privacy and schema fields");
}

void testExceptionDurabilityUnknownIsStableAndRecoverable() {
    ScopedTempDirectory temp(
        "master-agent-exception-durability");
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids =
        std::make_shared<IdGenerator>("exception-durability");
    auto log = std::make_shared<DataLogService>(
        temp.path() / "data-log", clock, ids);
    expect(log->initialize().ok,
           "durability test DataLog must initialize");
    auto sync_calls =
        std::make_shared<std::atomic<std::size_t>>(0);
    auto sync_probe_passed =
        std::make_shared<std::atomic<bool>>(false);
    const auto exception_storage = temp.path() / "exceptions";
    CallContext call{CallerModuleId::AgentService,
                     "request-exception-durability",
                     "trace-exception-durability", "principal",
                     TaskPriority::P1,
                     clock->monotonicNowNs() + 1'000'000'000LL};
    std::weak_ptr<ExceptionManager> manager_probe;
    auto manager = std::make_shared<ExceptionManager>(
        exception_storage, clock, ids, log,
        [sync_calls, sync_probe_passed, &manager_probe,
         call](const std::filesystem::path&) -> Status {
            const auto call_no = ++(*sync_calls);
            if (call_no == 1) return Status::Ok();
            if (const auto current = manager_probe.lock()) {
                auto completion =
                    std::make_shared<std::promise<void>>();
                auto ready = completion->get_future();
                std::thread([current, call, completion]() {
                    (void)current->getException(
                        "sync-reentrant-probe", call);
                    try {
                        completion->set_value();
                    } catch (...) {
                    }
                }).detach();
                sync_probe_passed->store(
                    ready.wait_for(std::chrono::milliseconds(500)) ==
                    std::future_status::ready);
            }
            throw std::runtime_error(
                "injected exception-journal sync failure");
        });
    manager_probe = manager;
    expect(manager->initialize().ok,
           "first recovery sync must open the Ready gate");
    auto occurrence = makeExceptionOccurrence(
        "occurrence-durability-unknown", clock);
    occurrence.side_effect_state = SideEffectState::Unknown;
    ExceptionReportRequest report;
    report.report_id = "report-durability-unknown";
    report.occurrences = {occurrence};
    report.requested_durability = DurabilityClass::D3Fsynced;
    sealExceptionReport(report, call);

    const auto failed = manager->report(report, call);
    const auto retry = manager->report(report, call);
    auto different_report = report;
    different_report.report_id = "report-while-fenced";
    different_report.occurrences.front().occurrence_id =
        "occurrence-while-fenced";
    sealExceptionReport(different_report, call);
    const auto fenced =
        manager->report(different_report, call);
    const auto journal_path =
        exception_storage / "journal" / "active" /
        "exception.jsonl";
    expect(!failed.status.ok &&
               failed.status.error.code ==
                   "EXM_DURABILITY_UNKNOWN" &&
               failed.status.error.side_effect_state ==
                   SideEffectState::Unknown &&
               !retry.status.ok && !fenced.status.ok &&
               sync_calls->load() == 2 &&
               sync_probe_passed->load() &&
               readNonEmptyLines(journal_path).size() == 1,
           "reentrant throwing fsync must not deadlock and must keep same-ID retry stable without reappend");

    manager.reset();
    ExceptionManager recovered(
        exception_storage, clock,
        std::make_shared<IdGenerator>(
            "exception-durability-recovered"),
        log);
    expect(recovered.initialize().ok,
           "restart with a healthy sync must resolve the complete ambiguous frame");
    const auto replay = recovered.report(report, call);
    expect(replay.status.ok && replay.value &&
               replay.value->accepted_count == 1 &&
               readNonEmptyLines(journal_path).size() == 2,
           "recovery must publish the durable report and ACK its observation exactly once");
}

void testExceptionJournalRecoveryIntegrityRules() {
    {
        ScopedTempDirectory temp(
            "master-agent-exception-tail-repair");
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids =
            std::make_shared<IdGenerator>("exception-tail-repair");
        auto log = std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
        expect(log->initialize().ok,
               "tail-repair DataLog must initialize");
        const auto storage = temp.path() / "exceptions";
        CallContext call{CallerModuleId::AgentService,
                         "request-tail", "trace-tail", "principal",
                         TaskPriority::P1,
                         clock->monotonicNowNs() + 1'000'000'000LL};
        std::string exception_id;
        {
            ExceptionManager manager(storage, clock, ids, log);
            expect(manager.initialize().ok,
                   "tail-repair journal must initialize");
            ExceptionReportRequest report;
            report.report_id = "report-tail";
            report.occurrences = {
                makeExceptionOccurrence("occurrence-tail", clock)};
            sealExceptionReport(report, call);
            const auto created = manager.report(report, call);
            expect(created.status.ok && created.value,
                   "tail-repair seed report must commit");
            exception_id =
                created.value->results.front().exception_id;
        }
        const auto journal =
            storage / "journal" / "active" /
            "exception.jsonl";
        const auto committed_size =
            std::filesystem::file_size(journal);
        appendAllBytes(journal, "{\"schema_version\":1");
        ExceptionManager recovered(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-tail-recovered"),
            log);
        const auto initialized = recovered.initialize();
        const auto group =
            recovered.getException(exception_id, call);
        expect(initialized.ok && group.value &&
                   std::filesystem::file_size(journal) ==
                       committed_size,
               "only an unterminated physical tail may be truncated and recovered");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-exception-corrupt-frame");
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids =
            std::make_shared<IdGenerator>("exception-corrupt-frame");
        auto log = std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
        expect(log->initialize().ok,
               "corrupt-frame DataLog must initialize");
        const auto storage = temp.path() / "exceptions";
        CallContext call{CallerModuleId::AgentService,
                         "request-corrupt", "trace-corrupt",
                         "principal", TaskPriority::P1,
                         clock->monotonicNowNs() + 1'000'000'000LL};
        {
            ExceptionManager manager(storage, clock, ids, log);
            ExceptionReportRequest report;
            report.report_id = "report-corrupt";
            report.occurrences = {makeExceptionOccurrence(
                "occurrence-corrupt", clock)};
            sealExceptionReport(report, call);
            expect(manager.report(report, call).status.ok,
                   "corrupt-frame seed report must commit");
        }
        const auto journal =
            storage / "journal" / "active" /
            "exception.jsonl";
        auto bytes = readAllBytes(journal);
        const std::string marker = "\"payload_hash\":\"";
        const auto marker_at = bytes.rfind(marker);
        expect(marker_at != std::string::npos,
               "journal frame must carry a payload hash");
        const auto hash_at = marker_at + marker.size();
        bytes[hash_at] = bytes[hash_at] == '0' ? '1' : '0';
        writeAllBytes(journal, bytes);
        const auto corrupt_size =
            std::filesystem::file_size(journal);
        ExceptionManager corrupted(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-corrupt-restart"),
            log);
        const auto initialized = corrupted.initialize();
        expect(!initialized.ok &&
                   initialized.error.code ==
                       "EXM_JOURNAL_INTEGRITY" &&
                   std::filesystem::file_size(journal) ==
                       corrupt_size,
               "newline-committed corrupt frame must fail Ready without truncation");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-exception-transaction-binding");
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids = std::make_shared<IdGenerator>(
            "exception-transaction-binding");
        auto log = std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
        expect(log->initialize().ok,
               "transaction-binding DataLog must initialize");
        const auto storage = temp.path() / "exceptions";
        CallContext call{CallerModuleId::AgentService,
                         "request-binding", "trace-binding",
                         "principal", TaskPriority::P1,
                         clock->monotonicNowNs() + 1'000'000'000LL};
        {
            ExceptionManager manager(storage, clock, ids, log);
            ExceptionReportRequest report;
            report.report_id = "report-binding";
            report.occurrences = {makeExceptionOccurrence(
                "occurrence-binding", clock)};
            sealExceptionReport(report, call);
            expect(manager.report(report, call).status.ok,
                   "transaction-binding seed report must commit");
        }
        const auto journal =
            storage / "journal" / "active" /
            "exception.jsonl";
        const auto lines = readNonEmptyLines(journal);
        expect(lines.size() == 2,
               "seed report and observation ACK must form two frames");
        auto duplicate = nlohmann::json::parse(lines.front());
        duplicate["payload"]["journal_sequence"] = 3;
        duplicate["payload"]["writer_epoch"] =
            duplicate["payload"]["writer_epoch"]
                .get<std::uint64_t>() +
            1;
        refreshJournalFrame(duplicate);
        appendAllBytes(journal, duplicate.dump() + "\n");
        ExceptionManager conflicted(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-binding-restart"),
            log);
        const auto initialized = conflicted.initialize();
        expect(!initialized.ok &&
                   initialized.error.code ==
                       "EXM_JOURNAL_INTEGRITY",
               "same transaction id with a different canonical payload must fail recovery");
    }

    {
        ScopedTempDirectory temp(
            "master-agent-exception-sequence-gap");
        auto clock = std::make_shared<ManualRuntimeClock>();
        auto ids =
            std::make_shared<IdGenerator>("exception-sequence-gap");
        auto log = std::make_shared<DataLogService>(
            temp.path() / "data-log", clock, ids);
        expect(log->initialize().ok,
               "sequence-gap DataLog must initialize");
        const auto storage = temp.path() / "exceptions";
        CallContext call{CallerModuleId::AgentService,
                         "request-gap", "trace-gap", "principal",
                         TaskPriority::P1,
                         clock->monotonicNowNs() + 1'000'000'000LL};
        {
            ExceptionManager manager(storage, clock, ids, log);
            ExceptionReportRequest report;
            report.report_id = "report-gap";
            report.occurrences = {
                makeExceptionOccurrence("occurrence-gap", clock)};
            sealExceptionReport(report, call);
            expect(manager.report(report, call).status.ok,
                   "sequence-gap seed report must commit");
        }
        const auto journal =
            storage / "journal" / "active" /
            "exception.jsonl";
        auto lines = readNonEmptyLines(journal);
        expect(lines.size() == 2,
               "sequence-gap seed must have report and ACK");
        auto second = nlohmann::json::parse(lines[1]);
        second["payload"]["journal_sequence"] = 3;
        refreshJournalFrame(second);
        writeAllBytes(journal,
                      lines[0] + "\n" + second.dump() + "\n");
        ExceptionManager gap(
            storage, clock,
            std::make_shared<IdGenerator>(
                "exception-gap-restart"),
            log);
        const auto initialized = gap.initialize();
        expect(!initialized.ok &&
                   initialized.error.code ==
                       "EXM_JOURNAL_INTEGRITY",
               "journal sequence gap must fail recovery and keep Ready closed");
    }
}

}  // namespace

int main() {
    testLogIdempotencyPrivacyAndAuditChain();
    testLogRestartRecoveryAndAuditContinuation();
    testD4RequiresKeyedAnchorAndDetectsRollback();
    testD4AnchorReadbackRejectsFalseAcknowledgement();
    testLogRejectsStructuredPrivacyBypasses();
    testAuditPrivacyContractIsAtomicAndRecoverable();
    testAgentServiceSanitizesDownstreamErrorsAndConcurrentProducers();
    testTraceQueryAndHealthAuthorizationBounds();
    testLogBatchFrameAndD1FlushRecovery();
    testLogIngressValidationAndMixedDurabilityPromotion();
    testLogAmbiguousDurabilityFailureIsStable();
    testExceptionPrivacyContractIsAtomicAndRecoverable();
    testExceptionGroupingAndOccurrenceReplay();
    testExceptionReopenedLifecycleAndDuplicateVersionRecovery();
    testExceptionJournalCommitAndObservationRecovery();
    testExceptionRecoveredObservationPreservesCausality();
    testExceptionDurabilityUnknownIsStableAndRecoverable();
    testExceptionJournalRecoveryIntegrityRules();
    return 0;
}
