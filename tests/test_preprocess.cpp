/**
 * @file test_preprocess.cpp
 * @brief Verifies the preprocessing public contracts and component flow.
 */

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "master_agent/preprocess/preprocess_engine.h"
#include "test_support.h"

namespace {

using master_agent::CallContext;
using master_agent::CallerModuleId;
using master_agent::IdGenerator;
using master_agent::ManualRuntimeClock;
using master_agent::Result;
using master_agent::Status;
using master_agent::TaskPriority;
using master_agent::test_support::expect;
namespace data_log = master_agent::data_log;
namespace exception = master_agent::exception;
namespace interaction = master_agent::interaction;
namespace preprocess = master_agent::preprocess;

class CapturingLog final : public data_log::IDataLogService {
public:
    Result<data_log::LogAppendResult> appendEvents(
        const data_log::LogEventBatch& batch,
        const CallContext& call) override {
        expect(
            call.caller ==
                CallerModuleId::PreprocessingEngine,
            "preprocess must identify itself to DataLog");
        batches.push_back(batch);
        data_log::LogAppendResult result;
        result.disposition =
            data_log::AppendDisposition::Accepted;
        result.batch_id = batch.batch_id;
        result.accepted_count =
            static_cast<std::uint32_t>(
                batch.records.size());
        result.achieved_durability =
            data_log::DurabilityClass::D1Buffered;
        return Result<data_log::LogAppendResult>::
            Success(std::move(result));
    }

    Result<data_log::AuditAppendResult> appendAudit(
        const data_log::AuditBatch&,
        const CallContext&) override {
        return Result<data_log::AuditAppendResult>::
            Failure(Status::Error(
                "test", "NOT_USED",
                "audit is not used"));
    }

    Result<data_log::TracePage> queryTrace(
        const data_log::TraceQuery&,
        const CallContext&) const override {
        return Result<data_log::TracePage>::
            Failure(Status::Error(
                "test", "NOT_USED",
                "trace query is not used"));
    }

    Status flush(const CallContext&) override {
        return Status::Ok();
    }

    data_log::LogHealth getHealth(
        const CallContext&) const override {
        return {};
    }

    std::vector<data_log::LogEventBatch> batches;
};

class CapturingExceptions final
    : public exception::IExceptionManager {
public:
    Result<exception::ExceptionReportResult> report(
        const exception::ExceptionReportRequest& request,
        const CallContext& call) override {
        expect(
            call.caller ==
                CallerModuleId::PreprocessingEngine,
            "preprocess must identify itself to Exception");
        reports.push_back(request);
        exception::ExceptionReportResult result;
        result.report_id = request.report_id;
        result.accepted_count =
            static_cast<std::uint32_t>(
                request.occurrences.size());
        return Result<exception::ExceptionReportResult>::
            Success(std::move(result));
    }

    Result<exception::ExceptionGroup> getException(
        const std::string&,
        const CallContext&) const override {
        return Result<exception::ExceptionGroup>::
            Failure(Status::Error(
                "test", "NOT_USED",
                "exception query is not used"));
    }

    Result<exception::ExceptionMutationResult>
    acknowledge(
        const exception::ExceptionMutationRequest&,
        const CallContext&) override {
        return mutationNotUsed();
    }

    Result<exception::ExceptionMutationResult>
    markMitigating(
        const exception::ExceptionMutationRequest&,
        const CallContext&) override {
        return mutationNotUsed();
    }

    Result<exception::ExceptionMutationResult> resolve(
        const exception::ExceptionMutationRequest&,
        const CallContext&) override {
        return mutationNotUsed();
    }

    std::vector<exception::ExceptionReportRequest>
        reports;

private:
    static Result<exception::ExceptionMutationResult>
    mutationNotUsed() {
        return Result<
            exception::ExceptionMutationResult>::Failure(
            Status::Error(
                "test", "NOT_USED",
                "exception mutation is not used"));
    }
};

class CountingProvider final
    : public preprocess::IRuntimeStateProvider {
public:
    CountingProvider(
        std::shared_ptr<ManualRuntimeClock> clock,
        preprocess::StateDomain domain,
        std::map<std::string, std::string> values)
        : clock_(std::move(clock)),
          domain_(domain),
          values_(std::move(values)) {}

    Result<preprocess::StateCapability>
    getCapability() const override {
        ++capability_calls;
        preprocess::StateCapability capability;
        capability.state_type = domain_;
        for (const auto& [name, unused] : values_) {
            (void)unused;
            capability.fields.push_back(name);
        }
        return Result<preprocess::StateCapability>::
            Success(std::move(capability));
    }

    Result<preprocess::StateQueryResult> query(
        const preprocess::StateQuery& request)
        const override {
        ++query_calls;
        preprocess::StateQueryResult result;
        result.timestamp_utc_ms =
            clock_->utcNowMs();
        for (const auto& field : request.fields) {
            const auto found = values_.find(field);
            if (found == values_.end()) {
                result.missing_fields.push_back(field);
            } else {
                result.values.emplace(*found);
            }
        }
        result.success =
            result.missing_fields.empty();
        if (!result.success) {
            result.error_message =
                "one or more requested fields are unavailable";
        }
        return Result<preprocess::StateQueryResult>::
            Success(std::move(result));
    }

    mutable std::size_t capability_calls = 0;
    mutable std::size_t query_calls = 0;

private:
    std::shared_ptr<ManualRuntimeClock> clock_;
    preprocess::StateDomain domain_;
    std::map<std::string, std::string> values_;
};

interaction::StandardRequest requestFor(
    const std::shared_ptr<ManualRuntimeClock>& clock) {
    interaction::StandardRequest request;
    request.request_id = "preprocess-request";
    request.trace_id = "preprocess-trace";
    request.trigger_type = "TEXT_INPUT";
    request.text = "hello";
    request.timestamp_utc_ms = clock->utcNowMs();
    request.deadline_mono_ns =
        clock->monotonicNowNs() +
        10'000'000'000LL;
    request.user_id = "driver-preprocess";
    request.session_id = "session-preprocess";
    request.turn_id = 1;
    request.priority = TaskPriority::P1;
    return request;
}

CallContext callFor(
    const interaction::StandardRequest& request) {
    return {
        CallerModuleId::AgentService,
        request.request_id,
        request.trace_id,
        "principal-preprocess",
        request.priority,
        request.deadline_mono_ns};
}

void testProcessExampleAndObservability() {
    auto clock =
        std::make_shared<ManualRuntimeClock>();
    auto log = std::make_shared<CapturingLog>();
    auto exceptions =
        std::make_shared<CapturingExceptions>();
    preprocess::PreprocessDependencies dependencies;
    dependencies.clock = clock;
    dependencies.ids =
        std::make_shared<IdGenerator>(
            "preprocess-test");
    dependencies.log_service = log;
    dependencies.exception_manager = exceptions;
    preprocess::PreprocessEngine engine(
        std::move(dependencies));

    preprocess::IPreprocess* process_contract =
        &engine;
    preprocess::IStateQuery* query_contract =
        &engine;
    expect(
        process_contract != nullptr &&
            query_contract != nullptr,
        "process and state-query contracts must "
        "be independently addressable");

    auto request = requestFor(clock);
    request.text = u8"  打开\t\n空调  ";
    request.params = {
        {"TargetDevice", "ac"},
        {"Source Page", "vehicle_hmi"},
        {"request-id", "must-not-override"},
        {"empty", ""}};
    request.timestamp_utc_ms =
        clock->utcNowMs() - 6'000;

    const auto result =
        process_contract->process(
            request, callFor(request));
    expect(
        result.status.ok && result.value &&
            result.value->valid,
        "the preprocessing design example must succeed");
    expect(
        result.value->normalized_request.text ==
            u8"打开 空调",
        "controls and repeated whitespace must "
        "collapse to one space");
    expect(
        result.value->normalized_request.params ==
            std::map<std::string, std::string>{
                {"source_page", "vehicle_hmi"},
                {"target_device", "ac"}},
        "parameter names must normalize and reserved "
        "fields must be removed");
    expect(
        result.value->event_schema.at("request_id") ==
                request.request_id &&
            result.value->event_schema.at("is_fresh") ==
                "false" &&
            result.value->normalized_request
                    .timestamp_utc_ms ==
                clock->utcNowMs() - 1'000,
        "identity, aligned time and freshness must "
        "follow the preprocessing design");
    expect(
        log->batches.size() == 1 &&
            exceptions->reports.empty(),
        "successful preprocessing must emit one "
        "metadata-only event");
    expect(
        log->batches.front()
                .records.front()
                .payload_summary_json
                .find(u8"打开") ==
            std::string::npos,
        "observability must not contain user text");

    auto invalid = requestFor(clock);
    invalid.text =
        std::string("\xF0\x28\x8C\x28", 4);
    const auto rejected =
        process_contract->process(
            invalid, callFor(invalid));
    expect(
        rejected.status.ok && rejected.value &&
            !rejected.value->valid &&
            !exceptions->reports.empty() &&
            exceptions->reports.back()
                    .occurrences.front()
                    .code ==
                "PREPROCESS_TEXT_UTF8_INVALID",
        "known invalid UTF-8 must return valid=false "
        "and report a bounded exception");
}

void testUtf8TruncationAndEventInput() {
    auto clock =
        std::make_shared<ManualRuntimeClock>();
    preprocess::PreprocessEngine engine(clock);

    auto request = requestFor(clock);
    request.text =
        std::string(2047, 'a') + u8"车";
    const auto truncated =
        engine.process(request, callFor(request));
    expect(
        truncated.status.ok && truncated.value &&
            truncated.value->valid &&
            truncated.value->normalized_request
                    .text.size() ==
                2047,
        "text truncation must not split a UTF-8 scalar");

    auto event = requestFor(clock);
    event.trigger_type = "PERCEPTION_EVENT";
    event.text.clear();
    event.params = {{"DoorState", "open"}};
    const auto event_result =
        engine.process(event, callFor(event));
    expect(
        event_result.status.ok &&
            event_result.value &&
            event_result.value->valid &&
            event_result.value->event_schema.at(
                "trigger_type") ==
                "perception_event",
        "event input may omit text when it carries "
        "a valid parameter");
}

void testStateDomainUniquenessIsFailClosed() {
    auto clock =
        std::make_shared<ManualRuntimeClock>();
    auto first = std::make_shared<CountingProvider>(
        clock, preprocess::StateDomain::Vehicle,
        std::map<std::string, std::string>{
            {"speed_kmh", "42"}});
    auto second = std::make_shared<CountingProvider>(
        clock, preprocess::StateDomain::Vehicle,
        std::map<std::string, std::string>{
            {"battery_soc", "80"}});
    preprocess::PreprocessEngine engine(
        clock, {first, second});
    auto request = requestFor(clock);
    const auto call = callFor(request);

    const auto capabilities =
        engine.getCapabilities(call);
    expect(
        !capabilities.status.ok &&
            capabilities.status.error.code ==
                "PREPROCESS_STATE_PROVIDER_DOMAIN_CONFLICT" &&
            first->query_calls == 0 &&
            second->query_calls == 0,
        "duplicate StateDomain registration must "
        "fail capability discovery without live reads");

    preprocess::StateQuery speed;
    speed.request_id = request.request_id;
    speed.session_id = request.session_id;
    speed.turn_id = request.turn_id;
    speed.state_type =
        preprocess::StateDomain::Vehicle;
    speed.fields = {"speed_kmh"};
    const auto speed_result =
        engine.queryRuntimeState(speed, call);
    expect(
        !speed_result.status.ok &&
            speed_result.status.error.code ==
                "PREPROCESS_STATE_PROVIDER_DOMAIN_CONFLICT" &&
            first->query_calls == 0 &&
            second->query_calls == 0,
        "duplicate StateDomain registration must "
        "not silently select a Provider");

    const auto process_result =
        engine.process(request, call);
    expect(
        process_result.status.ok &&
            process_result.value &&
            process_result.value->valid,
        "state Provider configuration must remain "
        "isolated from the base preprocessing path");
}

void testUniqueStateDomainRouting() {
    auto clock =
        std::make_shared<ManualRuntimeClock>();
    auto vehicle = std::make_shared<CountingProvider>(
        clock, preprocess::StateDomain::Vehicle,
        std::map<std::string, std::string>{
            {"speed_kmh", "42"}});
    auto environment =
        std::make_shared<CountingProvider>(
            clock,
            preprocess::StateDomain::Environment,
            std::map<std::string, std::string>{
                {"outside_temperature", "25"}});
    preprocess::PreprocessEngine engine(
        clock, {vehicle, environment});
    const auto request = requestFor(clock);
    const auto call = callFor(request);

    const auto capabilities =
        engine.getCapabilities(call);
    expect(
        capabilities.status.ok &&
            capabilities.value &&
            capabilities.value->size() == 2 &&
            vehicle->query_calls == 0 &&
            environment->query_calls == 0,
        "unique domains must expose one capability "
        "record per Provider without live reads");

    preprocess::StateQuery speed;
    speed.request_id = request.request_id;
    speed.session_id = request.session_id;
    speed.turn_id = request.turn_id;
    speed.state_type =
        preprocess::StateDomain::Vehicle;
    speed.fields = {"speed_kmh"};
    const auto speed_result =
        engine.queryRuntimeState(speed, call);
    expect(
        speed_result.status.ok &&
            speed_result.value &&
            speed_result.value->success &&
            speed_result.value->values.at(
                "speed_kmh") == "42" &&
            vehicle->query_calls == 1 &&
            environment->query_calls == 0,
        "a Vehicle query must route directly to the "
        "unique Vehicle Provider");

    auto temperature = speed;
    temperature.state_type =
        preprocess::StateDomain::Environment;
    temperature.fields = {
        "outside_temperature"};
    const auto temperature_result =
        engine.queryRuntimeState(
            temperature, call);
    expect(
        temperature_result.status.ok &&
            temperature_result.value &&
            temperature_result.value->success &&
            temperature_result.value->values.at(
                "outside_temperature") == "25" &&
            vehicle->query_calls == 1 &&
            environment->query_calls == 1,
        "an Environment query must route directly to "
        "the unique Environment Provider");
}

}  // namespace

int main() {
    try {
        testProcessExampleAndObservability();
        testUtf8TruncationAndEventInput();
        testStateDomainUniquenessIsFailClosed();
        testUniqueStateDomainRouting();
        expect(
            preprocess::toString(
                preprocess::StateDomain::Vehicle) ==
                    "VEHICLE" &&
                preprocess::toString(
                    preprocess::StateDomain::
                        Environment) ==
                    "ENVIRONMENT",
            "state-domain wire names must be stable");
        std::cout
            << "Preprocessing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
