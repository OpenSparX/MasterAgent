#pragma once

/**
 * @file intent_support.h
 * @brief Defines adapters between Intent and the standalone Prompt/Skill engines.
 *
 * Prompt and Skill keep the interfaces supplied by their module owners. The
 * Intent module owns the additional deterministic-slot and model-protocol
 * contracts because those responsibilities are not part of either standalone
 * engine.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/interaction/interaction_layer.h"
#include "master_agent/kv_cache/kv_cache_manager.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/prompt/prompt_engine.h"
#include "master_agent/skill/skill_engine.h"

namespace master_agent::intent_support {

enum class DeterministicResolutionState : std::uint8_t {
    Resolved,
    Clarify,
    NoMatch,
    Failed
};

struct DeterministicResolution {
    DeterministicResolutionState state =
        DeterministicResolutionState::NoMatch;
    std::string tool_name;
    nlohmann::json arguments = nlohmann::json::object();
    std::string user_reply;
    std::string reason_code;
};

struct PromptPackage {
    std::string prompt;
    std::string prompt_digest;
    std::vector<kv_cache::PromptSegment> segments;
    std::string protocol_version = "prompt-v2";
};

class IIntentSkillResolver {
public:
    virtual ~IIntentSkillResolver() = default;

    virtual Result<std::vector<atomic_service::McpToolDefinition>>
    listAvailableTools(const CallContext& call) const = 0;

    virtual DeterministicResolution resolveDeterministic(
        const std::string& normalized_text,
        const CallContext& call) const = 0;

    /**
     * Returns model-facing Skill candidates without converting a fuzzy match
     * into an executable action. This is the Skill branch of QUERY_BATCH.
     */
    virtual Result<nlohmann::json> querySkillCandidates(
        const std::string& normalized_text,
        std::size_t top_k,
        const CallContext& call) const {
        (void)normalized_text;
        (void)top_k;
        (void)call;
        return Result<nlohmann::json>::Failure(
            Status::Error(
                "intent_support",
                "SKILL_QUERY_NOT_SUPPORTED",
                "Skill candidate query is not implemented"));
    }
};

class IIntentPromptAssembler {
public:
    virtual ~IIntentPromptAssembler() = default;

    virtual Result<PromptPackage> assembleIntentPrompt(
        const interaction::StandardRequest& request,
        const memory::MemoryContext& memory_context,
        const CallContext& call) const = 0;

    virtual Result<PromptPackage> assembleIntentEvidencePrompt(
        const interaction::StandardRequest&,
        const memory::MemoryContext&,
        const nlohmann::json&,
        const CallContext&) const {
        return Result<PromptPackage>::Failure(Status::Error(
            "intent_support", "PROMPT_EVIDENCE_NOT_SUPPORTED",
            "evidence prompt assembly is not implemented"));
    }
};

std::shared_ptr<IIntentSkillResolver> createIntentSkillResolver(
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<skill::ISkillEngine> skill_library);

std::shared_ptr<IIntentPromptAssembler> createIntentPromptAssembler(
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<prompt::IPromptEngine> prompt_engine,
    std::shared_ptr<skill::ISkillEngine> skill_library);

}  // namespace master_agent::intent_support
