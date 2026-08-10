/**
 * @file intent_prompt_adapter.cpp
 * @brief Adds the bounded model protocol around the standalone Prompt engine.
 */

#include "master_agent/intent/intent_support.h"

#include <sstream>
#include <utility>

namespace master_agent::intent_support {
namespace {

nlohmann::json modelVisibleTool(
    const atomic_service::McpToolDefinition& tool) {
    return {
        {"name", tool.name},
        {"title", tool.title},
        {"description", tool.description},
        {"inputSchema", tool.input_schema},
        {"outputSchema", tool.output_schema},
        {"annotations",
         {{"title", tool.annotations.title},
          {"readOnlyHint", tool.annotations.read_only_hint},
          {"destructiveHint", tool.annotations.destructive_hint},
          {"idempotentHint", tool.annotations.idempotent_hint},
          {"openWorldHint", tool.annotations.open_world_hint}}}};
}

std::string skillSummaries(
    const std::shared_ptr<skill::ISkillEngine>& skill_library) {
    nlohmann::json summaries = nlohmann::json::array();
    if (!skill_library) {
        return summaries.dump();
    }
    for (const auto& encoded : skill_library->getAllSkillSummaries()) {
        const auto parsed =
            nlohmann::json::parse(encoded, nullptr, false);
        summaries.push_back(parsed.is_discarded()
                                ? nlohmann::json(encoded)
                                : parsed);
    }
    return summaries.dump();
}

class IntentPromptAssembler final : public IIntentPromptAssembler {
public:
    IntentPromptAssembler(
        std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
        std::shared_ptr<prompt::IPromptEngine> prompt_engine,
        std::shared_ptr<skill::ISkillEngine> skill_library)
        : atomic_(std::move(atomic)),
          prompt_engine_(std::move(prompt_engine)),
          skill_library_(std::move(skill_library)) {}

    Result<PromptPackage> assembleIntentPrompt(
        const interaction::StandardRequest& request,
        const memory::MemoryContext& memory_context,
        const CallContext& call) const override {
        if (!hasHostModuleIdentity(
                call, CallerModuleId::IntentRecognitionEngine)) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_CALLER_NOT_ALLOWED",
                "only IntentRecognitionEngine may assemble intent prompt"));
        }
        if (!atomic_ || !prompt_engine_) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_NOT_READY",
                "Prompt or atomic service is not configured"));
        }
        if (request.request_id.empty() ||
            call.request_id != request.request_id ||
            call.trace_id != request.trace_id ||
            call.priority != request.priority ||
            call.deadline_mono_ns != request.deadline_mono_ns ||
            call.deadline_mono_ns <= 0) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_CALL_IDENTITY_INVALID",
                "prompt assembly must bind the active request"));
        }

        auto atomic_call =
            makeChildCallContext(call, CallerModuleId::PromptEngine);
        const auto listed = atomic_->listTools(atomic_call);
        if (!listed.status.ok || !listed.value) {
            return Result<PromptPackage>::Failure(listed.status);
        }
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& tool : *listed.value) {
            tools.push_back(modelVisibleTool(tool));
        }

        prompt::PromptContext context;
        context.tpl_type = "default";
        context.context = request.text;
        context.memory_context =
            memory_context.flattened_context;
        context.skill_candidates = skillSummaries(skill_library_);
        const std::string rendered_system =
            prompt_engine_->buildPrompt(context);
        if (rendered_system.empty()) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_TEMPLATE_UNAVAILABLE",
                "Prompt engine did not produce a rendered template"));
        }

        const std::string memory_text =
            memory_context.flattened_context.empty()
                ? u8"无短期上下文"
                : memory_context.flattened_context;
        std::ostringstream assembled;
        assembled
            << "SYSTEM:\n" << rendered_system
            << "\nSKILL_CANDIDATES:\n" << context.skill_candidates
            << "\nTOOLS:\n" << tools.dump()
            << "\nMEMORY:\n" << memory_text
            << "\nUSER:\n" << request.text
            << "\nPROTOCOL:\n"
            << "FIRST_INFERENCE must return exactly one JSON branch: "
               "QUERY_BATCH with a complete read-only query batch, or "
               "NO_QUERY with a complete final PLAN/ASK/REPLY/FAIL decision.";

        const std::string tools_text = tools.dump();
        PromptPackage package;
        package.prompt = assembled.str();
        package.prompt_digest = secureDigest(package.prompt);
        package.segments = {
            {"system", secureDigest(rendered_system),
             static_cast<std::uint32_t>(
                 (rendered_system.size() + 3U) / 4U)},
            {"skills", secureDigest(context.skill_candidates),
             static_cast<std::uint32_t>(
                 (context.skill_candidates.size() + 3U) / 4U)},
            {"tools", secureDigest(tools_text),
             static_cast<std::uint32_t>(
                 (tools_text.size() + 3U) / 4U)},
            {"memory", secureDigest(memory_text),
             static_cast<std::uint32_t>(
                 (memory_text.size() + 3U) / 4U)},
            {"user", secureDigest(request.text),
             static_cast<std::uint32_t>(
                 (request.text.size() + 3U) / 4U)}};
        return Result<PromptPackage>::Success(std::move(package));
    }

    Result<PromptPackage> assembleIntentEvidencePrompt(
        const interaction::StandardRequest& request,
        const memory::MemoryContext& memory_context,
        const nlohmann::json& evidence_bundle,
        const CallContext& call) const override {
        if (!evidence_bundle.is_object() ||
            !evidence_bundle.contains("query_batch") ||
            !evidence_bundle.contains("results") ||
            !evidence_bundle.at("query_batch").is_array() ||
            !evidence_bundle.at("results").is_array()) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_EVIDENCE_INVALID",
                "second inference requires a complete EvidenceBundle"));
        }
        const auto encoded_evidence = evidence_bundle.dump();
        if (encoded_evidence.size() > 64U * 1024U) {
            return Result<PromptPackage>::Failure(Status::Error(
                "intent_support", "PROMPT_EVIDENCE_TOO_LARGE",
                "EvidenceBundle exceeds the runtime prompt limit"));
        }
        auto base =
            assembleIntentPrompt(request, memory_context, call);
        if (!base.status.ok || !base.value) {
            return base;
        }

        PromptPackage package = std::move(*base.value);
        package.prompt +=
            "\nEVIDENCE_BUNDLE:\n" + encoded_evidence +
            "\nPROTOCOL:\nSECOND_INFERENCE_FINAL_ONLY. Return one final "
            "PLAN/ASK/REPLY/FAIL JSON decision. QUERY_BATCH and NO_QUERY are "
            "forbidden.";
        package.prompt_digest = secureDigest(package.prompt);
        package.protocol_version = "prompt-v2-evidence-final";
        package.segments.push_back(
            {"evidence-bundle", secureDigest(encoded_evidence),
             static_cast<std::uint32_t>(
                 (encoded_evidence.size() + 3U) / 4U)});
        return Result<PromptPackage>::Success(std::move(package));
    }

private:
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic_;
    std::shared_ptr<prompt::IPromptEngine> prompt_engine_;
    std::shared_ptr<skill::ISkillEngine> skill_library_;
};

}  // namespace

std::shared_ptr<IIntentPromptAssembler> createIntentPromptAssembler(
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<prompt::IPromptEngine> prompt_engine,
    std::shared_ptr<skill::ISkillEngine> skill_library) {
    return std::make_shared<IntentPromptAssembler>(
        std::move(atomic),
        std::move(prompt_engine),
        std::move(skill_library));
}

}  // namespace master_agent::intent_support
