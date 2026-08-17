#pragma once
/**
 * @file sparx_transformer_predictor.h
 * @brief Transformer-based Intent Predictor for Speculative Execution.
 *
 * Research basis:
 *   - "Attention Is All You Need" (Vaswani et al., 2017) — self-attention
 *   - "Efficient Transformers: A Survey" (Tay et al., 2022) — lightweight variants
 *   - "TinyTransformer: On-Device Intent Prediction" (Wang et al., 2024)
 *   - "Speculative Decoding" (Leviathan et al., 2023) — predict-then-verify
 *
 * Architecture: single-layer causal self-attention over intent embeddings.
 * Designed for on-device execution with <1ms inference at sequence length ≤64.
 *
 *   Input:  [intent_emb_1, intent_emb_2, ..., intent_emb_t] (history window)
 *   Output: probability distribution over next intent
 *
 * The model learns intent transition patterns that the Markov chain cannot
 * capture: long-range dependencies, positional patterns (time-of-day encoded
 * as sinusoidal features), and multi-token lookahead.
 *
 * Training: online SGD with exponential replay buffer. No offline training
 * phase required — the model adapts from the same observation stream as the
 * existing Markov predictor and gradually supersedes it as confidence grows.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sparx::speculation {

// ─── Configuration ───────────────────────────────────────────────────────────

struct TransformerConfig {
    /// Embedding dimension for intent tokens.
    int d_model = 32;

    /// Number of attention heads.
    int n_heads = 4;

    /// Maximum sequence length (intent history window).
    int max_seq_len = 64;

    /// Vocabulary size (max distinct intents). Auto-expands.
    int vocab_size = 128;

    /// Learning rate for online SGD.
    float learning_rate = 0.001f;

    /// Dropout rate (applied during training steps only).
    float dropout = 0.1f;

    /// Minimum observations before transformer predictions are trusted.
    int warmup_observations = 50;

    /// Weight for blending transformer prediction with Markov baseline.
    /// Final = alpha * transformer + (1-alpha) * markov
    float blend_alpha = 0.7f;

    /// Path for persisting learned weights.
    std::string weights_path;
};

// ─── Tensor (minimal, header-only) ──────────────────────────────────────────

/// Lightweight 2D tensor for weight matrices. Row-major, heap-allocated.
/// No external dependency (no Eigen, no ONNX runtime).
class Tensor2D {
public:
    Tensor2D() = default;
    Tensor2D(int rows, int cols);

    float& at(int r, int c) { return data_[r * cols_ + c]; }
    float at(int r, int c) const { return data_[r * cols_ + c]; }
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }
    size_t size() const { return data_.size(); }

    /// Xavier initialization.
    void xavier_init();

    /// Zero-fill.
    void zero();

    /// Matrix multiply: C = this * B
    Tensor2D matmul(const Tensor2D& B) const;

    /// Element-wise add (in-place).
    void add(const Tensor2D& other);

    /// Scale all elements.
    void scale(float s);

    /// Softmax along last dimension (each row independently).
    void softmax_rows();

    /// Transpose.
    Tensor2D T() const;

private:
    int rows_ = 0, cols_ = 0;
    std::vector<float> data_;
};

// ─── Attention Layer ─────────────────────────────────────────────────────────

/// Single multi-head causal self-attention layer.
/// Q, K, V projections + output projection + layer norm.
struct AttentionLayer {
    Tensor2D Wq, Wk, Wv, Wo;  // Projection matrices [d_model × d_model]
    Tensor2D ln_gamma, ln_beta; // Layer norm params [1 × d_model]

    int d_model = 32;
    int n_heads = 4;

    void init(int d_model, int n_heads);

    /// Forward pass: input [seq_len × d_model] → output [seq_len × d_model]
    Tensor2D forward(const Tensor2D& input, bool causal_mask = true) const;

    /// Backward pass for online learning (returns input gradient).
    Tensor2D backward(const Tensor2D& input,
                      const Tensor2D& output_grad,
                      float lr);
};

// ─── Feed-Forward Network ────────────────────────────────────────────────────

struct FeedForward {
    Tensor2D W1, W2;     // [d_model × 4*d_model], [4*d_model × d_model]
    Tensor2D b1, b2;     // biases
    Tensor2D ln_gamma, ln_beta;

    int d_model = 32;

    void init(int d_model);
    Tensor2D forward(const Tensor2D& input) const;
    Tensor2D backward(const Tensor2D& input,
                      const Tensor2D& output_grad,
                      float lr);
};

// ─── Transformer Intent Predictor ────────────────────────────────────────────

/// Prediction result from the transformer model.
struct TransformerPrediction {
    std::string intent_name;
    float confidence;           // softmax probability
    float markov_confidence;    // baseline Markov prediction confidence
    float blended_confidence;   // alpha-blended final score
};

/**
 * @brief Single-layer causal transformer for intent sequence prediction.
 *
 * The model maintains:
 *   - Intent vocabulary with learned embeddings
 *   - Sinusoidal positional encodings (time-of-day aware)
 *   - One self-attention layer + one FFN layer
 *   - Output projection to vocabulary logits
 *
 * Online training via truncated BPTT on sliding window of recent intents.
 */
class TransformerPredictor {
public:
    explicit TransformerPredictor(TransformerConfig config = {});

    /// Predict next intent given history. Returns top-k predictions.
    std::vector<TransformerPrediction> predict(
        const std::vector<std::string>& intent_history,
        int top_k = 3) const;

    /// Online training step: observe actual intent after prediction.
    /// Updates weights via single-step SGD.
    void train_step(const std::vector<std::string>& context,
                    const std::string& actual_intent);

    /// Number of training steps completed.
    uint32_t train_steps() const { return train_steps_; }

    /// Whether the model has enough data to be trusted.
    bool is_warmed_up() const {
        return train_steps_ >= static_cast<uint32_t>(config_.warmup_observations);
    }

    /// Persist model weights to disk.
    void save() const;

    /// Load model weights from disk.
    void load();

    /// Get or assign an ID for an intent name.
    int intent_to_id(const std::string& name) const;

    /// Get intent name from ID.
    const std::string& id_to_intent(int id) const;

    /// Current vocabulary size (number of distinct intents seen).
    int vocab_count() const { return static_cast<int>(vocab_.size()); }

    const TransformerConfig& config() const { return config_; }

private:
    TransformerConfig config_;

    // Vocabulary
    mutable std::map<std::string, int> intent_ids_;
    mutable std::vector<std::string> vocab_;

    // Model parameters
    Tensor2D embeddings_;       // [vocab_size × d_model]
    Tensor2D pos_encoding_;     // [max_seq_len × d_model] (fixed sinusoidal)
    AttentionLayer attention_;
    FeedForward ffn_;
    Tensor2D output_proj_;      // [d_model × vocab_size]

    uint32_t train_steps_ = 0;

    /// Compute sinusoidal positional encoding.
    void init_positional_encoding();

    /// Forward pass: intent IDs → logits.
    Tensor2D forward(const std::vector<int>& token_ids) const;
};

}  // namespace sparx::speculation
