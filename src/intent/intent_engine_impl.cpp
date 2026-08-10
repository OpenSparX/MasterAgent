/**
 * @file intent_engine_impl.cpp
 * @brief Routes deterministic intents and selects the bounded model path.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

#include <fstream>
#include <iterator>
#include <regex>

namespace master_agent::intent {
namespace {

std::optional<RuleMatchMode> parseMatchMode(
    const std::string& value) {
    if (value == "EXACT") return RuleMatchMode::Exact;
    if (value == "REGEX_SLOT") return RuleMatchMode::RegexSlot;
    if (value == "INTENT_PATTERN") return RuleMatchMode::IntentPattern;
    return std::nullopt;
}

std::optional<RuleEffectiveStage> parseEffectiveStage(
    const std::string& value) {
    if (value == "NLU") return RuleEffectiveStage::Nlu;
    if (value == "TOOL") return RuleEffectiveStage::Tool;
    if (value == "RAG") return RuleEffectiveStage::Rag;
    return std::nullopt;
}

Result<std::vector<IntentRuleEntry>> parseRuleArtifact(
    const std::string& bytes, std::uint64_t artifact_version) {
    try {
        const auto root = nlohmann::json::parse(bytes);
        if (!root.is_object() || !root.contains("rules") ||
            !root.at("rules").is_array() ||
            root.at("rules").size() > 4096) {
            throw std::runtime_error("rule root");
        }
        std::set<std::string> rule_ids;
        std::vector<IntentRuleEntry> rules;
        for (const auto& encoded : root.at("rules")) {
            if (!encoded.is_object()) {
                throw std::runtime_error("rule entry");
            }
            IntentRuleEntry rule;
            rule.rule_id = encoded.at("rule_id").get<std::string>();
            rule.level1_category =
                encoded.at("level1_category").get<std::string>();
            rule.level2_category =
                encoded.at("level2_category").get<std::string>();
            rule.example_utterances = encoded.value(
                "example_utterances", std::vector<std::string>{});
            rule.patterns =
                encoded.at("patterns").get<std::vector<std::string>>();
            const auto match_mode = parseMatchMode(
                encoded.at("match_mode").get<std::string>());
            const auto stage = parseEffectiveStage(
                encoded.at("effective_stage").get<std::string>());
            rule.action = encoded.at("action").get<std::string>();
            rule.call_llm = encoded.at("call_llm").get<bool>();
            rule.tool_template_id =
                encoded.value("tool_template_id", std::string{});
            rule.required_slots = encoded.value(
                "required_slots", std::vector<std::string>{});
            rule.rule_priority =
                encoded.at("rule_priority").get<std::int32_t>();
            rule.enabled = encoded.at("enabled").get<bool>();
            rule.version =
                encoded.at("version").get<std::uint64_t>();
            if (!match_mode || !stage || rule.rule_id.empty() ||
                !rule_ids.insert(rule.rule_id).second ||
                rule.level1_category.empty() ||
                rule.level2_category.empty() ||
                rule.patterns.empty() || rule.patterns.size() > 128 ||
                rule.action.empty() || rule.version != artifact_version ||
                std::any_of(
                    rule.patterns.begin(), rule.patterns.end(),
                    [](const auto& pattern) {
                        return pattern.empty() || pattern.size() > 1024;
                    })) {
                throw std::runtime_error("rule invariant");
            }
            rule.match_mode = *match_mode;
            rule.effective_stage = *stage;
            if (rule.match_mode == RuleMatchMode::RegexSlot) {
                for (const auto& pattern : rule.patterns) {
                    (void)std::regex(pattern, std::regex::ECMAScript);
                }
            }
            rules.push_back(std::move(rule));
        }
        return Result<std::vector<IntentRuleEntry>>::Success(
            std::move(rules));
    } catch (...) {
        return Result<std::vector<IntentRuleEntry>>::Failure(
            Status::Error(
                "intent", "INTENT_RULE_ARTIFACT_SCHEMA_INVALID",
                "rule artifact does not satisfy the versioned schema"));
    }
}

struct RuleCandidate {
    IntentRuleEntry rule;
    std::size_t matched_length = 0;
};

std::optional<IntentRuleEntry> matchRule(
    const std::string& normalized_text,
    const std::vector<IntentRuleEntry>& rules) {
    std::vector<RuleCandidate> candidates;
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        std::size_t best_length = 0;
        for (const auto& pattern : rule.patterns) {
            bool matched = false;
            if (rule.match_mode == RuleMatchMode::Exact) {
                matched = normalized_text == pattern;
            } else if (rule.match_mode == RuleMatchMode::RegexSlot) {
                matched = std::regex_match(
                    normalized_text,
                    std::regex(pattern, std::regex::ECMAScript));
            } else {
                matched = normalized_text.find(pattern) !=
                          std::string::npos;
            }
            if (matched) best_length = std::max(best_length, pattern.size());
        }
        if (best_length != 0) {
            candidates.push_back({rule, best_length});
        }
    }
    std::sort(
        candidates.begin(), candidates.end(),
        [](const auto& left, const auto& right) {
            if (left.matched_length != right.matched_length) {
                return left.matched_length > right.matched_length;
            }
            if (left.rule.rule_priority !=
                right.rule.rule_priority) {
                return left.rule.rule_priority >
                       right.rule.rule_priority;
            }
            return left.rule.rule_id < right.rule.rule_id;
        });
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() > 1 &&
        candidates[0].matched_length == candidates[1].matched_length &&
        candidates[0].rule.rule_priority ==
            candidates[1].rule.rule_priority &&
        (candidates[0].rule.action != candidates[1].rule.action ||
         candidates[0].rule.tool_template_id !=
             candidates[1].rule.tool_template_id)) {
        return std::nullopt;
    }
    return candidates.front().rule;
}

bool matchesToolSchema(
    const nlohmann::json& schema,
    const nlohmann::json& arguments) {
    if (!schema.is_object() || !arguments.is_object() ||
        schema.value("type", std::string{}) != "object" ||
        !schema.contains("properties") ||
        !schema.at("properties").is_object()) {
        return false;
    }
    for (const auto& required :
         schema.value("required", nlohmann::json::array())) {
        if (!required.is_string() ||
            !arguments.contains(required.get<std::string>())) {
            return false;
        }
    }
    for (const auto& item : arguments.items()) {
        const auto property = schema.at("properties").find(item.key());
        if (property == schema.at("properties").end()) {
            if (!schema.value("additionalProperties", true)) return false;
            continue;
        }
        const auto type = property->value("type", std::string{});
        const bool type_valid =
            (type == "string" && item.value().is_string()) ||
            (type == "integer" && item.value().is_number_integer()) ||
            (type == "number" && item.value().is_number()) ||
            (type == "boolean" && item.value().is_boolean()) ||
            (type == "object" && item.value().is_object()) ||
            (type == "array" && item.value().is_array());
        if (!type_valid) return false;
        if (property->contains("enum") &&
            std::find(property->at("enum").begin(),
                      property->at("enum").end(), item.value()) ==
                property->at("enum").end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

IntentEngine::IntentEngine(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<intent_support::IIntentSkillResolver> skill,
    std::shared_ptr<intent_support::IIntentPromptAssembler> prompt,
    std::shared_ptr<inference::IInferenceFramework> inference,
    std::shared_ptr<IBgeIntentClassifier> bge,
    std::shared_ptr<IIntentQueryExecutor> query_executor,
    std::shared_ptr<IIntentResultListener> result_listener)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      skill_(std::move(skill)),
      prompt_(std::move(prompt)),
      inference_(std::move(inference)),
      bge_(std::move(bge)),
      query_executor_(std::move(query_executor)),
      result_listener_(std::move(result_listener)) {
    if (!bge_) {
        bge_ =
            createDeterministicMockBgeClassifier(clock_);
    }
    worker_ = std::thread(
        [this] { workerLoop(); });
}

IntentEngine::~IntentEngine() {
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        stopping_ = true;
    }
    jobs_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

IntentJobAcceptance IntentEngine::submit(
    const interaction::StandardRequest& request,
    const IntentContext& context,
    const std::string& idempotency_key,
    const CallContext& call) {
    if (!clock_ || !ids_ ||
        !hasHostModuleIdentity(
            call, CallerModuleId::AgentService) ||
        idempotency_key.empty() ||
        request.request_id.empty() ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        context.deadline_mono_ns !=
            call.deadline_mono_ns ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, {},
                "INTENT_SUBMIT_INVALID"};
    }
    const auto ledger =
        scopedIdempotencyLedgerKey(
            call.principal_id_hash,
            idempotency_key);
    const auto digest = secureDigest(
        request.request_id + "|" + request.session_id +
        "|" + std::to_string(request.turn_id) + "|" +
        context.preprocess_result.normalized_request.text +
        "|" + std::to_string(context.context_version) +
        "|" + toString(context.priority));
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    const auto replay =
        idempotency_to_job_.find(ledger);
    if (replay != idempotency_to_job_.end()) {
        const auto& existing = jobs_.at(replay->second);
        if (existing.request_digest != digest) {
            return {false, false, {},
                    "INTENT_IDEMPOTENCY_CONFLICT"};
        }
        return {true, true,
                existing.snapshot.job_id, {}};
    }
    JobRecord record;
    record.snapshot.job_id =
        ids_->next("intent-job");
    record.snapshot.request_id = request.request_id;
    record.snapshot.state = IntentJobState::Queued;
    record.snapshot.priority = context.priority;
    record.snapshot.deadline_mono_ns =
        context.deadline_mono_ns;
    record.snapshot.enqueue_sequence =
        ++enqueue_sequence_;
    record.request = request;
    record.context = context;
    record.call = call;
    record.idempotency_key = idempotency_key;
    record.request_digest = digest;
    record.rule_snapshot = rule_snapshot_;
    const auto job_id = record.snapshot.job_id;
    jobs_[job_id] = std::move(record);
    idempotency_to_job_[ledger] = job_id;
    jobs_cv_.notify_one();
    return {true, false, job_id, {}};
}

Result<IntentJob> IntentEngine::getResult(
    const std::string& job_id,
    const CallContext& call) const {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService) ||
        !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<IntentJob>::Failure(
            Status::Error(
                "intent", "INTENT_QUERY_INVALID",
                "intent job query is invalid"));
    }
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        return Result<IntentJob>::Failure(
            Status::Error(
                "intent", "INTENT_JOB_NOT_FOUND",
                "intent job was not found"));
    }
    if (call.request_id != found->second.request.request_id ||
        call.trace_id != found->second.request.trace_id ||
        call.principal_id_hash !=
            found->second.call.principal_id_hash) {
        return Result<IntentJob>::Failure(
            Status::Error(
                "intent",
                "INTENT_JOB_IDENTITY_MISMATCH",
                "intent job identity does not match"));
    }
    return Result<IntentJob>::Success(
        found->second.snapshot);
}

Status IntentEngine::cancel(
    const std::string& job_id,
    const std::string& reason,
    std::uint64_t control_epoch,
    const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService) ||
        !clock_ || reason.empty() ||
        control_epoch == 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "intent", "INTENT_CANCEL_INVALID",
            "intent cancellation is invalid");
    }
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        return Status::Error(
            "intent", "INTENT_JOB_NOT_FOUND",
            "intent job was not found");
    }
    auto& record = found->second;
    if (call.request_id != record.request.request_id ||
        call.trace_id != record.request.trace_id ||
        call.principal_id_hash !=
            record.call.principal_id_hash) {
        return Status::Error(
            "intent",
            "INTENT_JOB_IDENTITY_MISMATCH",
            "intent job identity does not match");
    }
    if (control_epoch < record.snapshot.control_epoch) {
        return Status::Error(
            "intent", "INTENT_CONTROL_EPOCH_STALE",
            "intent control epoch is stale");
    }
    if (record.snapshot.state ==
            IntentJobState::Completed ||
        record.snapshot.state == IntentJobState::Failed ||
        record.snapshot.state ==
            IntentJobState::Cancelled) {
        return Status::Ok();
    }
    record.snapshot.control_epoch = control_epoch;
    record.cancellation_requested = true;
    if (record.snapshot.state == IntentJobState::Queued) {
        record.snapshot.state =
            IntentJobState::Cancelled;
        record.snapshot.stage = "CANCELLED";
        IntentOrchestrationResult result;
        result.request_id =
            record.request.request_id;
        result.outcome_type =
            IntentOutcomeType::Cancelled;
        result.reason_code = reason;
        result.trace_id = record.request.trace_id;
        result.completed_at_utc_ms =
            clock_->utcNowMs();
        record.snapshot.result = std::move(result);
    }
    return Status::Ok();
}

Status IntentEngine::reloadRules(
    const RuleSetArtifactRef& artifact,
    std::uint64_t expected_version,
    const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService) ||
        !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        artifact.artifact_id.empty() ||
        artifact.version == 0 ||
        artifact.schema_version != 1 ||
        artifact.content_digest.empty() ||
        artifact.read_only_uri.empty()) {
        return Status::Error(
            "intent", "INTENT_RULE_ARTIFACT_INVALID",
            "rule artifact metadata is invalid");
    }
    std::ifstream input(
        artifact.read_only_uri,
        std::ios::binary);
    if (!input.is_open()) {
        return Status::Error(
            "intent", "INTENT_RULE_ARTIFACT_UNREADABLE",
            "rule artifact cannot be read");
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (bytes.empty() ||
        bytes.size() > 16U * 1024U * 1024U ||
        secureDigest(bytes) !=
            artifact.content_digest) {
        return Status::Error(
            "intent",
            "INTENT_RULE_ARTIFACT_DIGEST_MISMATCH",
            "rule artifact content is invalid");
    }
    const auto parsed = parseRuleArtifact(bytes, artifact.version);
    if (!parsed.status.ok || !parsed.value) return parsed.status;
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    if (rule_artifact_.version != expected_version ||
        artifact.version <= rule_artifact_.version) {
        return Status::Error(
            "intent",
            "INTENT_RULE_VERSION_CONFLICT",
            "rule artifact CAS version conflict");
    }
    rule_artifact_ = artifact;
    rule_snapshot_ = *parsed.value;
    return Status::Ok();
}

// Deterministic direct replies and skills are evaluated first. Only NoMatch
// enters the bounded model path, so known commands do not depend on model output.
Result<IntentOrchestrationResult> IntentEngine::process(
    const interaction::StandardRequest& request,
    const IntentContext& context, const CallContext& call) {
    std::vector<IntentRuleEntry> rules;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        rules = rule_snapshot_;
    }
    return processWithRuleSnapshot(request, context, call, rules);
}

Result<IntentOrchestrationResult>
IntentEngine::processWithRuleSnapshot(
    const interaction::StandardRequest& request,
    const IntentContext& context, const CallContext& call,
    const std::vector<IntentRuleEntry>& rules,
    const std::string& job_id) {

    if (!hasHostModuleIdentity(call, CallerModuleId::AgentService)) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_CALLER_NOT_ALLOWED",
            "only AgentService may submit intent work"));
    }
    if (!clock_ || !ids_ || !skill_ || !prompt_ || !inference_ ||
        !bge_ ||
        request.request_id.empty() || context.deadline_mono_ns <= 0 ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.priority != context.priority ||
        call.deadline_mono_ns != context.deadline_mono_ns ||
        context.session_id != request.session_id ||
        context.turn_id != request.turn_id ||
        context.preprocess_result.normalized_request.request_id !=
            request.request_id ||
        context.preprocess_result.normalized_request.session_id !=
            request.session_id ||
        context.expected_capability_digest.empty() ||
        (context.priority == TaskPriority::P0 &&
         context.p0_authorization_ref.rfind(
             "trusted-safety:", 0) != 0) ||
        deadlineExpired(context.deadline_mono_ns, *clock_)) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_REQUEST_INVALID",
            "intent request or dependencies are invalid"));
    }
    const auto& text =
        context.preprocess_result.normalized_request.text;
    if (isGreeting(text)) {
        IntentOrchestrationResult result;
        result.request_id = request.request_id;
        result.outcome_type = IntentOutcomeType::DirectReply;
        result.user_reply = text == u8"谢谢" ? u8"不客气。"
                                              : u8"你好，有什么可以帮你？";
        result.reason_code = "RULE_DIRECT_REPLY";
        result.trace_id = request.trace_id;
        result.completed_at_utc_ms = clock_->utcNowMs();
        return Result<IntentOrchestrationResult>::Success(std::move(result));
    }

    const auto matched_rule = matchRule(text, rules);

    CallContext intent_call = makeChildCallContext(
        call, CallerModuleId::IntentRecognitionEngine);
    if (!context.p0_authorization_ref.empty()) {
        intent_call.authorization_ref =
            context.p0_authorization_ref;
    }
    const auto deterministic =
        skill_->resolveDeterministic(text, intent_call);
    if (deterministic.state !=
        intent_support::DeterministicResolutionState::NoMatch) {
        // A synchronous Skill implementation is still an external module
        // boundary.  Its result may not be promoted after the original
        // request deadline merely because the call began in time.
        if (deadlineExpired(context.deadline_mono_ns, *clock_)) {
            return Result<IntentOrchestrationResult>::Failure(
                Status::Error(
                    "intent", "INTENT_RESULT_AFTER_DEADLINE",
                    "intent result arrived after the request deadline"));
        }
        auto resolution = deterministic;
        if (matched_rule && !matched_rule->call_llm) {
            resolution.reason_code =
                "RULE_RESOLVED:" + matched_rule->rule_id;
        }
        return Result<IntentOrchestrationResult>::Success(
            fromSkill(request, context, resolution, intent_call));
    }
    if (matched_rule && !matched_rule->call_llm) {
        std::vector<std::string> missing_slots;
        nlohmann::json arguments = nlohmann::json::object();
        for (const auto& slot : matched_rule->required_slots) {
            const auto value = request.params.find(slot);
            if (value == request.params.end() || value->second.empty()) {
                missing_slots.push_back(slot);
            } else {
                arguments[slot] = value->second;
            }
        }
        if (!missing_slots.empty()) {
            IntentOrchestrationResult clarify;
            clarify.request_id = request.request_id;
            clarify.outcome_type = IntentOutcomeType::Clarify;
            clarify.user_reply =
                u8"请补充执行该操作所需的信息。";
            clarify.reason_code =
                "RULE_REQUIRED_SLOT_MISSING:" +
                matched_rule->rule_id;
            clarify.trace_id = request.trace_id;
            clarify.completed_at_utc_ms = clock_->utcNowMs();
            return Result<IntentOrchestrationResult>::Success(
                std::move(clarify));
        }
        const auto tool_name =
            matched_rule->action.rfind("com_", 0) == 0
                ? matched_rule->action
                : matched_rule->tool_template_id;
        const auto tools = skill_->listAvailableTools(intent_call);
        const auto registered_tool =
            tools.status.ok && tools.value
                ? std::find_if(
                      tools.value->begin(), tools.value->end(),
                      [&tool_name](const auto& tool) {
                          return tool.name == tool_name;
                      })
                : std::vector<atomic_service::McpToolDefinition>::iterator{};
        const bool tool_registered =
            tools.status.ok && tools.value &&
            registered_tool != tools.value->end();
        if (tool_registered &&
            matchesToolSchema(
                registered_tool->input_schema, arguments)) {
            intent_support::DeterministicResolution resolution;
            resolution.state = intent_support::
                DeterministicResolutionState::Resolved;
            resolution.tool_name = tool_name;
            resolution.arguments = std::move(arguments);
            resolution.user_reply = u8"好的，正在执行。";
            resolution.reason_code =
                "RULE_RESOLVED:" + matched_rule->rule_id;
            return Result<IntentOrchestrationResult>::Success(
                fromSkill(request, context, resolution, intent_call));
        }
    }
    updateJobStage(
        job_id, IntentJobState::BgeClassifying,
        "BGE_CLASSIFYING");
    const auto classification =
        bge_->classify(text, intent_call);
    if (classification.status.ok && classification.value) {
        recordJobBgeResult(job_id, *classification.value);
    }
    if (classification.status.ok &&
        classification.value &&
        classification.value->accepted) {
        const auto resolved = fromBge(
            request, context,
            *classification.value, intent_call);
        if (resolved) {
            return Result<IntentOrchestrationResult>::Success(
                *resolved);
        }
    }
    updateJobStage(
        job_id, IntentJobState::FirstInference,
        "FIRST_INFERENCE");
    return runMockInferencePath(
        request, context, intent_call, job_id);
}

void IntentEngine::updateJobStage(
    const std::string& job_id,
    IntentJobState state,
    const std::string& stage) {
    if (job_id.empty()) return;
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end() ||
        found->second.snapshot.state == IntentJobState::Completed ||
        found->second.snapshot.state == IntentJobState::Failed ||
        found->second.snapshot.state == IntentJobState::Cancelled) {
        return;
    }
    found->second.snapshot.state = state;
    found->second.snapshot.stage = stage;
}

void IntentEngine::recordJobBgeResult(
    const std::string& job_id,
    const BgeClassifierResult& result) {
    if (job_id.empty()) return;
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    const auto found = jobs_.find(job_id);
    if (found != jobs_.end()) {
        found->second.snapshot.bge_result = result;
    }
}

std::optional<IntentOrchestrationResult>
IntentEngine::fromBge(
    const interaction::StandardRequest& request,
    const IntentContext& context,
    const BgeClassifierResult& classification,
    const CallContext& call) const {
    intent_support::DeterministicResolution resolution;
    resolution.state =
        intent_support::DeterministicResolutionState::Resolved;
    resolution.reason_code =
        "BGE_CLASSIFIER_RESOLVED";
    if (classification.top1_label ==
        "CLIMATE_CIRCULATION_AUTO") {
        resolution.tool_name =
            "com_sgm_service_climate_setAirCirculationMode";
        resolution.arguments =
            nlohmann::json{{"mode", "AUTO"}};
        resolution.user_reply =
            u8"好的，正在自动调节空气循环。";
    } else if (classification.top1_label ==
               "CLIMATE_FAN_NORMAL") {
        resolution.tool_name =
            "com_sgm_service_climate_setAutoFanSpeed";
        resolution.arguments =
            nlohmann::json{{"location", "FRONT"},
                           {"mode", "NORMAL"}};
        resolution.user_reply =
            u8"好的，正在将前排风速调节为舒适档。";
    } else {
        return std::nullopt;
    }
    const auto tools = skill_->listAvailableTools(call);
    if (!tools.status.ok || !tools.value ||
        std::none_of(
            tools.value->begin(), tools.value->end(),
            [&resolution](const auto& tool) {
                return tool.name ==
                       resolution.tool_name;
            })) {
        return std::nullopt;
    }
    return fromSkill(
        request, context, resolution, call);
}

void IntentEngine::workerLoop() {
    for (;;) {
        std::string job_id;
        {
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            jobs_cv_.wait(lock, [this] {
                return stopping_ ||
                       std::any_of(
                           jobs_.begin(), jobs_.end(),
                           [](const auto& item) {
                               return item.second.snapshot.state ==
                                      IntentJobState::Queued;
                           });
            });
            if (stopping_) return;
            const auto selected = std::min_element(
                jobs_.begin(), jobs_.end(),
                [](const auto& left, const auto& right) {
                    const bool left_queued =
                        left.second.snapshot.state ==
                        IntentJobState::Queued;
                    const bool right_queued =
                        right.second.snapshot.state ==
                        IntentJobState::Queued;
                    if (left_queued != right_queued) {
                        return left_queued;
                    }
                    if (!left_queued) return false;
                    return std::tie(
                               left.second.snapshot.priority,
                               left.second.snapshot.deadline_mono_ns,
                               left.second.snapshot.enqueue_sequence) <
                           std::tie(
                               right.second.snapshot.priority,
                               right.second.snapshot.deadline_mono_ns,
                               right.second.snapshot.enqueue_sequence);
                });
            if (selected == jobs_.end() ||
                selected->second.snapshot.state !=
                    IntentJobState::Queued) {
                continue;
            }
            job_id = selected->first;
            selected->second.snapshot.state =
                IntentJobState::RuleMatching;
            selected->second.snapshot.stage =
                "RULE_BGE_OR_MODEL";
        }

        interaction::StandardRequest request;
        IntentContext context;
        CallContext call;
        std::vector<IntentRuleEntry> rules;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            const auto& record = jobs_.at(job_id);
            request = record.request;
            context = record.context;
            call = record.call;
            rules = record.rule_snapshot;
        }
        const auto resolved =
            processWithRuleSnapshot(
                request, context, call, rules, job_id);
        std::optional<IntentJob> callback_job;
        std::optional<IntentOrchestrationResult> callback_result;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            auto& record = jobs_.at(job_id);
            if (record.cancellation_requested) {
                record.snapshot.state =
                    IntentJobState::Cancelled;
                record.snapshot.stage = "CANCELLED";
                IntentOrchestrationResult cancelled;
                cancelled.request_id = request.request_id;
                cancelled.outcome_type =
                    IntentOutcomeType::Cancelled;
                cancelled.reason_code =
                    "INTENT_CANCELLED_AT_SAFE_BOUNDARY";
                cancelled.trace_id = request.trace_id;
                cancelled.completed_at_utc_ms =
                    clock_->utcNowMs();
                record.snapshot.result =
                    std::move(cancelled);
            } else if (resolved.status.ok &&
                       resolved.value) {
                record.snapshot.state =
                    IntentJobState::Completed;
                record.snapshot.stage = "COMPLETED";
                record.snapshot.result = *resolved.value;
            } else {
                record.snapshot.state =
                    IntentJobState::Failed;
                record.snapshot.stage = "FAILED";
                record.snapshot.error_code =
                    resolved.status.error.code;
            }
            if (result_listener_ && record.snapshot.result) {
                callback_job = record.snapshot;
                callback_result = *record.snapshot.result;
            }
        }

        // A callback is an external module boundary. Never invoke it while
        // holding the intent state lock; getResult remains available if the
        // receiver rejects or loses this delivery.
        if (callback_job && callback_result) {
            try {
                auto callback_call = makeChildCallContext(
                    call,
                    CallerModuleId::IntentRecognitionEngine);
                (void)result_listener_->onIntentCompleted(
                    *callback_job, *callback_result,
                    callback_call);
            } catch (...) {
                // Pull recovery through getResult preserves the terminal
                // result even when an in-process listener is faulty.
            }
        }
    }
}


}  // namespace master_agent::intent
