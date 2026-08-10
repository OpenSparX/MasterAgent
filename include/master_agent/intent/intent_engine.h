#pragma once

/**
 * @file intent_engine.h
 * @brief Defines deterministic intent outcomes and the bounded two-stage inference protocol.
 */

#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/intent/intent_support.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/preprocess/preprocess_engine.h"

namespace master_agent::intent {

struct BgeClassifierConfig {
    std::string encoder_id = "mock-bge-encoder";
    std::string encoder_digest;
    std::string classification_head_id = "mock-intent-head";
    std::string classification_head_digest;
    std::uint32_t label_schema_version = 1;
    std::map<std::string, double> per_label_thresholds;
    std::map<std::string, double> per_label_margin_thresholds;
    double temperature = 1.0;
    std::string calibration_dataset_version = "mock-calibration-v1";
    std::string ood_method = "CLASS_PROTOTYPE_COSINE";
    std::string ood_prototype_digest;
    double default_ood_distance_threshold = 0.35;
    std::map<std::string, double>
        per_label_ood_distance_thresholds;
    std::size_t max_sequence_tokens = 256;
};

struct BgeClassifierResult {
    std::string top1_label;
    double top1_probability = 0.0;
    std::string top2_label;
    double top2_probability = 0.0;
    double margin = 0.0;
    double temperature = 1.0;
    std::string ood_method;
    double ood_score = 1.0;
    bool ood_detected = true;
    bool accepted = false;
    std::string encoder_version;
    std::string head_version;
    std::uint32_t label_schema_version = 0;
};

class IBgeIntentClassifier {
public:
    virtual ~IBgeIntentClassifier() = default;

    virtual Result<BgeClassifierResult> classify(
        const std::string& normalized_text,
        const CallContext& call) const = 0;

    virtual BgeClassifierConfig config() const = 0;
};

std::shared_ptr<IBgeIntentClassifier>
createDeterministicMockBgeClassifier(
    std::shared_ptr<IRuntimeClock> clock);

struct IntentContext {
    preprocess::PreprocessResult preprocess_result;
    memory::MemoryContext memory_context;
    std::string session_id;
    std::uint64_t turn_id = 0;
    std::uint64_t context_version = 0;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string expected_capability_digest;
    std::string p0_authorization_ref;
};

enum class IntentOutcomeType : std::uint8_t {
    DeterministicPlan,
    Clarify,
    DirectReply,
    Failed,
    Cancelled
};

/**
 * @brief Closed union produced by intent processing.
 *
 * task_dag is present only for DeterministicPlan. Clarify and DirectReply use
 * user_reply and bypass orchestration. Failed and Cancelled are terminal.
 */
struct IntentOrchestrationResult {
    std::string request_id;
    IntentOutcomeType outcome_type = IntentOutcomeType::Failed;
    std::optional<orchestrator::IntentDAG> task_dag;
    std::string user_reply;
    std::string reason_code;
    std::string trace_id;
    std::int64_t completed_at_utc_ms = 0;
};

struct QuerySpec {
    std::string query_id;
    std::string capability;
    nlohmann::json arguments = nlohmann::json::object();
    bool read_only = true;
    std::int64_t deadline_mono_ns = 0;
    std::string idempotency_key;
};

struct QueryResultRecord {
    std::string query_id;
    std::string capability;
    nlohmann::json normalized_arguments =
        nlohmann::json::object();
    std::string status;
    nlohmann::json result = nlohmann::json::object();
    std::string source;
    std::uint64_t source_version = 0;
    std::int64_t observed_at_utc_ms = 0;
    std::string error_code;
};

class IIntentQueryExecutor {
public:
    virtual ~IIntentQueryExecutor() = default;

    virtual QueryResultRecord execute(
        const QuerySpec& query,
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call) const = 0;
};

std::shared_ptr<IIntentQueryExecutor>
createModuleIntentQueryExecutor(
    std::shared_ptr<preprocess::IStateQuery> preprocess,
    std::shared_ptr<memory::IMemoryService> memory,
    std::shared_ptr<intent_support::IIntentSkillResolver> skill,
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<IRuntimeClock> clock);

enum class IntentJobState : std::uint8_t {
    Queued,
    RuleMatching,
    BgeClassifying,
    FirstInference,
    QueryBatch,
    SecondInference,
    Completed,
    Failed,
    Cancelled
};

struct IntentJob {
    std::string job_id;
    std::string request_id;
    IntentJobState state = IntentJobState::Queued;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::uint64_t enqueue_sequence = 0;
    std::string stage = "QUEUED";
    std::optional<IntentOrchestrationResult> result;
    std::optional<BgeClassifierResult> bge_result;
    std::string error_code;
    std::uint64_t control_epoch = 0;
};

struct IntentJobAcceptance {
    bool accepted = false;
    bool existing = false;
    std::string job_id;
    std::string reject_code;
};

struct IntentCompletionAck {
    bool accepted = false;
    bool duplicate = false;
    std::string job_id;
    std::string reject_code;
};

/**
 * @brief Receives the unique terminal result of an asynchronous intent job.
 *
 * Agent Service implementations may use this callback for push delivery and
 * still use getResult as the authoritative pull/recovery path. The callback
 * carries the same immutable job and result identities returned by getResult.
 */
class IIntentResultListener {
public:
    virtual ~IIntentResultListener() = default;

    virtual IntentCompletionAck onIntentCompleted(
        const IntentJob& job,
        const IntentOrchestrationResult& result,
        const CallContext& call) = 0;
};

struct RuleSetArtifactRef {
    std::string artifact_id;
    std::uint64_t version = 0;
    std::string content_digest;
    std::string signature_ref;
    std::uint32_t schema_version = 1;
    std::filesystem::path read_only_uri;
};

enum class RuleMatchMode : std::uint8_t {
    Exact,
    RegexSlot,
    IntentPattern
};

enum class RuleEffectiveStage : std::uint8_t {
    Nlu,
    Tool,
    Rag
};

/**
 * @brief Immutable rule entry loaded from a digest-sealed rule artifact.
 *
 * Design trace: Intent Recognition Engine Detailed Design, section 3.4.
 * Tool parameters are still bound from trusted StandardRequest.params and
 * validated against the current Tool catalog; rules never bypass schema or
 * capability admission.
 */
struct IntentRuleEntry {
    std::string rule_id;
    std::string level1_category;
    std::string level2_category;
    std::vector<std::string> example_utterances;
    std::vector<std::string> patterns;
    RuleMatchMode match_mode = RuleMatchMode::Exact;
    RuleEffectiveStage effective_stage = RuleEffectiveStage::Nlu;
    std::string action;
    bool call_llm = false;
    std::string tool_template_id;
    std::vector<std::string> required_slots;
    std::int32_t rule_priority = 0;
    bool enabled = true;
    std::uint64_t version = 1;
};

/**
 * @brief Produces exactly one closed intent outcome for a normalized request.
 *
 * The engine may use at most two model calls. Clarify, DirectReply, and Failed
 * are terminal outcomes; only DeterministicPlan carries an executable DAG.
 */
class IIntentEngine {
public:
    virtual ~IIntentEngine() = default;

    virtual IntentJobAcceptance submit(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const std::string& idempotency_key,
        const CallContext& call) = 0;

    virtual Result<IntentJob> getResult(
        const std::string& job_id,
        const CallContext& call) const = 0;

    virtual Status cancel(
        const std::string& job_id, const std::string& reason,
        std::uint64_t control_epoch,
        const CallContext& call) = 0;

    /**
     * @brief Resolves deterministic skills or runs the bounded model protocol.
     * @return One validated intent outcome; raw model JSON is never exposed.
     *
     * The first model phase may request one complete read-only query batch.
     * The optional second phase consumes frozen evidence and must be final.
     */
    virtual Result<IntentOrchestrationResult> process(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call) = 0;

    virtual Status reloadRules(
        const RuleSetArtifactRef& artifact,
        std::uint64_t expected_version,
        const CallContext& call) = 0;
};

class IntentEngine final : public IIntentEngine {
public:
    IntentEngine(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<intent_support::IIntentSkillResolver> skill,
        std::shared_ptr<intent_support::IIntentPromptAssembler> prompt,
        std::shared_ptr<inference::IInferenceFramework> inference,
        std::shared_ptr<IBgeIntentClassifier> bge = nullptr,
        std::shared_ptr<IIntentQueryExecutor> query_executor = nullptr,
        std::shared_ptr<IIntentResultListener> result_listener = nullptr);

    ~IntentEngine() override;

    IntentJobAcceptance submit(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const std::string& idempotency_key,
        const CallContext& call) override;

    Result<IntentJob> getResult(
        const std::string& job_id,
        const CallContext& call) const override;

    Status cancel(
        const std::string& job_id, const std::string& reason,
        std::uint64_t control_epoch,
        const CallContext& call) override;

    Result<IntentOrchestrationResult> process(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call) override;

    Status reloadRules(
        const RuleSetArtifactRef& artifact,
        std::uint64_t expected_version,
        const CallContext& call) override;

private:
    IntentOrchestrationResult fromSkill(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const intent_support::DeterministicResolution& resolution,
        const CallContext& call) const;

    Result<IntentOrchestrationResult> runMockInferencePath(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call,
        const std::string& job_id = {});

    Result<IntentOrchestrationResult> processWithRuleSnapshot(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const CallContext& call,
        const std::vector<IntentRuleEntry>& rules,
        const std::string& job_id = {});

    void updateJobStage(
        const std::string& job_id,
        IntentJobState state,
        const std::string& stage);

    void recordJobBgeResult(
        const std::string& job_id,
        const BgeClassifierResult& result);

    Result<IntentOrchestrationResult> parseModelDecision(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const inference::InferenceOutput& output,
        const CallContext& call) const;

    std::optional<IntentOrchestrationResult> fromBge(
        const interaction::StandardRequest& request,
        const IntentContext& context,
        const BgeClassifierResult& classification,
        const CallContext& call) const;

    void workerLoop();

    struct JobRecord {
        IntentJob snapshot;
        interaction::StandardRequest request;
        IntentContext context;
        CallContext call;
        std::string idempotency_key;
        std::string request_digest;
        // Frozen at submit() so reloadRules() cannot alter queued work.
        std::vector<IntentRuleEntry> rule_snapshot;
        bool cancellation_requested = false;
    };

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<intent_support::IIntentSkillResolver> skill_;
    std::shared_ptr<intent_support::IIntentPromptAssembler> prompt_;
    std::shared_ptr<inference::IInferenceFramework> inference_;
    std::shared_ptr<IBgeIntentClassifier> bge_;
    std::shared_ptr<IIntentQueryExecutor> query_executor_;
    std::shared_ptr<IIntentResultListener> result_listener_;
    mutable std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_;
    std::map<std::string, JobRecord> jobs_;
    std::map<std::string, std::string> idempotency_to_job_;
    std::uint64_t enqueue_sequence_ = 0;
    bool stopping_ = false;
    std::thread worker_;
    RuleSetArtifactRef rule_artifact_;
    std::vector<IntentRuleEntry> rule_snapshot_;
};

}  // namespace master_agent::intent
