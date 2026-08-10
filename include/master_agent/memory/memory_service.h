#pragma once

/**
 * @file memory_service.h
 * @brief Defines governed short-term memory retrieval and turn-commit interfaces.
 */

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "master_agent/common/types.h"
#include "master_agent/interaction/interaction_layer.h"
#include "master_agent/memory/memory_client.h"

namespace master_agent::memory {

struct MemoryContext {
    std::vector<master_agent::memory::ContextBlock> blocks;
    std::string flattened_context;
};

struct CompletedTurn {
    interaction::StandardRequest request;
    std::string normalized_user_input;
    std::string assistant_output;
    std::string scene = "cockpit";
    std::uint32_t record_version = 1;
};

/**
 * @brief Provides bounded recall and durable turn persistence.
 *
 * Reads return a frozen context for the current request. writeTurn is idempotent
 * by session, turn, and record version; uncertain storage outcomes must preserve
 * SideEffectState::Unknown rather than claim that no write occurred.
 */
class IMemoryService {
public:
    virtual ~IMemoryService() = default;

    /**
     * @brief Returns bounded context tied to the current request identity.
     *
     * The returned snapshot is validated and frozen before it enters a prompt.
     */
    virtual Result<MemoryContext> getContext(
        const interaction::StandardRequest& request,
        const std::string& normalized_query,
        const CallContext& call) = 0;

    /**
     * @brief Persists one versioned conversation turn.
     *
     * Replaying identical content is idempotent. Conflicting content for the
     * same session, turn, and record_version is rejected.
     */
    virtual Status writeTurn(const CompletedTurn& turn,
                             const CallContext& call) = 0;
};

class MemoryService final : public IMemoryService {
public:
    explicit MemoryService(
        std::shared_ptr<master_agent::memory::IMemoryClient> client,
        std::shared_ptr<IRuntimeClock> clock);

    Result<MemoryContext> getContext(
        const interaction::StandardRequest& request,
        const std::string& normalized_query,
        const CallContext& call) override;

    Status writeTurn(const CompletedTurn& turn,
                     const CallContext& call) override;

private:
    std::shared_ptr<master_agent::memory::IMemoryClient> client_;
    std::shared_ptr<IRuntimeClock> clock_;
};

std::shared_ptr<master_agent::memory::IMemoryClient>
createJournalMemoryClient(const std::filesystem::path& data_directory);

}  // namespace master_agent::memory
