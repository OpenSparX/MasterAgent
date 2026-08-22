/**
 * @file test_harness.cpp
 * @brief Unit tests for the edge-cloud pipeline harness.
 *
 * Tests cover:
 *   - Prompt engine compression and distillation
 *   - Confidence scoring (pre and post)
 *   - Arbiter logic (all strategies)
 *   - Pipeline harness end-to-end (with mock backends)
 */

#include "../cli/include/sparx_arbiter.h"
#include "../cli/include/sparx_cloud_backend.h"
#include "../cli/include/sparx_confidence_scorer.h"
#include "../cli/include/sparx_pipeline_harness.h"
#include "../cli/include/sparx_prompt_engine.h"

#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

using namespace sparx::harness;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Framework (lightweight, no static init issues)
// ═══════════════════════════════════════════════════════════════════════════════

static int tests_passed = 0;
static int tests_failed = 0;

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase>& getTests() {
    static std::vector<TestCase> tests;
    return tests;
}

#define TEST(name) \
    void test_##name(); \
    static bool reg_##name = (getTests().push_back({#name, test_##name}), true); \
    void test_##name()

#define ASSERT_TRUE(expr) \
    if (!(expr)) throw std::runtime_error("assertion failed: " #expr)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("assertion failed: " #a " == " #b)

#define ASSERT_GT(a, b) \
    if (!((a) > (b))) throw std::runtime_error("assertion failed: " #a " > " #b)

#define ASSERT_LT(a, b) \
    if (!((a) < (b))) throw std::runtime_error("assertion failed: " #a " < " #b)

// ═══════════════════════════════════════════════════════════════════════════════
// Mock Local Inference
// ═══════════════════════════════════════════════════════════════════════════════

class MockLocalInference : public ILocalInference {
public:
    MockLocalInference(const std::string& response = "local response",
                       int latency_ms = 100, bool success = true)
        : response_(response), latency_ms_(latency_ms), success_(success) {}

    LocalResult infer(const std::string& /*prompt*/) const override {
        std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms_));
        LocalResult r;
        r.success = success_;
        r.content = response_;
        r.latency_ms = latency_ms_;
        return r;
    }

    PostScoreSignals getLastPostSignals() const override {
        PostScoreSignals signals;
        signals.avg_logprob = -1.0f;
        signals.output_token_count = 20;
        signals.format_valid = true;
        return signals;
    }

    bool isReady() const override { return true; }
    std::string name() const override { return "mock_local"; }

private:
    std::string response_;
    int latency_ms_;
    bool success_;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Prompt Engine Tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(prompt_engine_distill_navigation) {
    PromptEngineConfig config;
    CompressedPromptEngine engine(config);

    auto intent = engine.distill("导航到最近的加油站");
    if (intent.task_type != "navigation") {
        throw std::runtime_error("expected navigation, got: " + intent.task_type);
    }
}

TEST(prompt_engine_distill_vehicle_control) {
    PromptEngineConfig config;
    CompressedPromptEngine engine(config);

    auto intent = engine.distill("把空调温度调到25度");
    if (intent.task_type != "vehicle_control") {
        throw std::runtime_error("expected vehicle_control, got: " + intent.task_type);
    }
}

TEST(prompt_engine_distill_media) {
    PromptEngineConfig config;
    CompressedPromptEngine engine(config);

    auto intent = engine.distill("播放周杰伦的歌");
    if (intent.task_type != "media") {
        throw std::runtime_error("expected media, got: " + intent.task_type);
    }
}

TEST(prompt_engine_compress_reduces_tokens) {
    PromptEngineConfig config;
    config.max_cloud_input_tokens = 500;
    CompressedPromptEngine engine(config);

    // Simulate a long history
    std::vector<ConversationTurn> history;
    for (int i = 0; i < 20; ++i) {
        history.push_back({"user", "这是第" + std::to_string(i) + "轮对话内容，包含很多无关信息", i * 1000, 1.0f});
        history.push_back({"assistant", "这是助手的回复" + std::to_string(i), i * 1000 + 500, 1.0f});
    }

    auto result = engine.compress("附近有充电桩吗", history, {});

    // Compressed result should be well under budget
    ASSERT_LT(result.estimated_tokens, 500);
    ASSERT_TRUE(!result.user_prompt.empty());
    ASSERT_TRUE(!result.system_prompt.empty());
}

TEST(prompt_engine_prune_keeps_relevant) {
    PromptEngineConfig config;
    config.max_history_turns = 2;
    config.relevance_threshold = 0.2f;
    CompressedPromptEngine engine(config);

    std::vector<ConversationTurn> history = {
        {"user", "今天天气怎么样", 1000, 1.0f},
        {"assistant", "今天晴天，25度", 1500, 1.0f},
        {"user", "导航到机场", 2000, 1.0f},
        {"assistant", "已为您规划路线", 2500, 1.0f},
    };

    DistilledIntent intent;
    intent.task_type = "navigation";
    intent.query = "还有多远到机场";

    auto pruned = engine.prune(history, intent);
    ASSERT_TRUE(pruned.size() <= 2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Confidence Scorer Tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(confidence_deterministic_is_high) {
    HeuristicScorer scorer;

    PreScoreSignals signals;
    signals.is_deterministic = true;
    signals.intent_type = "vehicle_control";

    auto score = scorer.preScore(signals);
    ASSERT_GT(score.overall, 0.9f);
}

TEST(confidence_novel_query_is_low) {
    HeuristicScorer scorer;

    PreScoreSignals signals;
    signals.is_deterministic = false;
    signals.input_token_count = 300;
    signals.speculative_hit_rate = 0.0f;
    signals.similar_intent_successes = 0;
    signals.similar_intent_failures = 5;

    auto score = scorer.preScore(signals);
    ASSERT_LT(score.overall, 0.5f);
}

TEST(confidence_post_score_with_good_logprob) {
    HeuristicScorer scorer;

    PreScoreSignals pre;
    pre.is_deterministic = false;
    pre.speculative_hit_rate = 0.5f;

    PostScoreSignals post;
    post.avg_logprob = -0.5f;
    post.output_token_count = 30;
    post.format_valid = true;

    auto score = scorer.postScore(pre, post);
    ASSERT_GT(score.overall, 0.5f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Arbiter Tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(arbiter_cloud_prefer_both_available) {
    ArbiterConfig config;
    config.strategy = ArbiterStrategy::CloudPrefer;
    CloudPreferArbiter arbiter(config);

    LocalResult local{true, "local answer", 50, {}, ""};
    CloudResult cloud{true, "cloud answer", 10, 20, 200, "", "gpt-4", "req-1"};

    auto out = arbiter.arbitrate(local, cloud);
    ASSERT_EQ(out.source, ArbiterOutput::Source::Cloud);
    ASSERT_EQ(out.content, std::string("cloud answer"));
}

TEST(arbiter_cloud_prefer_only_local) {
    ArbiterConfig config;
    config.strategy = ArbiterStrategy::CloudPrefer;
    CloudPreferArbiter arbiter(config);

    LocalResult local{true, "local answer", 50, {}, ""};
    std::optional<CloudResult> cloud;

    auto out = arbiter.arbitrate(local, cloud);
    ASSERT_EQ(out.source, ArbiterOutput::Source::Local);
}

TEST(arbiter_latency_first_picks_faster) {
    ArbiterConfig config;
    LatencyFirstArbiter arbiter(config);

    LocalResult local{true, "local fast", 30, {}, ""};
    CloudResult cloud{true, "cloud slow", 10, 20, 500, "", "model", "id"};

    auto out = arbiter.arbitrate(local, cloud);
    ASSERT_EQ(out.source, ArbiterOutput::Source::Local);
    ASSERT_EQ(out.content, std::string("local fast"));
}

TEST(arbiter_fallback_when_both_fail) {
    ArbiterConfig config;
    config.fallback_message = "Service unavailable";
    CloudPreferArbiter arbiter(config);

    LocalResult local{false, "", 0, {}, "model not loaded"};
    CloudResult cloud{false, "", 0, 0, 0, "timeout", "", ""};

    auto out = arbiter.arbitrate(local, cloud);
    ASSERT_EQ(out.source, ArbiterOutput::Source::Fallback);
    ASSERT_EQ(out.content, std::string("Service unavailable"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pipeline Harness Integration Test
// ═══════════════════════════════════════════════════════════════════════════════

TEST(harness_end_to_end_with_mocks) {
    PipelineHarness harness;

    PromptEngineConfig pe_config;
    harness.registerPromptEngine("compressed",
        std::make_shared<CompressedPromptEngine>(pe_config));

    harness.registerCloudBackend("mock",
        std::make_shared<MockCloudBackend>("cloud response", 50));

    ArbiterConfig arb_config;
    arb_config.strategy = ArbiterStrategy::CloudPrefer;
    harness.registerArbiter("cloud_prefer",
        std::make_shared<CloudPreferArbiter>(arb_config));

    harness.registerConfidenceScorer("heuristic",
        std::make_shared<HeuristicScorer>());

    harness.registerLocalInference("mock",
        std::make_shared<MockLocalInference>("local response", 30));

    HarnessConfig config;
    config.prompt_engine = "compressed";
    config.cloud_backend = "mock";
    config.arbiter = "cloud_prefer";
    config.confidence_scorer = "heuristic";
    config.cloud_enabled = true;
    config.confidence_thresholds.high = 0.85f;
    config.confidence_thresholds.low = 0.4f;
    config.arbiter_config = arb_config;
    harness.applyConfig(config);

    ASSERT_TRUE(harness.isReady());

    PipelineRequest req;
    req.user_input = "explain quantum computing basics";
    req.intent_type = "general_qa";

    auto response = harness.execute(req);

    ASSERT_TRUE(!response.result.content.empty());
    ASSERT_TRUE(response.total_latency_ms > 0);
}

TEST(harness_deterministic_skips_cloud) {
    PipelineHarness harness;

    PromptEngineConfig pe_config;
    harness.registerPromptEngine("compressed",
        std::make_shared<CompressedPromptEngine>(pe_config));

    harness.registerCloudBackend("mock",
        std::make_shared<MockCloudBackend>("should not see this", 500));

    ArbiterConfig arb_config;
    harness.registerArbiter("cloud_prefer",
        std::make_shared<CloudPreferArbiter>(arb_config));

    harness.registerConfidenceScorer("heuristic",
        std::make_shared<HeuristicScorer>());

    harness.registerLocalInference("mock",
        std::make_shared<MockLocalInference>("AC set to 25C", 10));

    HarnessConfig config;
    config.prompt_engine = "compressed";
    config.cloud_backend = "mock";
    config.arbiter = "cloud_prefer";
    config.confidence_scorer = "heuristic";
    config.cloud_enabled = true;
    config.confidence_thresholds.high = 0.85f;
    harness.applyConfig(config);

    PipelineRequest req;
    req.user_input = "set AC to 25 degrees";
    req.intent_type = "vehicle_control";

    auto response = harness.execute(req);

    // Cloud should not have been fired (deterministic intent)
    ASSERT_TRUE(!response.cloud_fired);
    ASSERT_EQ(response.result.source, ArbiterOutput::Source::Local);
}

TEST(harness_local_only_mode) {
    PipelineHarness harness;

    PromptEngineConfig pe_config;
    harness.registerPromptEngine("compressed",
        std::make_shared<CompressedPromptEngine>(pe_config));

    harness.registerLocalInference("mock",
        std::make_shared<MockLocalInference>("offline answer", 20));

    ArbiterConfig arb_config;
    harness.registerArbiter("cloud_prefer",
        std::make_shared<CloudPreferArbiter>(arb_config));

    harness.registerConfidenceScorer("heuristic",
        std::make_shared<HeuristicScorer>());

    HarnessConfig config;
    config.prompt_engine = "compressed";
    config.arbiter = "cloud_prefer";
    config.confidence_scorer = "heuristic";
    config.cloud_enabled = false;
    harness.applyConfig(config);

    PipelineRequest req;
    req.user_input = "what is a black hole";

    auto response = harness.execute(req);

    ASSERT_TRUE(!response.cloud_fired);
    ASSERT_EQ(response.result.content, std::string("offline answer"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n=== Edge-Cloud Harness Tests ===\n\n";

    for (auto& tc : getTests()) {
        std::cout << "  TEST " << tc.name << " ... ";
        try {
            tc.fn();
            std::cout << "PASSED\n";
            ++tests_passed;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            ++tests_failed;
        }
    }

    std::cout << "\n─────────────────────────────────\n";
    std::cout << "Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";

    return tests_failed > 0 ? 1 : 0;
}
