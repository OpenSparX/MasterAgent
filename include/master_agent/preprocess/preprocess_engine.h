#pragma once

/**
 * @file preprocess_engine.h
 * @brief Defines request preprocessing and on-demand state-query contracts.
 *
 * Design traceability:
 * - Preprocessing design Sections 1.1, 2.4 and 3.
 * - process() never reads live vehicle or environment state.
 * - IStateQuery exposes capability discovery separately from live reads.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "master_agent/common/types.h"
#include "master_agent/interaction/interaction_layer.h"

namespace master_agent::data_log {
class IDataLogService;
}

namespace master_agent::exception {
class IExceptionManager;
}

namespace master_agent::preprocess {

/**
 * @brief State domains defined by preprocessing design Section 3.3.
 *
 * A closed enum prevents an unvalidated string from selecting a Provider.
 * New domains require an explicit API and Provider-contract revision.
 */
enum class StateDomain : std::uint8_t {
    Vehicle,
    Environment
};

std::string toString(StateDomain domain);

struct PreprocessResult {
    interaction::StandardRequest normalized_request;
    std::map<std::string, std::string> event_schema;
    bool valid = false;
    std::string error_message;
};

struct StateCapability {
    StateDomain state_type = StateDomain::Vehicle;
    std::vector<std::string> fields;
};

struct StateQuery {
    std::string request_id;
    std::string session_id;
    std::uint64_t turn_id = 0;
    StateDomain state_type = StateDomain::Vehicle;
    std::vector<std::string> fields;
};

struct StateQueryResult {
    bool success = false;
    std::map<std::string, std::string> values;
    std::vector<std::string> missing_fields;
    /// Provider observation timestamp in Unix milliseconds (Section 3.3).
    std::int64_t timestamp_utc_ms = 0;
    std::string error_message;
};

/**
 * @brief Supplies allowlisted runtime state without changing system state.
 *
 * Providers expose capabilities separately from queries so callers can validate
 * fields before performing external I/O. Exactly one Provider may own a given
 * StateDomain. Duplicate domain registration is rejected by the state-query
 * subsystem; it is never resolved by registration order or field merging.
 *
 * A Provider is an injected adapter, not an independent Master Agent module,
 * and therefore does not introduce another CallerModuleId.
 */
class IRuntimeStateProvider {
public:
    virtual ~IRuntimeStateProvider() = default;

    /// Returns capability metadata only; it must not sample live state.
    virtual Result<StateCapability> getCapability() const = 0;

    /// Reads only the fields named by the validated query.
    virtual Result<StateQueryResult> query(
        const StateQuery& request) const = 0;
};

/**
 * @brief Section 3.1 request-preprocessing interface.
 *
 * This interface is intentionally independent from IStateQuery. A normal
 * process() call must not perform Provider I/O.
 */
class IPreprocess {
public:
    virtual ~IPreprocess() = default;

    /// Normalizes input without performing external state queries.
    virtual Result<PreprocessResult> process(
        const interaction::StandardRequest& request,
        const CallContext& call) const = 0;
};

/**
 * @brief Section 3.1 on-demand state-query interface.
 *
 * getCapabilities() returns metadata only. queryRuntimeState() reads exactly
 * the validated fields requested by Agent Service.
 */
class IStateQuery {
public:
    virtual ~IStateQuery() = default;

    /// Executes one allowlisted, read-only query against a matching provider.
    virtual Result<StateQueryResult> queryRuntimeState(
        const StateQuery& query, const CallContext& call) const = 0;

    virtual Result<std::vector<StateCapability>> getCapabilities(
        const CallContext& call) const = 0;
};

/**
 * @brief Compatibility aggregate for callers that inject one object.
 *
 * The two design contracts remain independently mockable through IPreprocess and
 * IStateQuery, while existing Agent Service construction can keep one shared
 * dependency.
 */
class IPreprocessEngine : public IPreprocess, public IStateQuery {
public:
    ~IPreprocessEngine() override = default;
};

struct PreprocessDependencies {
    std::shared_ptr<IRuntimeClock> clock;
    std::shared_ptr<IdGenerator> ids;
    /// Each Provider must declare a unique StateDomain.
    std::vector<std::shared_ptr<IRuntimeStateProvider>> providers;
    std::shared_ptr<data_log::IDataLogService> log_service;
    std::shared_ptr<exception::IExceptionManager> exception_manager;
};

class PreprocessEngineImpl;

class PreprocessEngine final : public IPreprocessEngine {
public:
    explicit PreprocessEngine(
        std::shared_ptr<IRuntimeClock> clock);

    /// Provider-injection overload used by integration and protocol tests.
    PreprocessEngine(
        std::shared_ptr<IRuntimeClock> clock,
        std::vector<std::shared_ptr<IRuntimeStateProvider>> providers);

    /// Complete dependency-injection constructor used by the runtime.
    explicit PreprocessEngine(PreprocessDependencies dependencies);

    Result<PreprocessResult> process(
        const interaction::StandardRequest& request,
        const CallContext& call) const override;

    Result<StateQueryResult> queryRuntimeState(
        const StateQuery& query, const CallContext& call) const override;

    Result<std::vector<StateCapability>> getCapabilities(
        const CallContext& call) const override;

private:
    std::shared_ptr<PreprocessEngineImpl> impl_;
};

}  // namespace master_agent::preprocess
