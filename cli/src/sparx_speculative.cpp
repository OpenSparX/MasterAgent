/**
 * @file sparx_speculative.cpp
 * @brief Speculative Agent Execution — implementation.
 *
 * This implements the full speculation pipeline:
 *   Intent prediction → cache management → idle-time execution → hit delivery
 */

#include "sparx_speculative.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>

namespace sparx::speculation {

namespace {

int64_t nowUtcSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trigramKey(const std::string& a, const std::string& b) {
    return a + "|" + b;
}

}  // namespace

// ---------------------------------------------------------------------------
// LstmPredictor — Single-layer LSTM for intent sequence prediction
// ---------------------------------------------------------------------------

class LstmPredictor {
public:
    static constexpr size_t kEmbedDim = 64;
    static constexpr size_t kHiddenDim = 64;
    static constexpr size_t kConcatDim = kEmbedDim + kHiddenDim;  // 128

    LstmPredictor(float learning_rate = 0.01f, float grad_clip = 5.0f)
        : lr_(learning_rate), grad_clip_(grad_clip) {
        xavierInit();
    }

    /// Register an intent in the vocabulary, returns its index.
    size_t registerIntent(const std::string& intent) {
        auto it = intent_to_idx_.find(intent);
        if (it != intent_to_idx_.end()) return it->second;
        size_t idx = idx_to_intent_.size();
        intent_to_idx_[intent] = idx;
        idx_to_intent_.push_back(intent);
        // Grow output layer
        growOutputLayer(idx + 1);
        return idx;
    }

    size_t vocabSize() const { return idx_to_intent_.size(); }

    /// Run forward pass over a sequence of embeddings, return softmax distribution.
    std::vector<float> predict(const std::vector<EmbeddingVec>& seq) {
        if (seq.empty() || idx_to_intent_.empty()) return {};
        // Run LSTM over sequence
        resetState();
        for (const auto& x : seq) {
            forwardStep(x);
        }
        // Output projection: logits = W_out * h + b_out
        size_t num_intents = idx_to_intent_.size();
        std::vector<float> logits(num_intents, 0.0f);
        for (size_t i = 0; i < num_intents; ++i) {
            float sum = b_out_[i];
            for (size_t j = 0; j < kHiddenDim; ++j) {
                sum += W_out_[i * kHiddenDim + j] * h_[j];
            }
            logits[i] = sum;
        }
        return softmax(logits);
    }

    /// Online learning: single SGD step given sequence and target intent.
    void train(const std::vector<EmbeddingVec>& seq, size_t target_idx) {
        if (seq.empty() || target_idx >= idx_to_intent_.size()) return;

        // Forward pass (storing intermediates for BPTT)
        size_t T = seq.size();
        resetState();
        // Store states for backprop
        std::vector<std::array<float, kHiddenDim>> h_states(T + 1);
        std::vector<std::array<float, kHiddenDim>> c_states(T + 1);
        std::vector<std::array<float, kHiddenDim>> f_gates(T);
        std::vector<std::array<float, kHiddenDim>> i_gates(T);
        std::vector<std::array<float, kHiddenDim>> o_gates(T);
        std::vector<std::array<float, kHiddenDim>> c_cands(T);

        h_states[0].fill(0.0f);
        c_states[0].fill(0.0f);

        for (size_t t = 0; t < T; ++t) {
            forwardStepCached(seq[t], h_states[t], c_states[t],
                              h_states[t + 1], c_states[t + 1],
                              f_gates[t], i_gates[t], o_gates[t], c_cands[t]);
        }

        // Copy final hidden state
        for (size_t j = 0; j < kHiddenDim; ++j) h_[j] = h_states[T][j];

        // Output logits and softmax
        size_t num_intents = idx_to_intent_.size();
        std::vector<float> logits(num_intents, 0.0f);
        for (size_t i = 0; i < num_intents; ++i) {
            float sum = b_out_[i];
            for (size_t j = 0; j < kHiddenDim; ++j) {
                sum += W_out_[i * kHiddenDim + j] * h_states[T][j];
            }
            logits[i] = sum;
        }
        std::vector<float> probs = softmax(logits);

        // Cross-entropy gradient: dL/d_logits = probs - one_hot(target)
        std::vector<float> d_logits(num_intents);
        for (size_t i = 0; i < num_intents; ++i) {
            d_logits[i] = probs[i] - (i == target_idx ? 1.0f : 0.0f);
        }

        // Gradient for output layer
        std::vector<float> dW_out(num_intents * kHiddenDim, 0.0f);
        std::vector<float> db_out(num_intents, 0.0f);
        std::array<float, kHiddenDim> dh{};
        dh.fill(0.0f);

        for (size_t i = 0; i < num_intents; ++i) {
            db_out[i] = d_logits[i];
            for (size_t j = 0; j < kHiddenDim; ++j) {
                dW_out[i * kHiddenDim + j] = d_logits[i] * h_states[T][j];
                dh[j] += d_logits[i] * W_out_[i * kHiddenDim + j];
            }
        }

        // BPTT through the LSTM (truncated to sequence length)
        std::array<float, kHiddenDim> dc{};
        dc.fill(0.0f);

        // Accumulate LSTM weight gradients
        std::vector<float> dW_f(kHiddenDim * kConcatDim, 0.0f);
        std::vector<float> dW_i(kHiddenDim * kConcatDim, 0.0f);
        std::vector<float> dW_o(kHiddenDim * kConcatDim, 0.0f);
        std::vector<float> dW_c(kHiddenDim * kConcatDim, 0.0f);
        std::array<float, kHiddenDim> db_f{}, db_i{}, db_o{}, db_c{};
        db_f.fill(0.0f); db_i.fill(0.0f);
        db_o.fill(0.0f); db_c.fill(0.0f);

        for (int t = static_cast<int>(T) - 1; t >= 0; --t) {
            // dh and dc coming from above
            // o_gate contribution
            std::array<float, kHiddenDim> tanh_c;
            for (size_t j = 0; j < kHiddenDim; ++j) {
                tanh_c[j] = std::tanh(c_states[t + 1][j]);
            }

            std::array<float, kHiddenDim> do_gate, dc_total, df_gate, di_gate, dc_cand;

            for (size_t j = 0; j < kHiddenDim; ++j) {
                do_gate[j] = dh[j] * tanh_c[j] * o_gates[t][j] * (1.0f - o_gates[t][j]);
                dc_total[j] = dc[j] + dh[j] * o_gates[t][j] * (1.0f - tanh_c[j] * tanh_c[j]);
                df_gate[j] = dc_total[j] * c_states[t][j] * f_gates[t][j] * (1.0f - f_gates[t][j]);
                di_gate[j] = dc_total[j] * c_cands[t][j] * i_gates[t][j] * (1.0f - i_gates[t][j]);
                dc_cand[j] = dc_total[j] * i_gates[t][j] * (1.0f - c_cands[t][j] * c_cands[t][j]);
            }

            // Build concatenated input [h_{t-1}, x_t]
            std::array<float, kConcatDim> concat;
            for (size_t j = 0; j < kHiddenDim; ++j) concat[j] = h_states[t][j];
            for (size_t j = 0; j < kEmbedDim; ++j) concat[kHiddenDim + j] = seq[t][j];

            // Accumulate weight gradients
            for (size_t j = 0; j < kHiddenDim; ++j) {
                db_f[j] += df_gate[j];
                db_i[j] += di_gate[j];
                db_o[j] += do_gate[j];
                db_c[j] += dc_cand[j];
                for (size_t k = 0; k < kConcatDim; ++k) {
                    dW_f[j * kConcatDim + k] += df_gate[j] * concat[k];
                    dW_i[j * kConcatDim + k] += di_gate[j] * concat[k];
                    dW_o[j * kConcatDim + k] += do_gate[j] * concat[k];
                    dW_c[j * kConcatDim + k] += dc_cand[j] * concat[k];
                }
            }

            // Propagate gradients to h_{t-1} and dc for next iteration
            dh.fill(0.0f);
            for (size_t j = 0; j < kHiddenDim; ++j) {
                for (size_t k = 0; k < kHiddenDim; ++k) {
                    float grad_from_k = df_gate[k] * W_f_[k * kConcatDim + j]
                                      + di_gate[k] * W_i_[k * kConcatDim + j]
                                      + do_gate[k] * W_o_[k * kConcatDim + j]
                                      + dc_cand[k] * W_c_[k * kConcatDim + j];
                    dh[j] += grad_from_k;
                }
            }
            for (size_t j = 0; j < kHiddenDim; ++j) {
                dc[j] = dc_total[j] * f_gates[t][j];
            }
        }

        // Clip gradients (global norm)
        float grad_norm = computeGradNorm(dW_f, dW_i, dW_o, dW_c,
                                           db_f, db_i, db_o, db_c,
                                           dW_out, db_out);
        float scale = 1.0f;
        if (grad_norm > grad_clip_) {
            scale = grad_clip_ / grad_norm;
        }

        // Apply SGD updates
        applyGradients(W_f_, dW_f, scale);
        applyGradients(W_i_, dW_i, scale);
        applyGradients(W_o_, dW_o, scale);
        applyGradients(W_c_, dW_c, scale);
        applyBiasGradients(b_f_, db_f, scale);
        applyBiasGradients(b_i_, db_i, scale);
        applyBiasGradients(b_o_, db_o, scale);
        applyBiasGradients(b_c_, db_c, scale);
        applyGradients(W_out_, dW_out, scale);
        applyVecGradients(b_out_, db_out, scale);
    }

    /// Save weights to binary file.
    bool save(const std::string& path) const {
        auto parent = std::filesystem::path(path).parent_path();
        std::filesystem::create_directories(parent);

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        // Header: magic + version + vocab size
        uint32_t magic = 0x4C53544D;  // "LSTM"
        uint32_t version = 1;
        uint32_t vocab_size = static_cast<uint32_t>(idx_to_intent_.size());
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&vocab_size), 4);

        // Write vocab (length-prefixed strings)
        for (const auto& name : idx_to_intent_) {
            uint16_t len = static_cast<uint16_t>(name.size());
            out.write(reinterpret_cast<const char*>(&len), 2);
            out.write(name.data(), len);
        }

        // Write LSTM weights (4 gates)
        out.write(reinterpret_cast<const char*>(W_f_.data()),
                  W_f_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_i_.data()),
                  W_i_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_o_.data()),
                  W_o_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_c_.data()),
                  W_c_.size() * sizeof(float));

        // Biases
        out.write(reinterpret_cast<const char*>(b_f_.data()), kHiddenDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_i_.data()), kHiddenDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_o_.data()), kHiddenDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_c_.data()), kHiddenDim * sizeof(float));

        // Output layer
        out.write(reinterpret_cast<const char*>(W_out_.data()),
                  W_out_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_out_.data()),
                  b_out_.size() * sizeof(float));

        return out.good();
    }

    /// Load weights from binary file.
    bool load(const std::string& path) {
        if (!std::filesystem::exists(path)) return false;

        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        uint32_t magic = 0, version = 0, vocab_size = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&vocab_size), 4);

        if (magic != 0x4C53544D || version != 1) return false;

        // Read vocab
        intent_to_idx_.clear();
        idx_to_intent_.clear();
        for (uint32_t i = 0; i < vocab_size; ++i) {
            uint16_t len = 0;
            in.read(reinterpret_cast<char*>(&len), 2);
            std::string name(len, '\0');
            in.read(name.data(), len);
            intent_to_idx_[name] = i;
            idx_to_intent_.push_back(name);
        }

        // Read LSTM weights
        in.read(reinterpret_cast<char*>(W_f_.data()), W_f_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(W_i_.data()), W_i_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(W_o_.data()), W_o_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(W_c_.data()), W_c_.size() * sizeof(float));

        in.read(reinterpret_cast<char*>(b_f_.data()), kHiddenDim * sizeof(float));
        in.read(reinterpret_cast<char*>(b_i_.data()), kHiddenDim * sizeof(float));
        in.read(reinterpret_cast<char*>(b_o_.data()), kHiddenDim * sizeof(float));
        in.read(reinterpret_cast<char*>(b_c_.data()), kHiddenDim * sizeof(float));

        // Read output layer
        growOutputLayer(vocab_size);
        in.read(reinterpret_cast<char*>(W_out_.data()),
                W_out_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(b_out_.data()),
                b_out_.size() * sizeof(float));

        return in.good();
    }

    const std::map<std::string, size_t>& intentIndex() const { return intent_to_idx_; }
    const std::string& intentName(size_t idx) const { return idx_to_intent_[idx]; }

private:
    float lr_;
    float grad_clip_;

    // LSTM gate weights: each kHiddenDim x kConcatDim
    std::vector<float> W_f_;  // forget gate
    std::vector<float> W_i_;  // input gate
    std::vector<float> W_o_;  // output gate
    std::vector<float> W_c_;  // candidate cell

    // LSTM gate biases: each kHiddenDim
    std::array<float, kHiddenDim> b_f_;
    std::array<float, kHiddenDim> b_i_;
    std::array<float, kHiddenDim> b_o_;
    std::array<float, kHiddenDim> b_c_;

    // Output projection: num_intents x kHiddenDim + bias
    std::vector<float> W_out_;
    std::vector<float> b_out_;

    // Hidden state and cell state
    std::array<float, kHiddenDim> h_;
    std::array<float, kHiddenDim> c_;

    // Vocab mapping
    std::map<std::string, size_t> intent_to_idx_;
    std::vector<std::string> idx_to_intent_;

    void xavierInit() {
        const size_t gate_size = kHiddenDim * kConcatDim;
        W_f_.resize(gate_size);
        W_i_.resize(gate_size);
        W_o_.resize(gate_size);
        W_c_.resize(gate_size);

        // Xavier uniform: U(-sqrt(6/(fan_in+fan_out)), sqrt(6/(fan_in+fan_out)))
        float limit = std::sqrt(6.0f / static_cast<float>(kConcatDim + kHiddenDim));
        std::mt19937 rng(42);  // deterministic seed for reproducibility
        std::uniform_real_distribution<float> dist(-limit, limit);

        for (auto& w : W_f_) w = dist(rng);
        for (auto& w : W_i_) w = dist(rng);
        for (auto& w : W_o_) w = dist(rng);
        for (auto& w : W_c_) w = dist(rng);

        // Forget gate bias = 1.0 (crucial for learning long-term dependencies)
        b_f_.fill(1.0f);
        b_i_.fill(0.0f);
        b_o_.fill(0.0f);
        b_c_.fill(0.0f);

        h_.fill(0.0f);
        c_.fill(0.0f);
    }

    void growOutputLayer(size_t new_size) {
        size_t old_size = b_out_.size();
        if (new_size <= old_size) return;

        // Preserve existing weights, Xavier-init new ones
        float limit = std::sqrt(6.0f / static_cast<float>(kHiddenDim + new_size));
        std::mt19937 rng(static_cast<uint32_t>(old_size * 7 + 13));
        std::uniform_real_distribution<float> dist(-limit, limit);

        std::vector<float> new_W_out(new_size * kHiddenDim, 0.0f);
        // Copy old weights
        for (size_t i = 0; i < old_size; ++i) {
            for (size_t j = 0; j < kHiddenDim; ++j) {
                new_W_out[i * kHiddenDim + j] = W_out_[i * kHiddenDim + j];
            }
        }
        // Init new rows
        for (size_t i = old_size; i < new_size; ++i) {
            for (size_t j = 0; j < kHiddenDim; ++j) {
                new_W_out[i * kHiddenDim + j] = dist(rng);
            }
        }
        W_out_ = std::move(new_W_out);

        b_out_.resize(new_size, 0.0f);
    }

    void resetState() {
        h_.fill(0.0f);
        c_.fill(0.0f);
    }

    static float sigmoid(float x) {
        return 1.0f / (1.0f + std::exp(-x));
    }

    void forwardStep(const EmbeddingVec& x) {
        // Concatenate [h, x]
        std::array<float, kConcatDim> concat;
        for (size_t j = 0; j < kHiddenDim; ++j) concat[j] = h_[j];
        for (size_t j = 0; j < kEmbedDim; ++j) concat[kHiddenDim + j] = x[j];

        std::array<float, kHiddenDim> f, i, o, c_cand;

        for (size_t j = 0; j < kHiddenDim; ++j) {
            float sum_f = b_f_[j], sum_i = b_i_[j], sum_o = b_o_[j], sum_c = b_c_[j];
            for (size_t k = 0; k < kConcatDim; ++k) {
                sum_f += W_f_[j * kConcatDim + k] * concat[k];
                sum_i += W_i_[j * kConcatDim + k] * concat[k];
                sum_o += W_o_[j * kConcatDim + k] * concat[k];
                sum_c += W_c_[j * kConcatDim + k] * concat[k];
            }
            f[j] = sigmoid(sum_f);
            i[j] = sigmoid(sum_i);
            o[j] = sigmoid(sum_o);
            c_cand[j] = std::tanh(sum_c);
        }

        for (size_t j = 0; j < kHiddenDim; ++j) {
            c_[j] = f[j] * c_[j] + i[j] * c_cand[j];
            h_[j] = o[j] * std::tanh(c_[j]);
        }
    }

    void forwardStepCached(
        const EmbeddingVec& x,
        const std::array<float, kHiddenDim>& h_prev,
        const std::array<float, kHiddenDim>& c_prev,
        std::array<float, kHiddenDim>& h_next,
        std::array<float, kHiddenDim>& c_next,
        std::array<float, kHiddenDim>& f_out,
        std::array<float, kHiddenDim>& i_out,
        std::array<float, kHiddenDim>& o_out,
        std::array<float, kHiddenDim>& c_cand_out) {

        std::array<float, kConcatDim> concat;
        for (size_t j = 0; j < kHiddenDim; ++j) concat[j] = h_prev[j];
        for (size_t j = 0; j < kEmbedDim; ++j) concat[kHiddenDim + j] = x[j];

        for (size_t j = 0; j < kHiddenDim; ++j) {
            float sum_f = b_f_[j], sum_i = b_i_[j], sum_o = b_o_[j], sum_c = b_c_[j];
            for (size_t k = 0; k < kConcatDim; ++k) {
                sum_f += W_f_[j * kConcatDim + k] * concat[k];
                sum_i += W_i_[j * kConcatDim + k] * concat[k];
                sum_o += W_o_[j * kConcatDim + k] * concat[k];
                sum_c += W_c_[j * kConcatDim + k] * concat[k];
            }
            f_out[j] = sigmoid(sum_f);
            i_out[j] = sigmoid(sum_i);
            o_out[j] = sigmoid(sum_o);
            c_cand_out[j] = std::tanh(sum_c);
        }

        for (size_t j = 0; j < kHiddenDim; ++j) {
            c_next[j] = f_out[j] * c_prev[j] + i_out[j] * c_cand_out[j];
            h_next[j] = o_out[j] * std::tanh(c_next[j]);
        }
    }

    static std::vector<float> softmax(const std::vector<float>& logits) {
        std::vector<float> result(logits.size());
        float max_val = *std::max_element(logits.begin(), logits.end());
        float sum = 0.0f;
        for (size_t i = 0; i < logits.size(); ++i) {
            result[i] = std::exp(logits[i] - max_val);
            sum += result[i];
        }
        if (sum > 0.0f) {
            for (auto& v : result) v /= sum;
        }
        return result;
    }

    float computeGradNorm(
        const std::vector<float>& dW_f, const std::vector<float>& dW_i,
        const std::vector<float>& dW_o, const std::vector<float>& dW_c,
        const std::array<float, kHiddenDim>& db_f,
        const std::array<float, kHiddenDim>& db_i,
        const std::array<float, kHiddenDim>& db_o,
        const std::array<float, kHiddenDim>& db_c,
        const std::vector<float>& dW_out,
        const std::vector<float>& db_out) const {

        float norm_sq = 0.0f;
        for (float v : dW_f) norm_sq += v * v;
        for (float v : dW_i) norm_sq += v * v;
        for (float v : dW_o) norm_sq += v * v;
        for (float v : dW_c) norm_sq += v * v;
        for (float v : db_f) norm_sq += v * v;
        for (float v : db_i) norm_sq += v * v;
        for (float v : db_o) norm_sq += v * v;
        for (float v : db_c) norm_sq += v * v;
        for (float v : dW_out) norm_sq += v * v;
        for (float v : db_out) norm_sq += v * v;
        return std::sqrt(norm_sq);
    }

    void applyGradients(std::vector<float>& W, const std::vector<float>& dW, float scale) {
        for (size_t i = 0; i < W.size(); ++i) {
            W[i] -= lr_ * scale * dW[i];
        }
    }

    void applyBiasGradients(std::array<float, kHiddenDim>& b,
                            const std::array<float, kHiddenDim>& db, float scale) {
        for (size_t i = 0; i < kHiddenDim; ++i) {
            b[i] -= lr_ * scale * db[i];
        }
    }

    void applyVecGradients(std::vector<float>& b, const std::vector<float>& db, float scale) {
        for (size_t i = 0; i < b.size(); ++i) {
            b[i] -= lr_ * scale * db[i];
        }
    }
};

// ---------------------------------------------------------------------------
// IntentPredictor
// ---------------------------------------------------------------------------

IntentPredictor::IntentPredictor(PredictionConfig config)
    : config_(std::move(config)),
      lstm_(std::make_unique<LstmPredictor>(config_.lstm_learning_rate,
                                             config_.lstm_grad_clip)) {
    // Attempt to load persisted LSTM weights
    std::string lstm_path = config_.lstm_weights_path;
    if (lstm_path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            lstm_path = std::string(home) + "/.sparx/speculation/lstm.bin";
        }
    }
    if (!lstm_path.empty()) {
        lstm_->load(lstm_path);
    }
}

IntentPredictor::~IntentPredictor() = default;

void IntentPredictor::observe(const IntentRecord& record) {
    ++observation_count_;

    // Update canonical phrasing (most recent wins)
    canonical_phrasing_[record.intent_name] = record.raw_input;

    // Register intent in LSTM vocab
    lstm_->registerIntent(record.intent_name);

    // Exponential decay factor: λ = 0.1, age in days
    // Older observations count less: weight = exp(-0.1 * age_days)
    const double decay_lambda = 0.1;
    int64_t now = record.timestamp_utc;

    // Update bigram transitions with decay
    if (!recent_history_.empty()) {
        const auto& prev = recent_history_.back().intent_name;

        // Compute age-weighted increment
        int64_t age_seconds = now - recent_history_.back().timestamp_utc;
        double age_days = static_cast<double>(age_seconds) / 86400.0;
        float weight = static_cast<float>(std::exp(-decay_lambda * age_days));

        // Store as float counts (fractional weights)
        transitions_weighted_[prev][record.intent_name] += weight;

        // Temporal bigram with decay
        temporal_transitions_weighted_[record.hour_of_day][prev][record.intent_name] += weight;

        // Trigram with decay
        if (recent_history_.size() >= 2) {
            auto it = recent_history_.rbegin();
            const auto& prev1 = it->intent_name;
            ++it;
            const auto& prev2 = it->intent_name;
            trigrams_weighted_[trigramKey(prev2, prev1)][record.intent_name] += weight;
        }
    }

    // Level 3 LSTM online learning: train on recent history sequence
    if (recent_history_.size() >= config_.lstm_min_history) {
        // Build embedding sequence from recent history
        std::vector<EmbeddingVec> seq;
        seq.reserve(recent_history_.size());
        for (const auto& r : recent_history_) {
            seq.push_back(intent_embedder_.embed(r.intent_name));
        }
        // Target is the current intent
        size_t target_idx = lstm_->registerIntent(record.intent_name);
        lstm_->train(seq, target_idx);
    }

    // Maintain history window
    recent_history_.push_back(record);
    while (recent_history_.size() > config_.history_window) {
        recent_history_.pop_front();
    }
}

std::vector<IntentPrediction> IntentPredictor::predict(
    std::uint32_t top_k) const {
    if (!isWarmedUp() || recent_history_.empty()) return {};

    const auto& current = recent_history_.back();
    const auto current_intent = current.intent_name;
    auto now_hour = static_cast<uint8_t>(
        std::chrono::duration_cast<std::chrono::hours>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 24);

    // Collect candidates with scores from n-gram models (Level 1 & 2)
    std::map<std::string, float> ngram_scores;

    // Level 1: Bigram P(B|A) with exponential decay
    auto bigram_it = transitions_weighted_.find(current_intent);
    if (bigram_it != transitions_weighted_.end()) {
        float total = 0.0f;
        for (const auto& [_, weight] : bigram_it->second) total += weight;
        if (total > 0.0f) {
            for (const auto& [intent, weight] : bigram_it->second) {
                float p = weight / total;
                ngram_scores[intent] += p * (1.0f - config_.temporal_weight);
            }
        }
    }

    // Level 2: Temporal bigram P(B|A, hour) with decay
    auto temp_it = temporal_transitions_weighted_.find(now_hour);
    if (temp_it != temporal_transitions_weighted_.end()) {
        auto temp_bigram = temp_it->second.find(current_intent);
        if (temp_bigram != temp_it->second.end()) {
            float total = 0.0f;
            for (const auto& [_, weight] : temp_bigram->second) total += weight;
            if (total > 0.0f) {
                for (const auto& [intent, weight] : temp_bigram->second) {
                    float p = weight / total;
                    ngram_scores[intent] += p * config_.temporal_weight;
                }
            }
        }
    }

    // Trigram boost (enhances Level 1&2 confidence, not a separate level)
    if (recent_history_.size() >= 2) {
        auto it = recent_history_.rbegin();
        const auto& prev1 = it->intent_name;
        ++it;
        const auto& prev2 = it->intent_name;
        auto tri_it = trigrams_weighted_.find(trigramKey(prev2, prev1));
        if (tri_it != trigrams_weighted_.end()) {
            float total = 0.0f;
            for (const auto& [_, weight] : tri_it->second) total += weight;
            if (total > 0.0f) {
                for (const auto& [intent, weight] : tri_it->second) {
                    float p = weight / total;
                    // Trigram acts as a confidence multiplier
                    ngram_scores[intent] *= (1.0f + p * 0.5f);
                }
            }
        }
    }

    // Level 3: LSTM sequence prediction (when sufficient history)
    std::map<std::string, float> lstm_scores;
    bool use_lstm = (recent_history_.size() >= config_.lstm_min_history &&
                     lstm_->vocabSize() > 0);

    if (use_lstm) {
        // Build embedding sequence from recent history
        std::vector<EmbeddingVec> seq;
        seq.reserve(recent_history_.size());
        for (const auto& r : recent_history_) {
            seq.push_back(intent_embedder_.embed(r.intent_name));
        }
        auto probs = lstm_->predict(seq);
        for (size_t i = 0; i < probs.size(); ++i) {
            if (probs[i] > 0.01f) {  // skip negligible probabilities
                lstm_scores[lstm_->intentName(i)] = probs[i];
            }
        }
    }

    // Blend Level 1&2 (n-gram) and Level 3 (LSTM) via weighted ensemble
    std::map<std::string, float> scores;
    float w_ngram = use_lstm ? config_.ngram_weight : 1.0f;
    float w_lstm = use_lstm ? config_.lstm_weight : 0.0f;

    // Merge all candidate intents
    for (const auto& [intent, score] : ngram_scores) {
        scores[intent] += score * w_ngram;
    }
    for (const auto& [intent, score] : lstm_scores) {
        scores[intent] += score * w_lstm;
    }

    // Sort by score and return top-k above threshold
    std::vector<std::pair<std::string, float>> sorted(
        scores.begin(), scores.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<IntentPrediction> results;
    for (const auto& [intent, score] : sorted) {
        if (results.size() >= top_k) break;
        if (score < config_.min_confidence) break;

        IntentPrediction pred;
        pred.predicted_intent = intent;
        pred.confidence = std::min(score, 1.0f);
        // Use canonical phrasing for pre-computation
        auto phrasing_it = canonical_phrasing_.find(intent);
        pred.predicted_input = (phrasing_it != canonical_phrasing_.end())
            ? phrasing_it->second : intent;
        pred.rationale = use_lstm
            ? "bigram+temporal+trigram+lstm ensemble"
            : "bigram+temporal+trigram ensemble";
        results.push_back(std::move(pred));
    }
    return results;
}

bool IntentPredictor::isWarmedUp() const {
    return observation_count_ >= config_.cold_start_threshold;
}

float IntentPredictor::recentHitRate(std::uint32_t window) const {
    if (prediction_hits_.empty()) return 0.0f;
    uint32_t count = std::min(window,
        static_cast<uint32_t>(prediction_hits_.size()));
    uint32_t hits = 0;
    auto it = prediction_hits_.rbegin();
    for (uint32_t i = 0; i < count && it != prediction_hits_.rend(); ++i, ++it) {
        if (*it) ++hits;
    }
    return static_cast<float>(hits) / static_cast<float>(count);
}

void IntentPredictor::save() const {
    // Serialize to JSON for persistence
    // In production, this would be encrypted with the device key
    auto path = config_.history_path;
    if (path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            path = std::string(home) + "/.sparx/speculation/model.json";
        } else return;
    }
    // Create parent dirs
    auto parent = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(parent);

    std::ofstream out(path);
    out << "{\n\"observation_count\":" << observation_count_ << ",\n";
    out << "\"transitions\":{\n";
    bool first_outer = true;
    for (const auto& [from, tos] : transitions_weighted_) {
        if (!first_outer) out << ",\n";
        first_outer = false;
        out << "\"" << from << "\":{";
        bool first_inner = true;
        for (const auto& [to, weight] : tos) {
            if (!first_inner) out << ",";
            first_inner = false;
            out << "\"" << to << "\":" << weight;
        }
        out << "}";
    }
    out << "\n}}\n";

    // Save LSTM weights to binary file
    std::string lstm_path = config_.lstm_weights_path;
    if (lstm_path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            lstm_path = std::string(home) + "/.sparx/speculation/lstm.bin";
        }
    }
    if (!lstm_path.empty() && lstm_) {
        lstm_->save(lstm_path);
    }
}

void IntentPredictor::load() {
    auto path = config_.history_path;
    if (path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            path = std::string(home) + "/.sparx/speculation/model.json";
        } else return;
    }
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path);
    if (!in) return;

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // Parse observation_count
    auto obs_pos = content.find("\"observation_count\":");
    if (obs_pos != std::string::npos) {
        auto val_start = obs_pos + 20;  // length of "observation_count":
        auto val_end = content.find_first_of(",}\n", val_start);
        if (val_end != std::string::npos) {
            try {
                observation_count_ = static_cast<uint32_t>(
                    std::stoul(content.substr(val_start, val_end - val_start)));
            } catch (...) { /* corrupt file, start fresh */ }
        }
    }

    // Parse transitions: {"from":{"to":count,...},...}
    auto trans_pos = content.find("\"transitions\":{");
    if (trans_pos == std::string::npos) return;
    auto block_start = trans_pos + 15;  // after "transitions":{

    // Simple state-machine parser for the nested object
    transitions_weighted_.clear();
    size_t pos = block_start;
    while (pos < content.size()) {
        // Find next outer key (a "from" intent)
        auto key_start = content.find('"', pos);
        if (key_start == std::string::npos || content[key_start - 1] == '}') break;
        auto key_end = content.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        std::string from_key = content.substr(key_start + 1, key_end - key_start - 1);

        // Find the inner object start
        auto inner_start = content.find('{', key_end);
        if (inner_start == std::string::npos) break;
        auto inner_end = content.find('}', inner_start);
        if (inner_end == std::string::npos) break;

        // Parse inner key:value pairs
        std::string inner = content.substr(inner_start + 1, inner_end - inner_start - 1);
        size_t ipos = 0;
        while (ipos < inner.size()) {
            auto ik_start = inner.find('"', ipos);
            if (ik_start == std::string::npos) break;
            auto ik_end = inner.find('"', ik_start + 1);
            if (ik_end == std::string::npos) break;
            std::string to_key = inner.substr(ik_start + 1, ik_end - ik_start - 1);

            auto colon = inner.find(':', ik_end);
            if (colon == std::string::npos) break;
            auto vend = inner.find_first_of(",}", colon + 1);
            if (vend == std::string::npos) vend = inner.size();
            std::string val_str = inner.substr(colon + 1, vend - colon - 1);

            try {
                float weight = std::stof(val_str);
                transitions_weighted_[from_key][to_key] = weight;
            } catch (...) { /* skip malformed entry */ }

            ipos = vend + 1;
        }

        pos = inner_end + 1;
        // Skip comma between outer entries
        while (pos < content.size() && (content[pos] == ',' || content[pos] == '\n'))
            ++pos;
        // Check if we hit the closing brace of transitions
        if (pos < content.size() && content[pos] == '}') break;
    }
}

// ---------------------------------------------------------------------------
// EmbeddingIndex (SimHash over character trigrams)
// ---------------------------------------------------------------------------

std::string EmbeddingIndex::normalizeForEmbedding(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool prev_space = false;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            prev_space = false;
        } else if (!prev_space && !out.empty()) {
            out += ' ';
            prev_space = true;
        }
    }
    // Trim trailing space
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

EmbeddingVec EmbeddingIndex::hashTrigram(const std::string& trigram) {
    // Deterministic random projection using FNV-1a hash with dimension rotation.
    // Each trigram maps to a consistent direction in embedding space.
    EmbeddingVec proj{};

    // FNV-1a hash of the trigram
    uint64_t hash = 14695981039346656037ULL;
    for (char c : trigram) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }

    // Generate kEmbeddingDim pseudo-random projections from this hash
    // using a simple xorshift64 PRNG seeded by the trigram hash
    uint64_t state = hash;
    for (size_t d = 0; d < kEmbeddingDim; ++d) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        // Convert to float in [-1, 1]
        proj[d] = static_cast<float>(static_cast<int64_t>(state)) /
                  static_cast<float>(INT64_MAX);
    }
    return proj;
}

EmbeddingVec EmbeddingIndex::embed(const std::string& text) const {
    std::string norm = normalizeForEmbedding(text);
    EmbeddingVec vec{};

    if (norm.size() < 3) {
        // Too short for trigrams — use character-level
        for (size_t i = 0; i < norm.size(); ++i) {
            std::string uni(1, norm[i]);
            auto proj = hashTrigram(uni);
            for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] += proj[d];
        }
    } else {
        // Extract character trigrams and accumulate their projections
        for (size_t i = 0; i + 3 <= norm.size(); ++i) {
            std::string tri = norm.substr(i, 3);
            auto proj = hashTrigram(tri);
            for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] += proj[d];
        }
    }

    // L2 normalize
    float norm_sq = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) norm_sq += vec[d] * vec[d];
    if (norm_sq > 1e-9f) {
        float inv_norm = 1.0f / std::sqrt(norm_sq);
        for (size_t d = 0; d < kEmbeddingDim; ++d) vec[d] *= inv_norm;
    }
    return vec;
}

float EmbeddingIndex::cosineSimilarity(const EmbeddingVec& a, const EmbeddingVec& b) {
    // Both vectors are L2-normalized, so dot product = cosine similarity
    float dot = 0.0f;
    for (size_t d = 0; d < kEmbeddingDim; ++d) dot += a[d] * b[d];
    return dot;
}

void EmbeddingIndex::insert(const std::string& cache_key, const EmbeddingVec& vec) {
    // Replace if exists
    for (auto& entry : entries_) {
        if (entry.cache_key == cache_key) {
            entry.vec = vec;
            return;
        }
    }
    entries_.push_back({cache_key, vec});
}

std::optional<EmbeddingIndex::NearestResult> EmbeddingIndex::findNearest(
    const EmbeddingVec& query, float threshold) const {

    NearestResult best;
    for (const auto& entry : entries_) {
        float sim = cosineSimilarity(query, entry.vec);
        if (sim > best.similarity) {
            best.cache_key = entry.cache_key;
            best.similarity = sim;
        }
    }
    if (best.similarity >= threshold) return best;
    return std::nullopt;
}

void EmbeddingIndex::remove(const std::string& cache_key) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const IndexEntry& e) { return e.cache_key == cache_key; }),
        entries_.end());
}

void EmbeddingIndex::clear() {
    entries_.clear();
}

// ---------------------------------------------------------------------------
// SpeculationCache
// ---------------------------------------------------------------------------

SpeculationCache::SpeculationCache(CacheConfig config)
    : config_(std::move(config)) {}

void SpeculationCache::put(SpeculativeResult result) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check memory limit
    std::size_t entry_size = result.raw_output.size() + result.cache_key.size() + 128;
    while (stats_.current_memory + entry_size > config_.max_memory_bytes &&
           !entries_.empty()) {
        stats_.current_memory -= entries_.front().raw_output.size() +
                                  entries_.front().cache_key.size() + 128;
        entries_.pop_front();
        stats_.evictions++;
    }

    // Check entry limit
    while (entries_.size() >= config_.max_entries && !entries_.empty()) {
        stats_.current_memory -= entries_.front().raw_output.size() +
                                  entries_.front().cache_key.size() + 128;
        entries_.pop_front();
        stats_.evictions++;
    }

    stats_.current_memory += entry_size;
    stats_.current_entries = static_cast<uint32_t>(entries_.size()) + 1;

    // Index embedding for similarity search
    if (config_.enable_similarity_match) {
        // Extract the input text from the cache_key (format: "intent:hash")
        // We store the raw_output's first line as proxy for content similarity,
        // but more importantly we need the original input text.
        // The cache_key itself is stable enough for embedding index linkage.
        auto vec = embedding_index_.embed(result.cache_key);
        embedding_index_.insert(result.cache_key, vec);
    }

    entries_.push_back(std::move(result));
}

std::optional<SpeculativeResult> SpeculationCache::get(
    const std::string& intent,
    const std::string& input_hash,
    const std::string& context_hash) const {

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = nowUtcSeconds();

    // Phase 1: Exact match (O(n) scan, most specific)
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->intent_name != intent) continue;

        // Check TTL
        if ((now - it->computed_at_utc) > it->ttl_seconds) {
            stats_.expirations++;
            continue;
        }

        // Check context freshness
        if (config_.invalidate_on_context_change &&
            it->context_hash != context_hash) {
            continue;
        }

        // Exact input match
        if (it->cache_key == intent + ":" + input_hash) {
            stats_.hits++;
            return *it;
        }
    }

    // Phase 2: Embedding similarity fallback (fuzzy match)
    if (config_.enable_similarity_match && !entries_.empty()) {
        std::string query_key = intent + ":" + input_hash;
        auto query_vec = embedding_index_.embed(query_key);
        auto nearest = embedding_index_.findNearest(
            query_vec, config_.similarity_threshold);

        if (nearest) {
            // Found a similar entry — validate it's still fresh
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
                if (it->cache_key != nearest->cache_key) continue;
                if (it->intent_name != intent) continue;
                if ((now - it->computed_at_utc) > it->ttl_seconds) continue;
                if (config_.invalidate_on_context_change &&
                    it->context_hash != context_hash) continue;

                // Similarity hit — mark as such and return
                stats_.similarity_hits++;
                stats_.hits++;
                SpeculativeResult result = *it;
                result.prediction_confidence *= nearest->similarity;  // discount
                return result;
            }
        }
    }

    stats_.misses++;
    return std::nullopt;
}

void SpeculationCache::invalidateAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    embedding_index_.clear();
    stats_.current_entries = 0;
    stats_.current_memory = 0;
}

void SpeculationCache::invalidate(const std::string& intent) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const SpeculativeResult& r) {
                return r.intent_name == intent;
            }),
        entries_.end());
    stats_.current_entries = static_cast<uint32_t>(entries_.size());
}

SpeculationCache::Stats SpeculationCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// SpeculativeExecutor — Async Worker with Idle Sensing
// ---------------------------------------------------------------------------

SpeculativeExecutor::SpeculativeExecutor(
    IntentPredictor& predictor,
    SpeculationCache& cache,
    ExecutorConfig config)
    : predictor_(predictor), cache_(cache), config_(std::move(config)) {
    // Start background worker thread
    worker_thread_ = std::thread(&SpeculativeExecutor::workerLoop, this);
}

SpeculativeExecutor::~SpeculativeExecutor() {
    shutdown_.store(true);
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void SpeculativeExecutor::afterTurn(
    const IntentRecord& record,
    const std::string& context_hash,
    InferenceCallback infer_fn) {

    // Record observation for the predictor
    predictor_.observe(record);

    if (!predictor_.isWarmedUp()) return;

    // Get predictions
    auto predictions = predictor_.predict(config_.max_speculation_tokens > 0 ? 3 : 0);
    if (predictions.empty()) return;

    // Enqueue speculation tasks for the background worker
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& pred : predictions) {
            if (pred.confidence < 0.6f) break;

            // Skip if already cached
            auto input_hash = normalizeForCacheKey(pred.predicted_input);
            auto existing = cache_.get(pred.predicted_intent, input_hash,
                                        context_hash);
            if (existing) continue;

            SpeculationTask task;
            task.predicted_input = pred.predicted_input;
            task.intent_name = pred.predicted_intent;
            task.confidence = pred.confidence;
            task.context_hash = context_hash;
            task.infer_fn = infer_fn;
            task_queue_.push_back(std::move(task));
        }
    }
    queue_cv_.notify_one();

    // Save predictor state periodically
    if (predictor_.observationCount() % 10 == 0) {
        predictor_.save();
    }
}

void SpeculativeExecutor::workerLoop() {
    while (!shutdown_.load()) {
        SpeculationTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return !task_queue_.empty() || shutdown_.load();
            });
            if (shutdown_.load()) break;
            if (task_queue_.empty()) continue;

            // Check system idle before dequeuing
            if (!isSystemIdle()) {
                // Not idle — wait and retry
                continue;
            }

            task = std::move(task_queue_.front());
            task_queue_.pop_front();
        }

        // Check preemption before executing
        if (preempt_requested_.load()) {
            preempt_requested_.store(false);
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.preempted++;
            continue;
        }

        executeTask(task);
    }
}

void SpeculativeExecutor::executeTask(const SpeculationTask& task) {
    active_speculation_.store(true);

    // Run inference at speculation priority
    auto result = task.infer_fn(task.predicted_input,
                                config_.max_speculation_tokens);

    active_speculation_.store(false);

    // Check if preempted during execution
    if (preempt_requested_.load()) {
        preempt_requested_.store(false);
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.preempted++;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.total_speculations++;
    }

    if (!result) return;

    // Cache the result
    auto input_hash = normalizeForCacheKey(task.predicted_input);
    SpeculativeResult cached;
    cached.cache_key = task.intent_name + ":" + input_hash;
    cached.raw_output = *result;
    cached.intent_name = task.intent_name;
    cached.prediction_confidence = task.confidence;
    cached.computed_at_utc = nowUtcSeconds();
    cached.ttl_seconds = 300;  // 5 minute TTL
    cached.context_hash = task.context_hash;
    cached.model_id = "local";
    cached.token_count = static_cast<uint32_t>(result->size() / 4);
    cache_.put(std::move(cached));
}

bool SpeculativeExecutor::isSystemIdle() const {
    float load = querySystemLoad();
    float battery = queryBatteryLevel();
    bool charging = queryIsCharging();

    switch (config_.priority) {
        case SpeculationPriority::IdleOnly:
            // Only speculate when load < 5% and (charging or battery > 50%)
            return load < 0.05f && (charging || battery > 0.5f);
        case SpeculationPriority::LowLoad:
            // Speculate when load < 20% and battery > 20%
            return load < 0.20f && battery > 0.2f;
        case SpeculationPriority::MediumLoad:
            // Speculate when load < 50% (aggressive)
            return load < 0.50f;
    }
    return false;
}

float SpeculativeExecutor::querySystemLoad() {
    // Platform-specific CPU/NPU load query
#ifdef __APPLE__
    // macOS: use getloadavg
    double loadavg[1] = {0.0};
    if (getloadavg(loadavg, 1) == 1) {
        // Normalize by number of CPUs (rough approximation)
        // loadavg of 1.0 on 8-core = 12.5% utilization
        return static_cast<float>(loadavg[0] / 8.0);
    }
    return 0.1f;  // fallback: assume light load
#elif defined(__linux__)
    // Linux: read /proc/stat for CPU utilization
    // Simplified: read loadavg
    double loadavg[1] = {0.0};
    if (getloadavg(loadavg, 1) == 1) {
        return static_cast<float>(loadavg[0] / 4.0);
    }
    return 0.1f;
#else
    return 0.1f;  // conservative default
#endif
}

float SpeculativeExecutor::queryBatteryLevel() {
#ifdef __APPLE__
    // macOS: would use IOKit PMSource queries
    // For now return 1.0 (desktop assumed to be on power)
    return 1.0f;
#elif defined(__linux__)
    // Linux: read /sys/class/power_supply/BAT0/capacity
    std::ifstream bat("/sys/class/power_supply/BAT0/capacity");
    if (bat) {
        int cap = 100;
        bat >> cap;
        return static_cast<float>(cap) / 100.0f;
    }
    return 1.0f;  // no battery = desktop
#else
    return 1.0f;
#endif
}

bool SpeculativeExecutor::queryIsCharging() {
#ifdef __linux__
    std::ifstream status("/sys/class/power_supply/BAT0/status");
    if (status) {
        std::string s;
        std::getline(status, s);
        return s == "Charging" || s == "Full";
    }
#endif
    return true;  // desktop or macOS (assume AC power)
}

std::optional<SpeculativeResult> SpeculativeExecutor::checkHit(
    const std::string& intent,
    const std::string& input_hash,
    const std::string& context_hash) const {

    auto result = cache_.get(intent, input_hash, context_hash);
    if (result) {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.successful_hits++;
    }
    return result;
}

void SpeculativeExecutor::preempt() {
    preempt_requested_.store(true);
    // Clear pending tasks — real work takes priority
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::lock_guard<std::mutex> mlock(mutex_);
        metrics_.preempted += static_cast<uint64_t>(task_queue_.size());
        task_queue_.clear();
    }
}

SpeculationMetrics SpeculativeExecutor::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

void SpeculativeExecutor::resetMetrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = {};
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string computeContextHash(const std::string& model_path,
                               const std::vector<std::string>& skills,
                               std::uint64_t session_turn) {
    std::ostringstream oss;
    oss << model_path << "|";
    for (const auto& s : skills) oss << s << ",";
    oss << "|" << session_turn;
    // Simple hash (in production: SHA-256)
    std::hash<std::string> hasher;
    return std::to_string(hasher(oss.str()));
}

std::string normalizeForCacheKey(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_ws = false;
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!last_ws && !out.empty()) out += ' ';
            last_ws = true;
        } else {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            last_ws = false;
        }
    }
    // Trim trailing
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace sparx::speculation
