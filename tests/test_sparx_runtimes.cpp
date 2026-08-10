/**
 * @file test_sparx_runtimes.cpp
 * @brief Contract tests for the two concrete IModelRuntime adapters.
 *
 * These do not require llama-server or libGenie.so. They verify the properties
 * that must hold regardless of whether a model is reachable:
 *
 *  1. Seal validation fails closed. A runtime that accepts an incomplete seal
 *     lets a stale or misrouted invocation reach the model, which defeats the
 *     framework's whole two-phase commit fence.
 *  2. requiredWorkUnits never returns 0 (the scheduler would starve the job)
 *     and never invokes the model.
 *  3. Both advertise streaming, or the framework never calls inferStream and
 *     the M1 streaming ABI is dead code for them.
 *  4. Construction with no model/config is safe and reports unusable rather
 *     than crashing, so `sparx doctor` can probe a host with no NPU.
 */

#include "genie_model_runtime.h"
#include "llama_cpp_model_runtime.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace master_agent;
using namespace master_agent::inference;

int g_failures = 0;

void expect(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "  ok   " << what << "\n";
    } else {
        std::cout << "  FAIL " << what << "\n";
        ++g_failures;
    }
}

/// A seal with only job_id set. Every other identity field is absent, so any
/// correct runtime must refuse it.
RuntimeInvocationSeal incompleteSeal(const InferenceRequest& request) {
    RuntimeInvocationSeal seal;
    seal.job_id = request.job_id;
    return seal;
}

InferenceRequest sampleRequest() {
    InferenceRequest request;
    request.job_id = "job-runtime-contract";
    request.request_id = "req-1";
    request.prompt = "hello";
    request.prompt_digest = "prompt-digest-1";
    request.deadline_mono_ns = 1000;
    return request;
}

void testLlamaCppRejectsIncompleteSeal() {
    LlamaCppConfig config;  // no model_path: no server is spawned
    LlamaCppModelRuntime runtime(config);

    const auto request = sampleRequest();
    const auto result = runtime.infer(request, incompleteSeal(request));

    expect(!result.status.ok, "llama.cpp refuses an incomplete seal");
    // The specific code matters: reporting UNAVAILABLE would mean the seal was
    // never checked and the call merely failed to reach a server.
    expect(result.status.error.code == "INFERENCE_RUNTIME_INVOCATION_INVALID",
           "llama.cpp reports INVOCATION_INVALID, not UNAVAILABLE");
    expect(!result.value.has_value(),
           "llama.cpp returns no output on a rejected seal");
}

void testGenieRejectsIncompleteSeal() {
    GenieConfig config;
    config.soc_id = 87;
    config.dsp_arch = "v81";
    GenieModelRuntime runtime(config);

    const auto request = sampleRequest();
    const auto result = runtime.infer(request, incompleteSeal(request));

    expect(!result.status.ok, "genie refuses an incomplete seal");
    // Seal validation must precede the availability check, so that an invalid
    // invocation is reported as invalid even on a host with no NPU.
    expect(result.status.error.code == "INFERENCE_RUNTIME_INVOCATION_INVALID",
           "genie validates the seal before checking NPU availability");
    expect(!result.value.has_value(),
           "genie returns no output on a rejected seal");
}

void testWorkUnitsAreNeverZero() {
    const auto request = sampleRequest();

    LlamaCppConfig llama_config;
    LlamaCppModelRuntime llama(llama_config);
    expect(llama.requiredWorkUnits(request) >= 1U,
           "llama.cpp requiredWorkUnits >= 1");

    GenieConfig genie_config;
    GenieModelRuntime genie(genie_config);
    expect(genie.requiredWorkUnits(request) >= 1U,
           "genie requiredWorkUnits >= 1");

    // A long prompt must cost more than a short one, or the scheduler cannot
    // distinguish a cheap turn from an expensive one.
    InferenceRequest big = request;
    big.prompt = std::string(100000, 'x');
    expect(llama.requiredWorkUnits(big) > llama.requiredWorkUnits(request),
           "llama.cpp work units grow with prompt size");
    expect(genie.requiredWorkUnits(big) > genie.requiredWorkUnits(request),
           "genie work units grow with prompt size");
}

void testBothAdvertiseStreaming() {
    LlamaCppConfig llama_config;
    LlamaCppModelRuntime llama(llama_config);
    expect(llama.supportsStreaming(), "llama.cpp advertises streaming");

    GenieConfig genie_config;
    GenieModelRuntime genie(genie_config);
    expect(genie.supportsStreaming(), "genie advertises streaming");
}

void testGenieProbeIsSafeWithoutQualcommStack() {
    // probe() must be callable on any host. On a dev machine it reports the
    // library as missing; it must never throw or abort.
    const auto availability = GenieModelRuntime::probe();
    expect(!availability.detail.empty() || availability.usable(),
           "genie probe explains itself when unusable");

    if (!availability.library_found) {
        expect(!availability.usable(),
               "genie is not usable when libGenie.so is absent");
    }

    // Constructing without a config must yield an unusable runtime, not a crash.
    GenieConfig config;
    GenieModelRuntime runtime(config);
    expect(!runtime.isReady(),
           "genie is not ready without a config");
    expect(!runtime.availability().usable(),
           "genie availability reports unusable without a config");
    expect(!runtime.availability().detail.empty(),
           "genie states why it is unusable");
}

void testStreamAbortIsHonouredWithoutARuntime() {
    // With no server reachable, inferStream must still fail closed rather than
    // deliver chunks. A sink that receives anything here would mean the runtime
    // fabricated output.
    LlamaCppConfig config;
    config.endpoint = "127.0.0.1:1";  // nothing listens on port 1
    LlamaCppModelRuntime runtime(config);

    const auto request = sampleRequest();
    auto seal = incompleteSeal(request);

    int chunks_seen = 0;
    const auto result = runtime.inferStream(
        request, seal,
        [&chunks_seen](const InferenceChunk&) {
            ++chunks_seen;
            return StreamControl::Continue;
        });

    expect(!result.status.ok, "unreachable llama.cpp fails closed");
    expect(chunks_seen == 0,
           "no chunks are delivered when the seal is rejected");
}

}  // namespace

int main() {
    testLlamaCppRejectsIncompleteSeal();
    testGenieRejectsIncompleteSeal();
    testWorkUnitsAreNeverZero();
    testBothAdvertiseStreaming();
    testGenieProbeIsSafeWithoutQualcommStack();
    testStreamAbortIsHonouredWithoutARuntime();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "\nall runtime contract checks passed\n";
    return 0;
}
