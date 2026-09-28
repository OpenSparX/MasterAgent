/**
 * @file sparx_pipeline_harness.cpp
 * @brief Pipeline Harness — pluggable orchestrator for edge-cloud dual-path inference.
 *
 * Core execution flow:
 *   1. Speculative cache miss arrives here (cache hits never reach the harness)
 *   2. Pre-score confidence via IConfidenceScorer
 *   3. If confidence < high_threshold → fire cloud path asynchronously
 *   4. Run local inference (blocking, on-device)
 *   5. Post-score local result
 *   6. Wait for cloud result up to deadline
 *   7. Arbiter selects final output
 *
 * Design principles:
 *   - Token-friendly: cloud only receives compressed prompts
 *   - Efficiency-first: cloud never blocks local path; deadline enforced
 *   - Pluggable: all components are interfaces, swappable via config
 */

#include "sparx_pipeline_harness.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

namespace sparx {
namespace harness {

// ═══════════════════════════════════════════════════════════════════════════════
// PipelineHarness
// ═══════════════════════════════════════════════════════════════════════════════

PipelineHarness::PipelineHarness() = default;
PipelineHarness::~PipelineHarness() = default;

// ─── Registration ───────────────────────────────────────────────────────────

void PipelineHarness::registerPromptEngine(
    const std::string& name, std::shared_ptr<IPromptEngine> engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    prompt_engines_[name] = std::move(engine);
}

void PipelineHarness::registerCloudBackend(
    const std::string& name, std::shared_ptr<ICloudBackend> backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    cloud_backends_[name] = std::move(backend);
}

void PipelineHarness::registerArbiter(
    const std::string& name, std::shared_ptr<IArbiter> arbiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    arbiters_[name] = std::move(arbiter);
}

void PipelineHarness::registerConfidenceScorer(
    const std::string& name, std::shared_ptr<IConfidenceScorer> scorer) {
    std::lock_guard<std::mutex> lock(mutex_);
    scorers_[name] = std::move(scorer);
}

void PipelineHarness::registerLocalInference(
    const std::string& name, std::shared_ptr<ILocalInference> local) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_inferences_[name] = std::move(local);
}

// ─── Configuration ──────────────────────────────────────────────────────────

void PipelineHarness::applyConfig(const HarnessConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    resolveActiveComponents();
}

void PipelineHarness::loadConfig(const std::string& yaml_path) {
    auto config = parseHarnessConfig(yaml_path);
    applyConfig(config);
}

void PipelineHarness::resolveActiveComponents() {
    active_prompt_engine_.reset();
    active_cloud_backend_.reset();
    active_arbiter_.reset();
    active_scorer_.reset();
    active_local_.reset();
    // Resolve prompt engine
    if (auto it = prompt_engines_.find(config_.prompt_engine); it != prompt_engines_.end()) {
        active_prompt_engine_ = it->second;
    }

    // Resolve cloud backend
    if (auto it = cloud_backends_.find(config_.cloud_backend); it != cloud_backends_.end()) {
        active_cloud_backend_ = it->second;
    }

    // Resolve arbiter
    if (auto it = arbiters_.find(config_.arbiter); it != arbiters_.end()) {
        active_arbiter_ = it->second;
    }

    // Resolve scorer
    if (auto it = scorers_.find(config_.confidence_scorer); it != scorers_.end()) {
        active_scorer_ = it->second;
    }

    // Resolve local inference (use first available if no name match)
    if (!local_inferences_.empty()) {
        active_local_ = local_inferences_.begin()->second;
    }
}

// ─── Runtime Control ────────────────────────────────────────────────────────

void PipelineHarness::setCloudEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.cloud_enabled = enabled;
}

bool PipelineHarness::isCloudEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.cloud_enabled;
}

void PipelineHarness::setActivePromptEngine(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.prompt_engine = name;
    resolveActiveComponents();
}

void PipelineHarness::setActiveCloudBackend(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.cloud_backend = name;
    resolveActiveComponents();
}

void PipelineHarness::setActiveArbiter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.arbiter = name;
    resolveActiveComponents();
}

bool PipelineHarness::isReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_prompt_engine_ != nullptr &&
           active_arbiter_ != nullptr &&
           active_scorer_ != nullptr &&
           active_local_ != nullptr;
}

// ─── Execution ──────────────────────────────────────────────────────────────

PreScoreSignals PipelineHarness::buildPreScoreSignals(
    const PipelineRequest& request) const {

    PreScoreSignals signals;
    signals.intent_type = request.intent_type;
    signals.input_token_count = static_cast<int>(request.user_input.size() / 4);  // rough
    signals.is_deterministic = false;  // Caller marks this if skill engine handles it

    // Deterministic intent types (never need cloud)
    static const std::vector<std::string> kDeterministicIntents = {
        "vehicle_control", "media", "phone"
    };
    for (const auto& di : kDeterministicIntents) {
        if (request.intent_type == di) {
            signals.is_deterministic = true;
            break;
        }
    }

    return signals;
}


namespace {
// Promise futures do not join a worker during destruction. Each worker owns its
// captured backends and one shared slot, never a pointer to the harness. At most
// one local and one cloud call can remain outstanding per harness, even when an
// adapter ignores its timeout. Late results are discarded, not re-arbitrated.
template<class T, class F>
std::future<T> launchBounded(const std::shared_ptr<std::atomic<bool>>& busy, F work) {
    if (busy->exchange(true)) return {};
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    try {
        std::thread([busy, promise, work = std::move(work)]() mutable {
            try { promise->set_value(work()); }
            catch (...) { promise->set_exception(std::current_exception()); }
            busy->store(false);
        }).detach();
    } catch (...) { busy->store(false); throw; }
    return future;
}
template<class T>
std::optional<T> completed(std::future<T>& future, std::chrono::steady_clock::time_point deadline) {
    if (!future.valid() || future.wait_until(deadline) != std::future_status::ready) return {};
    try { return future.get(); } catch (...) { return {}; }
}
}

PipelineResponse PipelineHarness::execute(const PipelineRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    PipelineResponse response;
    std::shared_ptr<IPromptEngine> prompt;
    std::shared_ptr<ICloudBackend> cloud;
    std::shared_ptr<IArbiter> arbiter;
    std::shared_ptr<IConfidenceScorer> scorer;
    std::shared_ptr<ILocalInference> local;
    HarnessConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prompt = active_prompt_engine_; cloud = active_cloud_backend_;
        arbiter = active_arbiter_; scorer = active_scorer_; local = active_local_;
        config = config_;
    }
    if (!prompt || !arbiter || !scorer || !local) {
        response.result.content = "Pipeline not initialized";
        response.result.source = ArbiterOutput::Source::Fallback;
        response.result.reason = "harness not ready";
        return response;
    }
    const auto deadline = start + std::chrono::milliseconds(std::max(0, arbiter->getDeadline(request.intent_type)));
    const auto signals = buildPreScoreSignals(request);
    const auto pre = scorer->preScore(signals);
    response.confidence = pre;
    response.prompt_engine_used = prompt->name();
    std::future<CloudResult> cloud_future;
    if (config.cloud_enabled && config.arbiter_config.strategy != ArbiterStrategy::LocalOnly &&
        cloud && cloud->isReady() && pre.overall < config.confidence_thresholds.high) {
        const auto compressed = prompt->compress(request.user_input, request.history, request.context_vars);
        cloud_future = launchBounded<CloudResult>(cloud_busy_, [cloud, compressed] {
            return cloud->infer(compressed.user_prompt, compressed.system_prompt);
        });
        response.cloud_fired = cloud_future.valid();
        if (response.cloud_fired) response.cloud_input_tokens = compressed.estimated_tokens;
    }
    const auto local_prompt = prompt->renderLocal(request.user_input, request.history, request.context_vars);
    auto local_future = launchBounded<LocalResult>(local_busy_, [local, scorer, signals, local_prompt] {
        const auto begin = std::chrono::steady_clock::now();
        auto result = local->infer(local_prompt);
        result.latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
        if (result.success) result.confidence = scorer->postScore(signals, local->getLastPostSignals());
        return result;
    });
    const auto local_result = completed(local_future, deadline);
    const auto cloud_result = completed(cloud_future, deadline);
    if (local_result) {
        response.local_latency_ms = local_result->latency_ms;
        response.confidence = local_result->confidence;
    }
    if (cloud_result) {
        response.cloud_latency_ms = cloud_result->latency_ms;
        response.cloud_output_tokens = cloud_result->output_tokens;
    }
    response.result = arbiter->arbitrate(local_result, cloud_result, request.intent_type);
    response.total_latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    response.result.total_latency_ms = response.total_latency_ms;
    return response;
}

PipelineResponse PipelineHarness::executeCloudOnly(const PipelineRequest& request) {
    std::shared_ptr<IPromptEngine> prompt;
    std::shared_ptr<ICloudBackend> cloud;
    { std::lock_guard<std::mutex> lock(mutex_); prompt = active_prompt_engine_; cloud = active_cloud_backend_; }
    PipelineResponse response;
    auto start = std::chrono::steady_clock::now();

    if (!prompt || !cloud) {
        response.result.content = "Cloud path not configured";
        response.result.source = ArbiterOutput::Source::Fallback;
        return response;
    }

    auto compressed = prompt->compress(
        request.user_input, request.history, request.context_vars);
    response.cloud_input_tokens = compressed.estimated_tokens;
    response.prompt_engine_used = prompt->name();

    auto cloud_result = cloud->infer(
        compressed.user_prompt, compressed.system_prompt);

    response.cloud_latency_ms = cloud_result.latency_ms;
    response.cloud_output_tokens = cloud_result.output_tokens;
    response.cloud_fired = true;

    if (cloud_result.success) {
        response.result.content = cloud_result.content;
        response.result.source = ArbiterOutput::Source::Cloud;
        response.result.reason = "cloud_only mode";
        response.result.cloud_result = cloud_result;
    } else {
        response.result.content = "Cloud inference failed: " + cloud_result.error;
        response.result.source = ArbiterOutput::Source::Fallback;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    response.total_latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return response;
}

PipelineResponse PipelineHarness::executeLocalOnly(const PipelineRequest& request) {
    std::shared_ptr<IPromptEngine> prompt;
    std::shared_ptr<ILocalInference> local;
    { std::lock_guard<std::mutex> lock(mutex_); prompt = active_prompt_engine_; local = active_local_; }
    PipelineResponse response;
    auto start = std::chrono::steady_clock::now();

    if (!prompt || !local) {
        response.result.content = "Local path not configured";
        response.result.source = ArbiterOutput::Source::Fallback;
        return response;
    }

    std::string local_prompt = prompt->renderLocal(
        request.user_input, request.history, request.context_vars);
    response.prompt_engine_used = prompt->name();

    auto local_result = local->infer(local_prompt);
    response.local_latency_ms = local_result.latency_ms;
    response.cloud_fired = false;

    if (local_result.success) {
        response.result.content = local_result.content;
        response.result.source = ArbiterOutput::Source::Local;
        response.result.reason = "local_only mode";
        response.result.local_result = local_result;
    } else {
        response.result.content = "Local inference failed: " + local_result.error;
        response.result.source = ArbiterOutput::Source::Fallback;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    response.total_latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return response;
}

// ═══════════════════════════════════════════════════════════════════════════════
// YAML Config Parser
// ═══════════════════════════════════════════════════════════════════════════════

HarnessConfig parseHarnessConfig(const std::string& yaml_path) {
    HarnessConfig config;
    std::ifstream file(yaml_path);
    if (!file.is_open()) return config;

    std::string line;
    std::string current_section;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Detect top-level sections
        if (!line.empty() && line[0] != ' ' && line.back() == ':') {
            current_section = line.substr(0, line.size() - 1);
            continue;
        }

        // Parse key: value
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);

        std::string val = line.substr(colon + 1);
        val.erase(0, val.find_first_not_of(" \t\""));
        if (!val.empty() && val.back() == '"') val.pop_back();
        // Remove trailing comments
        auto comment_pos = val.find(" #");
        if (comment_pos != std::string::npos) val = val.substr(0, comment_pos);
        val.erase(val.find_last_not_of(" \t") + 1);

        // ─── harness section ───
        if (current_section == "harness") {
            if (key == "prompt_engine") config.prompt_engine = val;
            else if (key == "cloud_backend") config.cloud_backend = val;
            else if (key == "arbiter") config.arbiter = val;
            else if (key == "confidence_scorer") config.confidence_scorer = val;
            else if (key == "cloud_enabled") config.cloud_enabled = (val == "true");
            else if (key == "trace_decisions") config.trace_decisions = (val == "true");
        }
        // ─── cloud section ───
        else if (current_section == "cloud") {
            if (key == "endpoint") config.cloud_config.endpoint = val;
            else if (key == "api_key") config.cloud_config.api_key = val;
            else if (key == "api_key_env") config.cloud_config.api_key_env = val;
            else if (key == "model") config.cloud_config.model = val;
            else if (key == "max_tokens") config.cloud_config.max_tokens = std::stoi(val);
            else if (key == "temperature") config.cloud_config.temperature = std::stof(val);
            else if (key == "timeout_ms") config.cloud_config.timeout_ms = std::stoi(val);
            else if (key == "provider") {
                if (val == "openai_compatible")
                    config.cloud_config.provider = CloudBackendConfig::Provider::OpenAICompatible;
                else if (val == "anthropic")
                    config.cloud_config.provider = CloudBackendConfig::Provider::Anthropic;
                else if (val == "custom")
                    config.cloud_config.provider = CloudBackendConfig::Provider::Custom;
            }
        }
        // ─── prompt section ───
        else if (current_section == "prompt") {
            if (key == "max_cloud_input_tokens")
                config.prompt_config.max_cloud_input_tokens = std::stoi(val);
            else if (key == "max_history_turns")
                config.prompt_config.max_history_turns = std::stoi(val);
            else if (key == "relevance_threshold")
                config.prompt_config.relevance_threshold = std::stof(val);
            else if (key == "template_dir")
                config.prompt_config.template_dir = val;
            else if (key == "default_template")
                config.prompt_config.default_template = val;
        }
        // ─── arbiter section ───
        else if (current_section == "arbiter_config") {
            if (key == "deadline_ms")
                config.arbiter_config.deadline_ms = std::stoi(val);
            else if (key == "confidence_gap_threshold")
                config.arbiter_config.confidence_gap_threshold = std::stof(val);
            else if (key == "strategy") {
                if (val == "cloud_prefer")
                    config.arbiter_config.strategy = ArbiterStrategy::CloudPrefer;
                else if (val == "latency_first")
                    config.arbiter_config.strategy = ArbiterStrategy::LatencyFirst;
                else if (val == "confidence")
                    config.arbiter_config.strategy = ArbiterStrategy::Confidence;
                else if (val == "local_only")
                    config.arbiter_config.strategy = ArbiterStrategy::LocalOnly;
            }
        }
        // ─── confidence section ───
        else if (current_section == "confidence") {
            if (key == "high_threshold")
                config.confidence_thresholds.high = std::stof(val);
            else if (key == "low_threshold")
                config.confidence_thresholds.low = std::stof(val);
        }
    }

    return config;
}

}  // namespace harness
}  // namespace sparx
