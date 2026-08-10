#pragma once

/**
 * @file preprocess_observer.h
 * @brief Private bounded observability bridge for preprocessing.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "master_agent/preprocess/preprocess_engine.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace master_agent::data_log {
class IDataLogService;
}

namespace master_agent::exception {
class IExceptionManager;
}

namespace master_agent::preprocess::detail {

/**
 * @brief Best-effort bridge to the injected DataLog and Exception modules.
 *
 * Observability never changes a successful preprocessing result. It emits only
 * bounded metadata and never includes user text or parameter values.
 */
class PreprocessObserver {
public:
    PreprocessObserver(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<data_log::IDataLogService> log_service,
        std::shared_ptr<exception::IExceptionManager> exception_manager);

    void record(
        const CallContext& call,
        const std::string& session_id,
        const std::string& operation,
        const std::string& outcome,
        const std::string& error_code = {}) const noexcept;

private:
    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<data_log::IDataLogService> log_service_;
    std::shared_ptr<exception::IExceptionManager> exception_manager_;
    mutable std::atomic<std::uint64_t> log_sequence_{1};
    mutable std::atomic<std::uint64_t> exception_sequence_{1};
};

}  // namespace master_agent::preprocess::detail

