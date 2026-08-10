/**
 * @file vehicle_state_collector.cpp
 * @brief Implements one-Provider-per-domain registration and state routing.
 *
 * Design mapping: Sections 2.2, 2.4, 4 and 5.
 */

#include "include/state_provider_registry.h"

#include <exception>
#include <set>
#include <utility>

namespace master_agent::preprocess::detail {
namespace {

/**
 * Reads and validates Provider metadata at the trust boundary.
 *
 * Provider-specific errors and exceptions are deliberately translated to
 * stable preprocessing errors. This prevents implementation details from
 * crossing the module boundary and keeps all callers on one error contract.
 */
Result<StateCapability> readCapability(
    const std::shared_ptr<IRuntimeStateProvider>& provider) {
    if (!provider) {
        return Result<StateCapability>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_NOT_CONFIGURED",
                "state Provider is not configured"));
    }

    Result<StateCapability> capability;
    try {
        capability = provider->getCapability();
    } catch (const std::exception&) {
        return Result<StateCapability>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_EXCEPTION",
                "state Provider raised an exception"));
    } catch (...) {
        return Result<StateCapability>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_EXCEPTION",
                "state Provider raised a non-standard exception"));
    }
    if (!capability.status.ok) {
        return Result<StateCapability>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_FAILED",
                "state Provider capability lookup failed"));
    }
    if (!capability.value ||
        !validCapability(*capability.value)) {
        return Result<StateCapability>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider returned invalid capability metadata"));
    }
    return capability;
}

}  // namespace

VehicleStateCollector::VehicleStateCollector(
    std::vector<std::shared_ptr<IRuntimeStateProvider>>
        providers,
    std::shared_ptr<IRuntimeClock> clock)
    : clock_(std::move(clock)) {
    // Provider registration is completed before the collector is published.
    // Duplicate domains are configuration errors, never an ordering rule.
    for (auto& provider : providers) {
        auto capability = readCapability(provider);
        if (!capability.status.ok || !capability.value) {
            initialization_status_ = capability.status.ok
                ? providerFailure(
                      "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                      "state Provider capability is missing")
                : capability.status;
            providers_.clear();
            return;
        }

        const auto [unused, inserted] = providers_.emplace(
            capability.value->state_type,
            std::move(provider));
        (void)unused;
        if (!inserted) {
            initialization_status_ = providerFailure(
                "PREPROCESS_STATE_PROVIDER_DOMAIN_CONFLICT",
                "only one state Provider may be configured per domain");
            providers_.clear();
            return;
        }
    }
    if (providers_.empty()) {
        initialization_status_ = providerFailure(
            "PREPROCESS_STATE_PROVIDER_NOT_CONFIGURED",
            "no state Provider capabilities are configured");
    }
}

Result<std::vector<StateCapability>>
VehicleStateCollector::getCapabilities() const {
    if (!initialization_status_.ok) {
        return Result<
            std::vector<StateCapability>>::Failure(
            initialization_status_);
    }

    std::vector<StateCapability> capabilities;
    capabilities.reserve(providers_.size());
    for (const auto& [domain, provider] : providers_) {
        auto capability = readCapability(provider);
        if (!capability.status.ok || !capability.value) {
            return Result<
                std::vector<StateCapability>>::Failure(
                capability.status.ok
                    ? providerFailure(
                          "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                          "state Provider capability is missing")
                    : capability.status);
        }
        // A Provider may not change its domain after registration. Doing so
        // would invalidate the ownership index and is treated as a protocol
        // violation rather than being routed dynamically.
        if (capability.value->state_type != domain) {
            return Result<
                std::vector<StateCapability>>::Failure(
                providerFailure(
                    "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                    "state Provider changed its registered domain"));
        }
        capabilities.push_back(*capability.value);
    }
    return Result<std::vector<StateCapability>>::Success(
        std::move(capabilities));
}

Result<StateQueryResult> VehicleStateCollector::query(
    const StateQuery& query) const {
    const auto now = [this]() {
        return clock_ ? clock_->utcNowMs() : 0;
    };
    if (!initialization_status_.ok) {
        return Result<StateQueryResult>::Failure(
            initialization_status_);
    }

    const auto selected = providers_.find(query.state_type);
    if (selected == providers_.end()) {
        StateQueryResult unavailable;
        unavailable.timestamp_utc_ms = now();
        unavailable.error_message =
            "state Provider is unavailable";
        return Result<StateQueryResult>::Success(
            std::move(unavailable));
    }

    const auto& provider = selected->second;
    auto capability = readCapability(provider);
    if (!capability.status.ok || !capability.value) {
        return Result<StateQueryResult>::Failure(
            capability.status.ok
                ? providerFailure(
                      "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                      "state Provider capability is missing")
                : capability.status);
    }
    if (capability.value->state_type != query.state_type) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider changed its registered domain"));
    }

    const std::set<std::string> declared_fields(
        capability.value->fields.begin(),
        capability.value->fields.end());
    StateQuery provider_query = query;
    provider_query.fields.clear();
    std::vector<std::string> undeclared_fields;
    for (const auto& field : query.fields) {
        if (declared_fields.count(field) != 0) {
            provider_query.fields.push_back(field);
        } else {
            undeclared_fields.push_back(field);
        }
    }
    if (provider_query.fields.empty()) {
        StateQueryResult unavailable;
        unavailable.timestamp_utc_ms = now();
        unavailable.missing_fields =
            std::move(undeclared_fields);
        unavailable.error_message =
            "one or more requested fields are unavailable";
        return Result<StateQueryResult>::Success(
            std::move(unavailable));
    }

    Result<StateQueryResult> provider_result;
    try {
        provider_result = provider->query(provider_query);
    } catch (const std::exception&) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_EXCEPTION",
                "state Provider raised an exception"));
    } catch (...) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_EXCEPTION",
                "state Provider raised a non-standard exception"));
    }
    if (!provider_result.status.ok) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_FAILED",
                "state Provider query failed"));
    }
    if (!provider_result.value) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider returned success without a value"));
    }

    auto sealed = sealProviderQueryResult(
        provider_query, *provider_result.value,
        now());
    if (!sealed.status.ok || !sealed.value) {
        return sealed.status.ok
                   ? Result<StateQueryResult>::Failure(
                         providerFailure(
                             "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                             "state result seal returned no value"))
                   : sealed;
    }
    if (!undeclared_fields.empty()) {
        sealed.value->success = false;
        sealed.value->missing_fields.insert(
            sealed.value->missing_fields.end(),
            undeclared_fields.begin(),
            undeclared_fields.end());
        sealed.value->error_message =
            "one or more requested fields are unavailable";
    }
    return sealed;
}

}  // namespace master_agent::preprocess::detail
