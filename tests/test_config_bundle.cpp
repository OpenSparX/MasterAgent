#include "master_agent/config/model_profile_loader.h"

#include "test_support.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

using master_agent::config::loadModelSelection;
using master_agent::test_support::expect;

std::filesystem::path sourceBundle() {
    return std::filesystem::path(MASTER_AGENT_SOURCE_DIR) / "agent-config";
}

void testOfficialDefaultsResolve() {
    const auto loaded = loadModelSelection(sourceBundle());
    expect(loaded.status.ok, "official model selection must load");
    expect(loaded.value.has_value(), "official model selection must exist");
    expect(loaded.value->bundle_name == "official-automotive-default",
           "official bundle name mismatch");
    expect(loaded.value->intent.id == "model.local.intent.mock",
           "official default intent profile mismatch");
    expect(loaded.value->intent.runtime_type == "deterministic_mock",
           "official default intent runtime mismatch");
    expect(loaded.value->classifier.id ==
               "model.local.classifier.deterministic",
           "official classifier profile mismatch");
    expect(!loaded.value->cloud_fallback_enabled,
           "official cloud fallback must default off");
    expect(!loaded.value->cloud_intent.has_value(),
           "disabled cloud fallback must not select a model");
}

void testExplicitQwenProfileResolves() {
    const auto loaded = loadModelSelection(
        sourceBundle(), std::string("model.local.intent.qwen2_5_3b"));
    expect(loaded.status.ok, "explicit Qwen profile must load");
    expect(loaded.value.has_value(), "explicit Qwen selection must exist");
    expect(loaded.value->intent.runtime_type == "llama_cpp",
           "Qwen runtime type mismatch");
    expect(loaded.value->intent.runtime_mode == "attach_or_spawn",
           "Qwen runtime mode mismatch");
    expect(loaded.value->intent.artifact_path_ref == "SPARX_MODEL",
           "Qwen artifact reference mismatch");
    expect(loaded.value->intent.default_endpoint == "127.0.0.1:8080",
           "Qwen endpoint mismatch");
    expect(loaded.value->intent.context_length == 4096,
           "Qwen context length mismatch");
}

void testExplicitDisabledCloudFailsClosed() {
    const auto loaded = loadModelSelection(
        sourceBundle(), std::nullopt,
        std::string("model.cloud.intent.fake"));
    expect(!loaded.status.ok, "disabled cloud profile must fail closed");
    expect(loaded.status.error.code == "CONFIG_CLOUD_FALLBACK_DISABLED",
           "disabled cloud profile returned wrong error");
}

void testUnknownIntentProfileFailsClosed() {
    const auto loaded = loadModelSelection(
        sourceBundle(), std::string("model.local.intent.unknown"));
    expect(!loaded.status.ok, "unknown intent profile must fail closed");
    expect(loaded.status.error.code == "CONFIG_INTENT_MODEL_NOT_FOUND",
           "unknown intent profile returned wrong error");
}

}  // namespace

int main() {
    try {
        testOfficialDefaultsResolve();
        testExplicitQwenProfileResolves();
        testExplicitDisabledCloudFailsClosed();
        testUnknownIntentProfileFailsClosed();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
