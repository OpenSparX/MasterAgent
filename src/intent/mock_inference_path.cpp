/**
 * @file mock_inference_path.cpp
 * @brief Builds and executes the two-stage deterministic model mock flow.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

namespace master_agent::intent {

Result<IntentOrchestrationResult>
IntentEngine::runMockInferencePath(
    const interaction::StandardRequest& request,
    const IntentContext& context, const CallContext& call,
    const std::string& job_id) {

    // Phase one classifies the request as a deterministic task or a bounded
    // information query. Its raw text is never returned directly to ingress.
    const auto package = prompt_->assembleIntentPrompt(
        context.preprocess_result.normalized_request,
        context.memory_context, call);
    if (!package.status.ok || !package.value) {
        return Result<IntentOrchestrationResult>::Failure(package.status);
    }
    inference::InferenceRequest inference_request;
    inference_request.job_id = ids_->next("intent-inference");
    inference_request.request_id = request.request_id;
    inference_request.session_id = request.session_id;
    inference_request.prompt = package.value->prompt;
    inference_request.prompt_digest = package.value->prompt_digest;
    inference_request.prompt_segments = package.value->segments;
    inference_request.prompt_protocol_version =
        package.value->protocol_version;
    inference_request.inference_phase = "FIRST_INFERENCE";
    inference_request.priority = context.priority;
    inference_request.deadline_mono_ns = context.deadline_mono_ns;
    inference_request.idempotency_key =
        "intent-inference-first|" + request.request_id + "|" +
        std::to_string(request.turn_id);
    inference_request.trace_id = request.trace_id;
    inference_request.admission.principal_id = call.principal_id_hash;
    inference_request.admission.caller_module_id =
        CallerModuleId::IntentRecognitionEngine;
    inference_request.admission.source_request_id = request.request_id;
    inference_request.admission.granted_priority = context.priority;
    inference_request.admission.p0_authorization =
        context.priority == TaskPriority::P0 &&
        !context.p0_authorization_ref.empty();
    inference_request.admission.policy_snapshot_id =
        "intent-admission-v1";
    inference_request.admission.allowed_model_profiles = {
        inference_request.model};
    inference_request.admission.deadline_mono_ns =
        context.deadline_mono_ns;
    inference_request.admission.signature_ref =
        context.p0_authorization_ref;
    const auto accepted =
        inference_->submitInference(inference_request, call);
    if (!accepted.accepted) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", accepted.reject_code,
            "inference framework rejected intent job"));
    }
    const auto pumped = inference_->runUntilIdle();
    if (!pumped.ok) {
        return Result<IntentOrchestrationResult>::Failure(pumped);
    }
    // runUntilIdle may complete at the exact deadline boundary.  Do not
    // replace the business deadline with the later query-control deadline.
    if (deadlineExpired(context.deadline_mono_ns, *clock_)) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_RESULT_AFTER_DEADLINE",
            "intent inference completed after the request deadline"));
    }
    auto query_call = call;
    query_call.deadline_mono_ns =
        clock_->monotonicNowNs() + 1'000'000'000LL;
    const auto snapshot =
        inference_->queryInference(accepted.job_id, query_call);
    if (!snapshot.status.ok || !snapshot.value ||
        snapshot.value->state !=
            inference::InferenceJobState::Completed ||
        !snapshot.value->result) {
        const auto code =
            snapshot.value && snapshot.value->last_error
                ? snapshot.value->last_error->code
                : "INTENT_INFERENCE_FAILED";
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", code, "intent inference did not complete"));
    }
    if (deadlineExpired(context.deadline_mono_ns, *clock_)) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_RESULT_AFTER_DEADLINE",
            "intent inference result arrived after the request deadline"));
    }
    inference::InferenceOutput final_output = *snapshot.value->result;
    try {
        const auto first =
            nlohmann::json::parse(final_output.raw_output);
        if (!first.is_object() || !first.contains("branch") ||
            !first.at("branch").is_string()) {
            throw std::runtime_error(
                "first inference branch is missing");
        }
        const auto branch =
            first.at("branch").get<std::string>();
        if (branch == "NO_QUERY") {
            if (first.size() != 2 || !first.contains("final") ||
                !first.at("final").is_object() ||
                first.contains("queries")) {
                throw std::runtime_error(
                    "NO_QUERY must carry one final decision");
            }
            final_output.raw_output = first.at("final").dump();
            final_output.output_digest =
                inference::inferenceOutputDigest(final_output);
            return parseModelDecision(request, context, final_output,
                                      call);
        }
        if (branch != "QUERY_BATCH" || first.size() != 2 ||
            !first.contains("queries") ||
            !first.at("queries").is_array() ||
            first.at("queries").empty() ||
            first.at("queries").size() > 8 ||
            first.contains("final")) {
            throw std::runtime_error("invalid QUERY_BATCH branch");
        }

        std::set<std::string> query_ids;
        std::vector<QuerySpec> queries;
        for (const auto& query : first.at("queries")) {
            if (!query.is_object() || query.size() != 4 ||
                !query.contains("query_id") ||
                !query.contains("capability") ||
                !query.contains("arguments") ||
                !query.contains("read_only") ||
                !query.at("query_id").is_string() ||
                !query.at("capability").is_string() ||
                !query.at("arguments").is_object() ||
                !query.at("read_only").is_boolean() ||
                !query.at("read_only").get<bool>()) {
                throw std::runtime_error(
                    "query schema or read-only seal invalid");
            }
            const auto query_id =
                query.at("query_id").get<std::string>();
            const auto capability =
                query.at("capability").get<std::string>();
            const auto& arguments = query.at("arguments");
            const bool memory_query =
                capability == "memory.getContext" ||
                capability == "memory.recallMemory";
            const bool preprocess_query =
                capability ==
                    "preprocess.queryRuntimeState" ||
                capability ==
                    "preprocess.getVehicleState";
            const bool skill_query =
                capability ==
                    "skill.getToolDefinitions" ||
                capability == "skill.matchSkill";
            const bool atomic_query =
                capability == "atomic.queryReadOnly";
            bool arguments_valid = false;
            if (capability == "memory.getContext") {
                arguments_valid =
                    arguments.size() == 2 &&
                    arguments.value(
                        "scope", std::string{}) ==
                        "short_term" &&
                    arguments.value(
                        "request_id", std::string{}) ==
                        request.request_id;
            } else if (
                capability == "memory.recallMemory") {
                arguments_valid =
                    arguments.contains("query") &&
                    arguments.at("query").is_string();
            } else if (preprocess_query) {
                arguments_valid =
                    arguments.contains("fields") &&
                    arguments.at("fields").is_array() &&
                    !arguments.at("fields").empty() &&
                    arguments.at("fields").size() <= 32 &&
                    (arguments.value(
                         "state_type",
                         std::string{"VEHICLE"}) ==
                         "VEHICLE" ||
                     arguments.value(
                         "state_type",
                         std::string{"VEHICLE"}) ==
                         "ENVIRONMENT");
            } else if (
                capability ==
                "skill.getToolDefinitions") {
                arguments_valid =
                    arguments.empty() ||
                    (arguments.size() == 1 &&
                     arguments.contains("names") &&
                     arguments.at("names").is_array());
            } else if (
                capability == "skill.matchSkill") {
                arguments_valid =
                    arguments.contains("query") &&
                    arguments.at("query").is_string() &&
                    arguments.value("top_k", 3U) > 0 &&
                    arguments.value("top_k", 3U) <= 16;
            } else if (atomic_query) {
                arguments_valid =
                    arguments.contains("tool_name") &&
                    arguments.at("tool_name").is_string() &&
                    arguments.contains("arguments") &&
                    arguments.at("arguments").is_object();
            }
            if (query_id.empty() || query_id.size() > 64 ||
                !query_ids.insert(query_id).second ||
                (!memory_query && !preprocess_query &&
                 !skill_query && !atomic_query) ||
                !arguments_valid) {
                throw std::runtime_error(
                    "query is outside the read-only whitelist");
            }
            QuerySpec spec;
            spec.query_id = query_id;
            spec.capability = capability;
            spec.arguments = arguments;
            spec.read_only = true;
            spec.deadline_mono_ns =
                context.deadline_mono_ns;
            spec.idempotency_key =
                "intent-query|" + request.request_id +
                "|" + query_id;
            queries.push_back(std::move(spec));
        }
        std::sort(
            queries.begin(), queries.end(),
            [](const auto& left, const auto& right) {
                return left.query_id < right.query_id;
            });

        updateJobStage(
            job_id, IntentJobState::QueryBatch,
            "QUERY_BATCH");

        nlohmann::json evidence;
        evidence["batch_id"] = ids_->next("query-batch");
        evidence["query_batch"] = nlohmann::json::array();
        evidence["results"] = nlohmann::json::array();
        std::vector<std::future<QueryResultRecord>> pending;
        for (const auto& query : queries) {
            evidence["query_batch"].push_back(
                {{"query_id", query.query_id},
                 {"capability", query.capability},
                 {"arguments", query.arguments},
                 {"read_only", true},
                 {"deadline_mono_ns",
                  query.deadline_mono_ns},
                 {"idempotency_key",
                  query.idempotency_key}});
            if (query_executor_) {
                pending.push_back(std::async(
                    std::launch::async,
                    [this, query, &request, &context,
                     &call] {
                        return query_executor_->execute(
                            query, request, context, call);
                    }));
            } else {
                pending.push_back(std::async(
                    std::launch::deferred,
                    [this, query, &context] {
                        QueryResultRecord record;
                        record.query_id = query.query_id;
                        record.capability =
                            query.capability;
                        record.normalized_arguments =
                            query.arguments;
                        if (query.capability !=
                            "memory.getContext") {
                            record.status = "ERROR";
                            record.error_code =
                                "INTENT_QUERY_EXECUTOR_NOT_CONFIGURED";
                            record.source =
                                "IntentRecognitionEngine";
                        } else {
                            record.status =
                                context.memory_context
                                        .flattened_context
                                        .empty()
                                    ? "EMPTY"
                                    : "OK";
                            record.result = {
                                {"flattened_context",
                                 context.memory_context
                                     .flattened_context},
                                {"block_count",
                                 context.memory_context
                                     .blocks.size()}};
                            record.source =
                                "MemoryService";
                            record.source_version =
                                context.context_version;
                        }
                        record.observed_at_utc_ms =
                            clock_->utcNowMs();
                        return record;
                    }));
            }
        }
        std::vector<QueryResultRecord> records;
        records.reserve(pending.size());
        for (auto& future : pending) {
            records.push_back(future.get());
        }
        std::sort(
            records.begin(), records.end(),
            [](const auto& left, const auto& right) {
                return left.query_id < right.query_id;
            });
        for (const auto& record : records) {
            evidence["results"].push_back(
                {{"query_id", record.query_id},
                 {"capability", record.capability},
                 {"normalized_arguments",
                  record.normalized_arguments},
                 {"status", record.status},
                 {"result", record.result},
                 {"source", record.source},
                 {"source_version",
                  record.source_version},
                 {"observed_at_ms",
                  record.observed_at_utc_ms},
                 {"error_code", record.error_code}});
        }
        evidence["frozen"] = true;
        evidence["bundle_digest"] =
            secureDigest(evidence.dump());

        if (deadlineExpired(context.deadline_mono_ns, *clock_)) {
            return Result<IntentOrchestrationResult>::Failure(
                Status::Error(
                    "intent", "INTENT_RESULT_AFTER_DEADLINE",
                    "query evidence completed after the deadline"));
        }
        const auto second_package =
            prompt_->assembleIntentEvidencePrompt(
                context.preprocess_result.normalized_request,
                context.memory_context, evidence, call);
        if (!second_package.status.ok || !second_package.value) {
            return Result<IntentOrchestrationResult>::Failure(
                second_package.status);
        }

        // Phase two receives only the frozen query result and must produce a
        // terminal decision. The protocol explicitly forbids another query.
        updateJobStage(
            job_id, IntentJobState::SecondInference,
            "SECOND_INFERENCE");
        inference::InferenceRequest second_request =
            inference_request;
        second_request.job_id =
            ids_->next("intent-inference-second");
        second_request.parent_operation_id = accepted.job_id;
        second_request.prompt = second_package.value->prompt;
        second_request.prompt_digest =
            second_package.value->prompt_digest;
        second_request.prompt_segments =
            second_package.value->segments;
        second_request.prompt_protocol_version =
            second_package.value->protocol_version;
        second_request.inference_phase = "SECOND_INFERENCE";
        second_request.idempotency_key =
            "intent-inference-second|" + request.request_id +
            "|" + std::to_string(request.turn_id);
        const auto second_accepted =
            inference_->submitInference(second_request, call);
        if (!second_accepted.accepted) {
            return Result<IntentOrchestrationResult>::Failure(
                Status::Error(
                    "intent", second_accepted.reject_code,
                    "inference framework rejected second intent job"));
        }
        const auto second_pumped = inference_->runUntilIdle();
        if (!second_pumped.ok) {
            return Result<IntentOrchestrationResult>::Failure(
                second_pumped);
        }
        if (deadlineExpired(context.deadline_mono_ns, *clock_)) {
            return Result<IntentOrchestrationResult>::Failure(
                Status::Error(
                    "intent", "INTENT_RESULT_AFTER_DEADLINE",
                    "second inference completed after the deadline"));
        }
        auto second_query_call = call;
        second_query_call.deadline_mono_ns =
            clock_->monotonicNowNs() + 1'000'000'000LL;
        const auto second_snapshot = inference_->queryInference(
            second_accepted.job_id, second_query_call);
        if (!second_snapshot.status.ok ||
            !second_snapshot.value ||
            second_snapshot.value->state !=
                inference::InferenceJobState::Completed ||
            !second_snapshot.value->result) {
            const auto code =
                second_snapshot.value &&
                        second_snapshot.value->last_error
                    ? second_snapshot.value->last_error->code
                    : "INTENT_SECOND_INFERENCE_FAILED";
            return Result<IntentOrchestrationResult>::Failure(
                Status::Error(
                    "intent", code,
                    "second intent inference did not complete"));
        }
        const auto second_decoded = nlohmann::json::parse(
            second_snapshot.value->result->raw_output);
        if (!second_decoded.is_object() ||
            second_decoded.contains("branch") ||
            second_decoded.contains("queries") ||
            second_decoded.contains("QUERY_BATCH") ||
            second_decoded.contains("NO_QUERY")) {
            throw std::runtime_error(
                "second inference attempted another query");
        }
        return parseModelDecision(
            request, context,
            *second_snapshot.value->result, call);
    } catch (...) {
        return Result<IntentOrchestrationResult>::Failure(
            Status::Error(
                "intent", "INTENT_FIRST_INFERENCE_PROTOCOL_INVALID",
                "first/second inference protocol validation failed"));
    }
}


}  // namespace master_agent::intent
