/**
 * @file bge_classifier.cpp
 * @brief Provides the deterministic BGE/classification-head/OOD test backend.
 *
 * Design trace: Intent Recognition Engine Detailed Design, sections 2.5, 3.4, and 5.
 * The production interface preserves encoder, head, calibration, margin, and
 * prototype-distance checks. This delivery intentionally uses deterministic
 * mock embeddings because no customer model binary is part of the source
 * package.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

#include <cctype>

namespace master_agent::intent {
namespace {

class DeterministicMockBgeClassifier final
    : public IBgeIntentClassifier {
public:
    explicit DeterministicMockBgeClassifier(
        std::shared_ptr<IRuntimeClock> clock)
        : clock_(std::move(clock)) {
        config_.encoder_digest =
            secureDigest("mock-bge-encoder-v1");
        config_.classification_head_digest =
            secureDigest("mock-intent-head-v1");
        config_.ood_prototype_digest =
            secureDigest("mock-class-prototypes-v1");
        config_.per_label_thresholds = {
            {"CLIMATE_CIRCULATION_AUTO", 0.80},
            {"CLIMATE_FAN_NORMAL", 0.80}};
        config_.per_label_margin_thresholds = {
            {"CLIMATE_CIRCULATION_AUTO", 0.30},
            {"CLIMATE_FAN_NORMAL", 0.30}};
        config_.per_label_ood_distance_thresholds = {
            {"CLIMATE_CIRCULATION_AUTO", 0.25},
            {"CLIMATE_FAN_NORMAL", 0.25}};
    }

    Result<BgeClassifierResult> classify(
        const std::string& normalized_text,
        const CallContext& call) const override {
        if (!clock_ ||
            !hasHostModuleIdentity(
                call,
                CallerModuleId::IntentRecognitionEngine) ||
            normalized_text.empty() ||
            deadlineExpired(call.deadline_mono_ns, *clock_)) {
            return Result<BgeClassifierResult>::Failure(
                Status::Error(
                    "intent", "BGE_REQUEST_INVALID",
                    "BGE classification request is invalid"));
        }

        // Tokenization is deliberately bounded. It models the production
        // tokenizer's truncation contract without shipping model vocabulary.
        std::size_t token_count = 0;
        bool inside_token = false;
        for (const unsigned char ch : normalized_text) {
            const bool separator =
                ch <= 0x20 || std::ispunct(ch) != 0;
            if (!separator && !inside_token) ++token_count;
            inside_token = !separator;
        }
        token_count = std::max<std::size_t>(
            token_count, normalized_text.size() / 6);
        if (token_count > config_.max_sequence_tokens) {
            return Result<BgeClassifierResult>::Failure(
                Status::Error(
                    "intent", "BGE_SEQUENCE_TOO_LONG",
                    "BGE input exceeds the configured token bound"));
        }

        BgeClassifierResult result;
        result.temperature = config_.temperature;
        result.ood_method = config_.ood_method;
        result.encoder_version = config_.encoder_id;
        result.head_version =
            config_.classification_head_id;
        result.label_schema_version =
            config_.label_schema_version;

        const auto contains =
            [&normalized_text](const std::string& value) {
                return normalized_text.find(value) !=
                       std::string::npos;
            };
        if (contains(u8"空气流通") ||
            contains(u8"车内换气") ||
            contains("ventilate cabin")) {
            result.top1_label =
                "CLIMATE_CIRCULATION_AUTO";
            result.top1_probability = 0.93;
            result.top2_label = "CLIMATE_FAN_NORMAL";
            result.top2_probability = 0.04;
            result.ood_score = 0.08;
        } else if (
            contains(u8"舒适风") ||
            contains(u8"空调舒服") ||
            contains("comfortable fan")) {
            result.top1_label = "CLIMATE_FAN_NORMAL";
            result.top1_probability = 0.91;
            result.top2_label =
                "CLIMATE_CIRCULATION_AUTO";
            result.top2_probability = 0.05;
            result.ood_score = 0.10;
        } else {
            // A classification head must always emit a label. The OOD path is
            // what prevents a forced, low-quality label from becoming a tool
            // execution.
            result.top1_label = "CLIMATE_FAN_NORMAL";
            result.top1_probability = 0.43;
            result.top2_label =
                "CLIMATE_CIRCULATION_AUTO";
            result.top2_probability = 0.37;
            result.ood_score = 0.72;
        }
        result.margin =
            result.top1_probability -
            result.top2_probability;
        const auto probability_threshold =
            config_.per_label_thresholds.at(
                result.top1_label);
        const auto margin_threshold =
            config_.per_label_margin_thresholds.at(
                result.top1_label);
        const auto ood_threshold =
            config_.per_label_ood_distance_thresholds.at(
                result.top1_label);
        result.ood_detected =
            result.ood_score > ood_threshold;
        result.accepted =
            result.top1_probability >=
                probability_threshold &&
            result.margin >= margin_threshold &&
            !result.ood_detected;
        return Result<BgeClassifierResult>::Success(
            std::move(result));
    }

    BgeClassifierConfig config() const override {
        return config_;
    }

private:
    std::shared_ptr<IRuntimeClock> clock_;
    BgeClassifierConfig config_;
};

}  // namespace

std::shared_ptr<IBgeIntentClassifier>
createDeterministicMockBgeClassifier(
    std::shared_ptr<IRuntimeClock> clock) {
    return std::make_shared<
        DeterministicMockBgeClassifier>(
        std::move(clock));
}

}  // namespace master_agent::intent
