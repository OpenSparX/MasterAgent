#pragma once

/**
 * @file preprocess_engine_impl.h
 * @brief Private composition contract for the preprocessing engine.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "preprocess_observer.h"
#include "state_query_service.h"
#include "text_pipeline.h"

namespace master_agent::preprocess {

/**
 * @brief PreprocessEngineImpl orchestration component.
 */
class PreprocessEngineImpl {
public:
    explicit PreprocessEngineImpl(PreprocessDependencies dependencies);

    Result<PreprocessResult> process(
        const interaction::StandardRequest& request,
        const CallContext& call) const;

    Result<StateQueryResult> queryRuntimeState(
        const StateQuery& query,
        const CallContext& call) const;

    Result<std::vector<StateCapability>> getCapabilities(
        const CallContext& call) const;

private:
    std::shared_ptr<IRuntimeClock> clock_;
    detail::Cleaner cleaner_;
    detail::Normalizer normalizer_;
    detail::TimeAligner time_aligner_;
    std::shared_ptr<detail::StateQueryServiceImpl>
        state_query_service_;
    std::shared_ptr<detail::PreprocessObserver> observer_;
};

}  // namespace master_agent::preprocess

