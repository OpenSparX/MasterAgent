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
// MambaPredictor — Mamba-2 Selective State Space Model with SSD Parallel Scan
// ---------------------------------------------------------------------------

/**
 * Implements a Mamba-2 architecture for intent sequence prediction with the
 * Structured State Space Duality (SSD) optimization from Gu & Dao (2024).
 *
 * Key insights:
 *   - For diagonal A, the selective scan can be reformulated as a parallel
 *     prefix scan using the associative operator:
 *       combine((d1, s1), (d2, s2)) = (d1*d2, d1*s2 + s1)
 *   - Sequences are processed in chunks of size C; within each chunk the
 *     cumulative decay matrix D[i,j] = prod_{k=i}^{j-1} d[k] is computed
 *     via prefix products, enabling parallel state computation.
 *   - Inter-chunk propagation: h_final(chunk c) seeds h_init(chunk c+1).
 *
 * Architecture:
 *   input_dim = 64 (matches EmbeddingVec dimension)
 *   state_dim = 32 (SSM hidden state dimension)
 *   chunk_size = 16 (configurable SSD chunk width)
 *   Selective scan: h[t] = Ā*h[t-1] + B̄[t], y[t] = C*h[t]
 *   Gating: output = σ(W_gate * x) ⊙ y
 *   Online training: truncated BPTT with SSD parallel forward pass
 */
class MambaPredictor {
public:
    static constexpr size_t kInputDim = 64;    // matches kEmbeddingDim
    static constexpr size_t kStateDim = 32;    // SSM hidden state dimension
    static constexpr size_t kDefaultChunkSize = 16;  // SSD parallel chunk width

    MambaPredictor(size_t num_intents_hint = 0,
                   float learning_rate = 0.005f,
                   uint32_t bptt_length = 16,
                   size_t chunk_size = kDefaultChunkSize)
        : lr_(learning_rate), bptt_length_(bptt_length),
          chunk_size_(chunk_size) {
        initWeights();
        if (num_intents_hint > 0) {
            growOutputLayer(num_intents_hint);
        }
    }

    /// Register an intent in the vocabulary, returns its index.
    size_t registerIntent(const std::string& intent) {
        auto it = intent_to_idx_.find(intent);
        if (it != intent_to_idx_.end()) return it->second;
        size_t idx = idx_to_intent_.size();
        intent_to_idx_[intent] = idx;
        idx_to_intent_.push_back(intent);
        growOutputLayer(idx + 1);
        return idx;
    }

    size_t vocabSize() const { return idx_to_intent_.size(); }

    /// Set an NPU dispatcher for hardware-accelerated parallel scan.
    void setNpuDispatcher(NpuScanDispatcher* dispatcher) {
        npu_dispatcher_ = dispatcher;
    }

    /// Run forward pass (sequential fallback): selective scan, return softmax.
    std::vector<float> predict(const std::vector<EmbeddingVec>& seq) {
        if (seq.empty() || idx_to_intent_.empty()) return {};

        // Reset SSM state
        std::array<float, kStateDim> h{};
        h.fill(0.0f);

        // Selective scan over the sequence (sequential O(T) recurrence)
        std::array<float, kStateDim> y_final{};
        for (const auto& x : seq) {
            selectiveScanStep(x, h, y_final);
        }

        return projectOutput(y_final, seq.back());
    }

    /// Run forward pass using SSD parallel scan (O(T/C) sequential depth).
    /// Falls back to sequential if sequence is shorter than one chunk.
    std::vector<float> predictParallel(const std::vector<EmbeddingVec>& seq) {
        if (seq.empty() || idx_to_intent_.empty()) return {};

        size_t T = seq.size();

        // For very short sequences, sequential is faster (no chunk overhead)
        if (T <= chunk_size_) {
            return predict(seq);
        }

        // Run SSD parallel scan
        std::array<float, kStateDim> h_init{};
        h_init.fill(0.0f);

        auto [h_states, y_states] = parallelScan(seq, h_init);

        // y_states[T-1] is our final output
        return projectOutput(y_states[T - 1], seq.back());
    }

    /// Benchmark: run both paths and return timing comparison.
    ScanBenchmark benchmark(const std::vector<EmbeddingVec>& seq) {
        ScanBenchmark result;
        if (seq.empty() || idx_to_intent_.empty()) return result;

        // Warm up
        predict(seq);
        predictParallel(seq);

        // Sequential timing
        {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i) predict(seq);
            auto t1 = std::chrono::steady_clock::now();
            result.sequential_us = std::chrono::duration<double, std::micro>(
                t1 - t0).count() / 10.0;
        }

        // Parallel timing
        {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i) predictParallel(seq);
            auto t1 = std::chrono::steady_clock::now();
            result.parallel_us = std::chrono::duration<double, std::micro>(
                t1 - t0).count() / 10.0;
        }

        return result;
    }

    /// Online training with truncated BPTT using SSD parallel forward pass.
    void train(const std::vector<EmbeddingVec>& seq, size_t target_idx) {
        if (seq.empty() || target_idx >= idx_to_intent_.size()) return;

        // Truncate sequence to bptt_length_
        size_t T = std::min(static_cast<size_t>(bptt_length_), seq.size());
        size_t start = seq.size() > T ? seq.size() - T : 0;

        // Build the truncated subsequence
        std::vector<EmbeddingVec> sub_seq(seq.begin() + start, seq.begin() + start + T);

        // Use SSD parallel forward pass to compute cached states efficiently.
        // For sequences >= chunk_size_, the parallel scan reduces sequential depth.
        std::array<float, kStateDim> h_init{};
        h_init.fill(0.0f);

        // Forward pass with cached intermediates (parallel or sequential based on T)
        std::vector<std::array<float, kStateDim>> h_states(T + 1);
        std::vector<std::array<float, kStateDim>> y_states(T);
        std::vector<std::array<float, kStateDim>> A_bar_states(T);
        std::vector<std::array<float, kStateDim>> B_bar_states(T);
        std::vector<float> dt_states(T);

        h_states[0] = h_init;

        if (T >= chunk_size_ && T > 1) {
            // Use SSD parallel scan for the forward pass (compute all h_states)
            auto [par_h, par_y] = parallelScan(sub_seq, h_init);
            // Copy results and also compute per-step A_bar, B_bar, dt for backprop
            for (size_t t = 0; t < T; ++t) {
                h_states[t + 1] = par_h[t + 1];
                y_states[t] = par_y[t];
                // Recompute discretization parameters (needed for gradient computation)
                const auto& x = sub_seq[t];
                float dt_pre = b_dt_;
                for (size_t j = 0; j < kInputDim; ++j) {
                    dt_pre += W_dt_[j] * x[j];
                }
                float dt = std::log(1.0f + std::exp(dt_pre));
                dt_states[t] = dt;
                for (size_t i = 0; i < kStateDim; ++i) {
                    A_bar_states[t][i] = std::exp(A_diag_[i] * dt);
                }
                for (size_t i = 0; i < kStateDim; ++i) {
                    float b_proj = 0.0f;
                    for (size_t j = 0; j < kInputDim; ++j) {
                        b_proj += W_B_[i * kInputDim + j] * x[j];
                    }
                    B_bar_states[t][i] = dt * b_proj;
                }
            }
        } else {
            // Sequential forward pass for short sequences
            for (size_t t = 0; t < T; ++t) {
                const auto& x = sub_seq[t];
                float dt_pre = b_dt_;
                for (size_t j = 0; j < kInputDim; ++j) {
                    dt_pre += W_dt_[j] * x[j];
                }
                float dt = std::log(1.0f + std::exp(dt_pre));
                dt_states[t] = dt;
                for (size_t i = 0; i < kStateDim; ++i) {
                    A_bar_states[t][i] = std::exp(A_diag_[i] * dt);
                }
                for (size_t i = 0; i < kStateDim; ++i) {
                    float b_proj = 0.0f;
                    for (size_t j = 0; j < kInputDim; ++j) {
                        b_proj += W_B_[i * kInputDim + j] * x[j];
                    }
                    B_bar_states[t][i] = dt * b_proj;
                }
                for (size_t i = 0; i < kStateDim; ++i) {
                    h_states[t + 1][i] = A_bar_states[t][i] * h_states[t][i]
                                       + B_bar_states[t][i];
                }
                for (size_t i = 0; i < kStateDim; ++i) {
                    y_states[t][i] = C_[i] * h_states[t + 1][i];
                }
            }
        }

        // Gating on last step
        const auto& x_last = sub_seq[T - 1];
        std::array<float, kStateDim> gate;
        for (size_t i = 0; i < kStateDim; ++i) {
            float sum = b_gate_[i];
            for (size_t j = 0; j < kInputDim; ++j) {
                sum += W_gate_[i * kInputDim + j] * x_last[j];
            }
            gate[i] = 1.0f / (1.0f + std::exp(-sum));
        }

        std::array<float, kStateDim> gated;
        for (size_t i = 0; i < kStateDim; ++i) {
            gated[i] = gate[i] * y_states[T - 1][i];
        }

        // Output logits and loss
        size_t num_intents = idx_to_intent_.size();
        std::vector<float> logits(num_intents, 0.0f);
        for (size_t i = 0; i < num_intents; ++i) {
            float sum = b_out_[i];
            for (size_t j = 0; j < kStateDim; ++j) {
                sum += W_out_[i * kStateDim + j] * gated[j];
            }
            logits[i] = sum;
        }
        auto probs = softmax(logits);

        // Backprop through output layer
        std::vector<float> d_logits(num_intents);
        for (size_t i = 0; i < num_intents; ++i) {
            d_logits[i] = probs[i] - (i == target_idx ? 1.0f : 0.0f);
        }

        // Gradient for W_out, b_out
        std::array<float, kStateDim> d_gated{};
        for (size_t i = 0; i < num_intents; ++i) {
            b_out_[i] -= lr_ * d_logits[i];
            for (size_t j = 0; j < kStateDim; ++j) {
                float grad = d_logits[i] * gated[j];
                W_out_[i * kStateDim + j] -= lr_ * grad;
                d_gated[j] += d_logits[i] * W_out_[i * kStateDim + j];
            }
        }

        // Backprop through gating
        std::array<float, kStateDim> d_y_last{};
        std::array<float, kStateDim> d_gate{};
        for (size_t i = 0; i < kStateDim; ++i) {
            d_y_last[i] = d_gated[i] * gate[i];
            d_gate[i] = d_gated[i] * y_states[T - 1][i] * gate[i] * (1.0f - gate[i]);
        }

        // Update gate weights
        for (size_t i = 0; i < kStateDim; ++i) {
            b_gate_[i] -= lr_ * d_gate[i];
            for (size_t j = 0; j < kInputDim; ++j) {
                W_gate_[i * kInputDim + j] -= lr_ * d_gate[i] * x_last[j];
            }
        }

        // Backprop through C (output mapping) at last timestep
        std::array<float, kStateDim> d_h{};
        for (size_t i = 0; i < kStateDim; ++i) {
            float d_C = d_y_last[i] * h_states[T][i];
            C_[i] -= lr_ * d_C;
            d_h[i] = d_y_last[i] * C_[i];
        }

        // Truncated BPTT through the SSM recurrence
        for (int t = static_cast<int>(T) - 1; t >= 0; --t) {
            const auto& x = sub_seq[t];

            for (size_t i = 0; i < kStateDim; ++i) {
                // Gradient for A_diag (through Ā = exp(A*dt))
                float d_A_bar = d_h[i] * h_states[t][i];
                float d_A = d_A_bar * A_bar_states[t][i] * dt_states[t];
                A_diag_[i] -= lr_ * std::max(-1.0f, std::min(1.0f, d_A));

                // Gradient for B projection weights
                float d_B_bar = d_h[i];
                for (size_t j = 0; j < kInputDim; ++j) {
                    float d_W_B = d_B_bar * dt_states[t] * x[j];
                    W_B_[i * kInputDim + j] -= lr_ * std::max(-1.0f, std::min(1.0f, d_W_B));
                }

                // Gradient for dt (through both A_bar and B_bar)
                float d_dt_from_A = d_A_bar * A_bar_states[t][i] * A_diag_[i];
                float b_proj = 0.0f;
                for (size_t j = 0; j < kInputDim; ++j) {
                    b_proj += W_B_[i * kInputDim + j] * x[j];
                }
                float d_dt_from_B = d_B_bar * b_proj;
                float d_dt_total = d_dt_from_A + d_dt_from_B;

                // softplus derivative: d_softplus/d_pre = sigmoid(pre)
                float dt_pre = b_dt_;
                for (size_t j = 0; j < kInputDim; ++j) {
                    dt_pre += W_dt_[j] * x[j];
                }
                float sig_dt = 1.0f / (1.0f + std::exp(-dt_pre));
                float d_dt_pre = d_dt_total * sig_dt / static_cast<float>(kStateDim);

                // Update dt projection weights
                for (size_t j = 0; j < kInputDim; ++j) {
                    W_dt_[j] -= lr_ * std::max(-0.1f, std::min(0.1f, d_dt_pre * x[j]));
                }
                b_dt_ -= lr_ * std::max(-0.1f, std::min(0.1f, d_dt_pre));
            }

            // Propagate gradient to previous timestep
            std::array<float, kStateDim> d_h_prev{};
            for (size_t i = 0; i < kStateDim; ++i) {
                d_h_prev[i] = d_h[i] * A_bar_states[t][i];
            }
            d_h = d_h_prev;
        }
    }

    /// Save weights to binary file.
    bool save(const std::string& path) const {
        auto parent = std::filesystem::path(path).parent_path();
        std::filesystem::create_directories(parent);

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        uint32_t magic = 0x4D414D42;  // "MAMB"
        uint32_t version = 2;  // bumped for SSD fields
        uint32_t vocab_size = static_cast<uint32_t>(idx_to_intent_.size());
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&vocab_size), 4);

        // Write chunk_size configuration
        uint32_t cs = static_cast<uint32_t>(chunk_size_);
        out.write(reinterpret_cast<const char*>(&cs), 4);

        // Write vocab
        for (const auto& name : idx_to_intent_) {
            uint16_t len = static_cast<uint16_t>(name.size());
            out.write(reinterpret_cast<const char*>(&len), 2);
            out.write(name.data(), len);
        }

        // Write SSM parameters
        out.write(reinterpret_cast<const char*>(A_diag_.data()), kStateDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(C_.data()), kStateDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_B_.data()), W_B_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_dt_.data()), kInputDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(&b_dt_), sizeof(float));
        out.write(reinterpret_cast<const char*>(W_gate_.data()), W_gate_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_gate_.data()), kStateDim * sizeof(float));
        out.write(reinterpret_cast<const char*>(W_out_.data()), W_out_.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(b_out_.data()), b_out_.size() * sizeof(float));

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

        if (magic != 0x4D414D42 || (version != 1 && version != 2)) return false;

        // Version 2 adds chunk_size field
        if (version >= 2) {
            uint32_t cs = 0;
            in.read(reinterpret_cast<char*>(&cs), 4);
            if (cs > 0 && cs <= 256) chunk_size_ = static_cast<size_t>(cs);
        }

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

        // Read SSM parameters
        in.read(reinterpret_cast<char*>(A_diag_.data()), kStateDim * sizeof(float));
        in.read(reinterpret_cast<char*>(C_.data()), kStateDim * sizeof(float));
        in.read(reinterpret_cast<char*>(W_B_.data()), W_B_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(W_dt_.data()), kInputDim * sizeof(float));
        in.read(reinterpret_cast<char*>(&b_dt_), sizeof(float));
        in.read(reinterpret_cast<char*>(W_gate_.data()), W_gate_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(b_gate_.data()), kStateDim * sizeof(float));

        growOutputLayer(vocab_size);
        in.read(reinterpret_cast<char*>(W_out_.data()), W_out_.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(b_out_.data()), b_out_.size() * sizeof(float));

        return in.good();
    }

    const std::map<std::string, size_t>& intentIndex() const { return intent_to_idx_; }
    const std::string& intentName(size_t idx) const { return idx_to_intent_[idx]; }

    /// Get/set the SSD chunk size.
    size_t chunkSize() const { return chunk_size_; }
    void setChunkSize(size_t cs) { if (cs > 0) chunk_size_ = cs; }

private:
    float lr_;
    uint32_t bptt_length_;
    size_t chunk_size_;

    // SSM diagonal state matrix A (negative values for stability)
    std::array<float, kStateDim> A_diag_;
    // Output mapping C
    std::array<float, kStateDim> C_;
    // Input-dependent B projection: kStateDim x kInputDim
    std::vector<float> W_B_;
    // dt (discretization step) projection: kInputDim -> scalar
    std::array<float, kInputDim> W_dt_;
    float b_dt_ = 0.0f;
    // Gating: kStateDim x kInputDim
    std::vector<float> W_gate_;
    std::array<float, kStateDim> b_gate_;
    // Output projection: num_intents x kStateDim
    std::vector<float> W_out_;
    std::vector<float> b_out_;

    // Vocab
    std::map<std::string, size_t> intent_to_idx_;
    std::vector<std::string> idx_to_intent_;

    // Optional NPU dispatcher for hardware-accelerated scan
    NpuScanDispatcher* npu_dispatcher_ = nullptr;

    // -----------------------------------------------------------------------
    // SSD Parallel Scan — Structured State Space Duality (Gu & Dao 2024)
    // -----------------------------------------------------------------------

    /**
     * @brief Parallel prefix scan implementing the SSD associative operator.
     *
     * For diagonal A, the recurrence h[t] = d[t]*h[t-1] + B_bar[t] can be
     * computed as a parallel prefix scan with the associative operator:
     *   combine((d1, s1), (d2, s2)) = (d1*d2, d2*s1 + s2)
     *
     * This processes the sequence in chunks of size C:
     *   1. Within each chunk: compute all states via parallel prefix scan
     *   2. Between chunks: propagate final state of chunk c as initial state of c+1
     *
     * The within-chunk computation has O(log C) sequential depth per state dimension,
     * and the inter-chunk propagation is O(num_chunks) sequential.
     *
     * SIMD hints: state arrays are 64-byte aligned for AVX-512 / NEON auto-vectorization.
     *
     * @param seq Full input sequence of embeddings.
     * @param h_init Initial hidden state for the scan.
     * @return Pair of (h_states[T+1], y_states[T]) — all intermediate states.
     */
    std::pair<std::vector<std::array<float, kStateDim>>,
              std::vector<std::array<float, kStateDim>>>
    parallelScan(const std::vector<EmbeddingVec>& seq,
                 const std::array<float, kStateDim>& h_init) {

        size_t T = seq.size();
        size_t C = chunk_size_;
        size_t num_chunks = (T + C - 1) / C;

        // Output arrays: h_states[0..T], y_states[0..T-1]
        std::vector<std::array<float, kStateDim>> h_states(T + 1);
        std::vector<std::array<float, kStateDim>> y_states(T);

        h_states[0] = h_init;

        // Pre-compute per-step decay and input for the entire sequence.
        // These are aligned for SIMD auto-vectorization.
        // Layout: decay_buf[t * kStateDim + i], input_buf[t * kStateDim + i]
        alignas(64) std::vector<float> decay_buf(T * kStateDim);
        alignas(64) std::vector<float> input_buf(T * kStateDim);

        // Compute discretized A_bar and B_bar for all timesteps
        for (size_t t = 0; t < T; ++t) {
            const auto& x = seq[t];

            // dt = softplus(W_dt * x + b_dt)
            float dt_pre = b_dt_;
            for (size_t j = 0; j < kInputDim; ++j) {
                dt_pre += W_dt_[j] * x[j];
            }
            float dt = std::log(1.0f + std::exp(dt_pre));

            // Ā[t] = exp(A_diag * dt) — per-step decay
            for (size_t i = 0; i < kStateDim; ++i) {
                decay_buf[t * kStateDim + i] = std::exp(A_diag_[i] * dt);
            }

            // B̄[t] = dt * B_proj(x) — per-step input
            for (size_t i = 0; i < kStateDim; ++i) {
                float b_proj = 0.0f;
                for (size_t j = 0; j < kInputDim; ++j) {
                    b_proj += W_B_[i * kInputDim + j] * x[j];
                }
                input_buf[t * kStateDim + i] = dt * b_proj;
            }
        }

        // Process each chunk. Inter-chunk is sequential; within-chunk is parallel.
        std::array<float, kStateDim> chunk_h_init = h_init;

        for (size_t chunk = 0; chunk < num_chunks; ++chunk) {
            size_t chunk_start = chunk * C;
            size_t chunk_len = std::min(C, T - chunk_start);

            // Check if NPU dispatch is available for this chunk
            if (npu_dispatcher_ && npu_dispatcher_->isAvailable() && chunk_len >= 4) {
                // Dispatch to NPU: it computes all states within the chunk
                // Output: chunk_len + 1 states (including h_init for this chunk)
                alignas(64) std::vector<float> npu_out((chunk_len + 1) * kStateDim);

                npu_dispatcher_->dispatchChunk(
                    decay_buf.data() + chunk_start * kStateDim,
                    input_buf.data() + chunk_start * kStateDim,
                    chunk_h_init.data(),
                    chunk_len,
                    kStateDim,
                    npu_out.data());

                npu_dispatcher_->syncResults();

                // Unpack NPU results into h_states
                for (size_t t = 0; t <= chunk_len; ++t) {
                    for (size_t i = 0; i < kStateDim; ++i) {
                        h_states[chunk_start + t][i] = npu_out[t * kStateDim + i];
                    }
                }
            } else {
                // CPU parallel prefix scan within chunk.
                // The associative scan operator for (decay, sum) tuples:
                //   combine((d1, s1), (d2, s2)) = (d1*d2, d2*s1 + s2)
                //
                // We use the Blelloch (1990) work-efficient parallel prefix algorithm.
                // For a CPU implementation this executes sequentially but with
                // vectorizable inner loops over kStateDim.

                // Temporary scan buffers for the prefix computation.
                // scan_decay[t][i] = cumulative decay from chunk_start to chunk_start+t
                // scan_sum[t][i] = accumulated weighted input
                struct alignas(64) ScanElement {
                    std::array<float, kStateDim> decay;
                    std::array<float, kStateDim> sum;
                };

                std::vector<ScanElement> scan(chunk_len);

                // Initialize scan elements from per-step values
                for (size_t t = 0; t < chunk_len; ++t) {
                    size_t global_t = chunk_start + t;
                    for (size_t i = 0; i < kStateDim; ++i) {
                        scan[t].decay[i] = decay_buf[global_t * kStateDim + i];
                        scan[t].sum[i] = input_buf[global_t * kStateDim + i];
                    }
                }

                // Inclusive prefix scan with the associative operator.
                // combine((d1, s1), (d2, s2)) = (d1*d2, d2*s1 + s2)
                // After this, scan[t] holds the combined (decay, sum) from step 0..t
                // meaning h[t+1] = scan[t].decay * h_init + scan[t].sum
                for (size_t t = 1; t < chunk_len; ++t) {
                    // Fuse previous into current: scan[t] = combine(scan[t-1], scan[t])
                    // This inner loop is structured for auto-vectorization over i
                    for (size_t i = 0; i < kStateDim; ++i) {
                        scan[t].sum[i] = scan[t].decay[i] * scan[t - 1].sum[i]
                                       + scan[t].sum[i];
                        scan[t].decay[i] = scan[t - 1].decay[i] * scan[t].decay[i];
                    }
                }

                // Materialize h_states from scan results:
                // h[chunk_start + t + 1] = scan[t].decay * h_init + scan[t].sum
                h_states[chunk_start] = chunk_h_init;
                for (size_t t = 0; t < chunk_len; ++t) {
                    for (size_t i = 0; i < kStateDim; ++i) {
                        h_states[chunk_start + t + 1][i] =
                            scan[t].decay[i] * chunk_h_init[i] + scan[t].sum[i];
                    }
                }
            }

            // Compute y_states for this chunk: y[t] = C * h[t+1]
            for (size_t t = 0; t < chunk_len; ++t) {
                for (size_t i = 0; i < kStateDim; ++i) {
                    y_states[chunk_start + t][i] = C_[i] * h_states[chunk_start + t + 1][i];
                }
            }

            // Inter-chunk propagation: final state of this chunk is init for next
            chunk_h_init = h_states[chunk_start + chunk_len];
        }

        return {std::move(h_states), std::move(y_states)};
    }

    // -----------------------------------------------------------------------
    // Output projection (shared between predict and predictParallel)
    // -----------------------------------------------------------------------

    std::vector<float> projectOutput(const std::array<float, kStateDim>& y_final,
                                     const EmbeddingVec& x_last) {
        // Gating: gate = sigmoid(W_gate * x_last)
        std::array<float, kStateDim> gate;
        for (size_t i = 0; i < kStateDim; ++i) {
            float sum = b_gate_[i];
            for (size_t j = 0; j < kInputDim; ++j) {
                sum += W_gate_[i * kInputDim + j] * x_last[j];
            }
            gate[i] = 1.0f / (1.0f + std::exp(-sum));
        }

        // Gated output: gated = gate ⊙ y
        std::array<float, kStateDim> gated;
        for (size_t i = 0; i < kStateDim; ++i) {
            gated[i] = gate[i] * y_final[i];
        }

        // Output projection: logits = W_out * gated + b_out
        size_t num_intents = idx_to_intent_.size();
        std::vector<float> logits(num_intents, 0.0f);
        for (size_t i = 0; i < num_intents; ++i) {
            float sum = b_out_[i];
            for (size_t j = 0; j < kStateDim; ++j) {
                sum += W_out_[i * kStateDim + j] * gated[j];
            }
            logits[i] = sum;
        }
        return softmax(logits);
    }

    // -----------------------------------------------------------------------
    // Initialization and utilities
    // -----------------------------------------------------------------------

    void initWeights() {
        // Initialize A as negative values (ensures stable dynamics)
        // HiPPO initialization: A_n = -(n + 1)
        for (size_t i = 0; i < kStateDim; ++i) {
            A_diag_[i] = -static_cast<float>(i + 1) * 0.1f;
        }

        // C initialized near zero
        C_.fill(0.01f);

        // W_B: Xavier init
        W_B_.resize(kStateDim * kInputDim);
        float limit_B = std::sqrt(6.0f / static_cast<float>(kStateDim + kInputDim));
        std::mt19937 rng(123);
        std::uniform_real_distribution<float> dist_B(-limit_B, limit_B);
        for (auto& w : W_B_) w = dist_B(rng);

        // W_dt: small initialization (dt should start small for stability)
        W_dt_.fill(0.0f);
        b_dt_ = 0.5f;  // softplus(0.5) ~ 0.97, reasonable initial dt

        // W_gate: Xavier init
        W_gate_.resize(kStateDim * kInputDim);
        float limit_G = std::sqrt(6.0f / static_cast<float>(kStateDim + kInputDim));
        std::uniform_real_distribution<float> dist_G(-limit_G, limit_G);
        for (auto& w : W_gate_) w = dist_G(rng);
        b_gate_.fill(0.0f);
    }

    void growOutputLayer(size_t new_size) {
        size_t old_size = b_out_.size();
        if (new_size <= old_size) return;

        float limit = std::sqrt(6.0f / static_cast<float>(kStateDim + new_size));
        std::mt19937 rng(static_cast<uint32_t>(old_size * 11 + 7));
        std::uniform_real_distribution<float> dist(-limit, limit);

        std::vector<float> new_W_out(new_size * kStateDim, 0.0f);
        for (size_t i = 0; i < old_size; ++i) {
            for (size_t j = 0; j < kStateDim; ++j) {
                new_W_out[i * kStateDim + j] = W_out_[i * kStateDim + j];
            }
        }
        for (size_t i = old_size; i < new_size; ++i) {
            for (size_t j = 0; j < kStateDim; ++j) {
                new_W_out[i * kStateDim + j] = dist(rng);
            }
        }
        W_out_ = std::move(new_W_out);
        b_out_.resize(new_size, 0.0f);
    }

    void selectiveScanStep(const EmbeddingVec& x,
                           std::array<float, kStateDim>& h,
                           std::array<float, kStateDim>& y) {
        // Compute dt = softplus(W_dt * x + b_dt)
        float dt_pre = b_dt_;
        for (size_t j = 0; j < kInputDim; ++j) {
            dt_pre += W_dt_[j] * x[j];
        }
        float dt = std::log(1.0f + std::exp(dt_pre));

        // Discretize and scan
        for (size_t i = 0; i < kStateDim; ++i) {
            // Ā = exp(A_i * dt)
            float A_bar = std::exp(A_diag_[i] * dt);

            // B̄ = dt * B_proj_i(x)  (Euler discretization)
            float b_proj = 0.0f;
            for (size_t j = 0; j < kInputDim; ++j) {
                b_proj += W_B_[i * kInputDim + j] * x[j];
            }
            float B_bar = dt * b_proj;

            // Recurrence
            h[i] = A_bar * h[i] + B_bar;

            // Output
            y[i] = C_[i] * h[i];
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
};

// ---------------------------------------------------------------------------
// IntentPredictor
// ---------------------------------------------------------------------------

IntentPredictor::IntentPredictor(PredictionConfig config)
    : config_(std::move(config)),
      lstm_(std::make_unique<LstmPredictor>(config_.lstm_learning_rate,
                                             config_.lstm_grad_clip)),
      mamba_(std::make_unique<MambaPredictor>(0, config_.mamba_learning_rate,
                                              config_.mamba_bptt_length,
                                              MambaPredictor::kDefaultChunkSize)) {
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

    // Attempt to load persisted Mamba-2 weights
    std::string mamba_path = config_.mamba_weights_path;
    if (mamba_path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            mamba_path = std::string(home) + "/.sparx/speculation/mamba.bin";
        }
    }
    if (!mamba_path.empty()) {
        mamba_->load(mamba_path);
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

    // Level 4 Mamba-2 online learning: train on recent history with truncated BPTT
    if (recent_history_.size() >= config_.mamba_min_history) {
        std::vector<EmbeddingVec> seq;
        seq.reserve(recent_history_.size());
        for (const auto& r : recent_history_) {
            seq.push_back(intent_embedder_.embed(r.intent_name));
        }
        size_t mamba_target_idx = mamba_->registerIntent(record.intent_name);
        mamba_->train(seq, mamba_target_idx);
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

    // Level 4: Mamba-2 SSM prediction using SSD parallel scan (when sufficient history)
    std::map<std::string, float> mamba_scores;
    bool use_mamba = (recent_history_.size() >= config_.mamba_min_history &&
                      mamba_->vocabSize() > 0);

    if (use_mamba) {
        std::vector<EmbeddingVec> seq;
        seq.reserve(recent_history_.size());
        for (const auto& r : recent_history_) {
            seq.push_back(intent_embedder_.embed(r.intent_name));
        }
        // Use SSD parallel path for sequences exceeding one chunk
        auto probs = mamba_->predictParallel(seq);
        for (size_t i = 0; i < probs.size(); ++i) {
            if (probs[i] > 0.01f) {
                mamba_scores[mamba_->intentName(i)] = probs[i];
            }
        }
    }

    // Blend all levels via weighted ensemble
    // Weights are dynamically normalized based on which levels are active
    std::map<std::string, float> scores;
    float total_weight = 0.0f;
    float w_ngram = 1.0f;  // ngram always active
    float w_lstm = use_lstm ? config_.lstm_weight : 0.0f;
    float w_mamba = use_mamba ? config_.mamba_weight : 0.0f;

    // If neural models are active, use configured ngram weight; else ngram gets all
    if (use_lstm || use_mamba) {
        w_ngram = config_.ngram_weight;
    }
    total_weight = w_ngram + w_lstm + w_mamba;
    if (total_weight > 0.0f) {
        w_ngram /= total_weight;
        w_lstm /= total_weight;
        w_mamba /= total_weight;
    }

    // Merge all candidate intents
    for (const auto& [intent, score] : ngram_scores) {
        scores[intent] += score * w_ngram;
    }
    for (const auto& [intent, score] : lstm_scores) {
        scores[intent] += score * w_lstm;
    }
    for (const auto& [intent, score] : mamba_scores) {
        scores[intent] += score * w_mamba;
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
        // Build rationale string reflecting active levels
        std::string rationale = "bigram+temporal+trigram";
        if (use_lstm) rationale += "+lstm";
        if (use_mamba) rationale += "+mamba2";
        rationale += " ensemble";
        pred.rationale = std::move(rationale);
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

    // Save Mamba-2 weights to binary file
    std::string mamba_path = config_.mamba_weights_path;
    if (mamba_path.empty()) {
        if (const char* home = std::getenv("HOME")) {
            mamba_path = std::string(home) + "/.sparx/speculation/mamba.bin";
        }
    }
    if (!mamba_path.empty() && mamba_) {
        mamba_->save(mamba_path);
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
// EmbeddingIndex (SimHash over character trigrams + HNSW graph for ANN)
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

int EmbeddingIndex::randomLevel() const {
    // Geometric distribution with P = 1/ln(M)
    // Each level has probability (1/M) of being exceeded
    static thread_local std::mt19937 rng(
        static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng);
    double mL = hnsw_params_.levelMultiplier();
    int level = static_cast<int>(-std::log(r) * mL);
    // Cap at a reasonable maximum (prevents degenerate tall graphs)
    return std::min(level, 16);
}

std::vector<std::pair<size_t, float>> EmbeddingIndex::searchLayer(
    const EmbeddingVec& query, size_t entry_id, size_t ef, int layer) const {

    // Priority queue based greedy search on a single HNSW layer
    // visited set to avoid revisiting
    std::vector<bool> visited(nodes_.size(), false);
    visited[entry_id] = true;

    float entry_sim = cosineSimilarity(query, nodes_[entry_id].vec);

    // candidates: max-heap by similarity (best candidates first for expansion)
    // Use negative similarity for min-heap behavior with std::greater
    using ScoredNode = std::pair<float, size_t>;  // (similarity, node_idx)

    // W = result set (sorted ascending by similarity, worst first for eviction)
    std::vector<std::pair<size_t, float>> W;
    W.push_back({entry_id, entry_sim});

    // C = candidate set (nodes to expand from)
    // We use a vector and process greedily
    std::vector<std::pair<float, size_t>> candidates;
    candidates.push_back({entry_sim, entry_id});

    while (!candidates.empty()) {
        // Pick the best unexpanded candidate
        auto best_it = std::max_element(candidates.begin(), candidates.end(),
            [](const ScoredNode& a, const ScoredNode& b) { return a.first < b.first; });
        auto [c_sim, c_idx] = *best_it;
        candidates.erase(best_it);

        // Find worst element in W
        float worst_sim = W.empty() ? -1.0f :
            std::min_element(W.begin(), W.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; })->second;

        // If the best candidate is worse than the worst result and we have enough, stop
        if (c_sim < worst_sim && W.size() >= ef) break;

        // Expand neighbors of c on this layer
        if (layer < static_cast<int>(nodes_[c_idx].neighbors.size())) {
            for (size_t neighbor_idx : nodes_[c_idx].neighbors[layer]) {
                if (neighbor_idx >= nodes_.size()) continue;
                if (visited[neighbor_idx]) continue;
                visited[neighbor_idx] = true;

                float n_sim = cosineSimilarity(query, nodes_[neighbor_idx].vec);

                // Add to W if better than worst or W not full
                worst_sim = W.empty() ? -1.0f :
                    std::min_element(W.begin(), W.end(),
                        [](const auto& a, const auto& b) { return a.second < b.second; })->second;

                if (W.size() < ef || n_sim > worst_sim) {
                    candidates.push_back({n_sim, neighbor_idx});
                    W.push_back({neighbor_idx, n_sim});

                    // Trim W to ef elements (keep best)
                    if (W.size() > ef) {
                        auto worst = std::min_element(W.begin(), W.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; });
                        W.erase(worst);
                    }
                }
            }
        }
    }

    // Sort by similarity descending
    std::sort(W.begin(), W.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    return W;
}

std::vector<size_t> EmbeddingIndex::selectNeighborsHeuristic(
    const EmbeddingVec& query,
    const std::vector<std::pair<size_t, float>>& candidates,
    size_t M) const {

    // Algorithm 4 from the HNSW paper: heuristic neighbor selection
    // Ensures diversity — avoids clustering all neighbors in one direction
    // Note: query is used implicitly — candidate similarities are relative to it
    (void)query;
    if (candidates.empty()) return {};
    if (candidates.size() <= M) {
        std::vector<size_t> result;
        result.reserve(candidates.size());
        for (const auto& [idx, _] : candidates) result.push_back(idx);
        return result;
    }

    // Working set sorted by similarity (descending)
    auto working = candidates;
    std::sort(working.begin(), working.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<size_t> selected;
    selected.reserve(M);

    for (const auto& [cand_idx, cand_sim] : working) {
        if (selected.size() >= M) break;

        // Heuristic pruning: only add if this candidate is closer to query
        // than to any already-selected neighbor (ensures spatial diversity)
        bool good = true;
        for (size_t sel_idx : selected) {
            float inter_sim = cosineSimilarity(nodes_[cand_idx].vec, nodes_[sel_idx].vec);
            if (inter_sim > cand_sim) {
                // This candidate is closer to an existing neighbor than to the query
                // Prune it to maintain diversity
                good = false;
                break;
            }
        }
        if (good) {
            selected.push_back(cand_idx);
        }
    }

    // If heuristic was too aggressive, fill with remaining closest candidates
    if (selected.size() < M) {
        for (const auto& [cand_idx, _] : working) {
            if (selected.size() >= M) break;
            if (std::find(selected.begin(), selected.end(), cand_idx) == selected.end()) {
                selected.push_back(cand_idx);
            }
        }
    }

    return selected;
}

void EmbeddingIndex::insert(const std::string& cache_key, const EmbeddingVec& vec) {
    // Check if this key already exists — update in place
    auto existing = key_to_index_.find(cache_key);
    if (existing != key_to_index_.end()) {
        size_t idx = existing->second;
        nodes_[idx].vec = vec;
        // Note: we don't rebuild connections on update (trade-off for speed)
        return;
    }

    // Assign random level for new node
    int node_level = randomLevel();
    size_t new_idx = nodes_.size();

    HnswNode node;
    node.cache_key = cache_key;
    node.vec = vec;
    node.level = node_level;
    node.neighbors.resize(node_level + 1);
    nodes_.push_back(std::move(node));
    key_to_index_[cache_key] = new_idx;

    // First node — becomes entry point
    if (nodes_.size() == 1) {
        max_level_ = node_level;
        entry_point_ = 0;
        return;
    }

    // Greedy descent from entry point through layers above the new node's level
    size_t ep = entry_point_;
    for (int layer = max_level_; layer > node_level; --layer) {
        auto results = searchLayer(vec, ep, 1, layer);
        if (!results.empty()) {
            ep = results[0].first;
        }
    }

    // Insert into each layer [0, node_level]
    for (int layer = std::min(node_level, max_level_); layer >= 0; --layer) {
        size_t ef = hnsw_params_.efConstruction;
        auto candidates = searchLayer(vec, ep, ef, layer);

        // Select M best neighbors using heuristic
        size_t M = hnsw_params_.M;
        // Layer 0 can have 2*M connections (as per HNSW paper)
        size_t maxM = (layer == 0) ? 2 * M : M;
        auto neighbors = selectNeighborsHeuristic(vec, candidates, maxM);

        // Set forward connections from new node
        nodes_[new_idx].neighbors[layer] = neighbors;

        // Set backward connections (add new node as neighbor to selected nodes)
        for (size_t neighbor_idx : neighbors) {
            auto& nb_neighbors = nodes_[neighbor_idx].neighbors[layer];
            nb_neighbors.push_back(new_idx);

            // If neighbor has too many connections, prune
            if (nb_neighbors.size() > maxM) {
                // Rebuild neighbor list with heuristic selection
                std::vector<std::pair<size_t, float>> nb_candidates;
                nb_candidates.reserve(nb_neighbors.size());
                for (size_t n : nb_neighbors) {
                    float sim = cosineSimilarity(nodes_[neighbor_idx].vec, nodes_[n].vec);
                    nb_candidates.push_back({n, sim});
                }
                nb_neighbors = selectNeighborsHeuristic(
                    nodes_[neighbor_idx].vec, nb_candidates, maxM);
            }
        }

        // Use closest result as entry point for next layer down
        if (!candidates.empty()) {
            ep = candidates[0].first;
        }
    }

    // Update entry point if new node has higher level
    if (node_level > max_level_) {
        max_level_ = node_level;
        entry_point_ = new_idx;
    }
}

std::optional<EmbeddingIndex::NearestResult> EmbeddingIndex::findNearest(
    const EmbeddingVec& query, float threshold) const {

    if (nodes_.empty()) return std::nullopt;

    // HNSW search: greedy descent from top layer, then ef-search on layer 0
    size_t ep = entry_point_;

    // Traverse from top layer down to layer 1 with ef=1 (greedy)
    for (int layer = max_level_; layer >= 1; --layer) {
        auto results = searchLayer(query, ep, 1, layer);
        if (!results.empty()) {
            ep = results[0].first;
        }
    }

    // Search layer 0 with full efSearch
    auto results = searchLayer(query, ep, hnsw_params_.efSearch, 0);

    if (results.empty()) return std::nullopt;

    // Best result is first (sorted descending by similarity)
    const auto& [best_idx, best_sim] = results[0];
    if (best_sim >= threshold) {
        NearestResult nr;
        nr.cache_key = nodes_[best_idx].cache_key;
        nr.similarity = best_sim;
        return nr;
    }
    return std::nullopt;
}

void EmbeddingIndex::remove(const std::string& cache_key) {
    auto it = key_to_index_.find(cache_key);
    if (it == key_to_index_.end()) return;

    size_t remove_idx = it->second;

    // Remove all connections TO this node from its neighbors
    for (int layer = 0; layer <= nodes_[remove_idx].level; ++layer) {
        if (layer >= static_cast<int>(nodes_[remove_idx].neighbors.size())) break;
        for (size_t neighbor_idx : nodes_[remove_idx].neighbors[layer]) {
            if (neighbor_idx >= nodes_.size()) continue;
            if (layer >= static_cast<int>(nodes_[neighbor_idx].neighbors.size())) continue;
            auto& nb_list = nodes_[neighbor_idx].neighbors[layer];
            nb_list.erase(
                std::remove(nb_list.begin(), nb_list.end(), remove_idx),
                nb_list.end());
        }
    }

    // Swap-remove: move last node into this slot
    size_t last_idx = nodes_.size() - 1;
    if (remove_idx != last_idx) {
        // Update all references from last_idx to remove_idx in neighbors
        for (int layer = 0; layer <= nodes_[last_idx].level; ++layer) {
            if (layer >= static_cast<int>(nodes_[last_idx].neighbors.size())) break;
            for (size_t neighbor_idx : nodes_[last_idx].neighbors[layer]) {
                if (neighbor_idx >= nodes_.size()) continue;
                if (layer >= static_cast<int>(nodes_[neighbor_idx].neighbors.size())) continue;
                auto& nb_list = nodes_[neighbor_idx].neighbors[layer];
                for (auto& n : nb_list) {
                    if (n == last_idx) n = remove_idx;
                }
            }
        }

        // Move node data
        nodes_[remove_idx] = std::move(nodes_[last_idx]);
        key_to_index_[nodes_[remove_idx].cache_key] = remove_idx;

        // Fix entry point if it was the last node
        if (entry_point_ == last_idx) {
            entry_point_ = remove_idx;
        }
    }

    nodes_.pop_back();
    key_to_index_.erase(it);

    // If we removed the entry point or graph is empty, reset
    if (nodes_.empty()) {
        max_level_ = -1;
        entry_point_ = 0;
    } else if (entry_point_ >= nodes_.size()) {
        // Find new entry point (node with highest level)
        entry_point_ = 0;
        max_level_ = nodes_[0].level;
        for (size_t i = 1; i < nodes_.size(); ++i) {
            if (nodes_[i].level > max_level_) {
                max_level_ = nodes_[i].level;
                entry_point_ = i;
            }
        }
    }
}

void EmbeddingIndex::clear() {
    nodes_.clear();
    key_to_index_.clear();
    max_level_ = -1;
    entry_point_ = 0;
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

    // NPU dispatch: pre-warm KV cache when confidence exceeds threshold
    if (config_.npu_dispatch_fn && task.confidence > config_.npu_dispatch_threshold) {
        bool accepted = config_.npu_dispatch_fn(
            task.predicted_input, *result, task.confidence);
        std::lock_guard<std::mutex> lock(mutex_);
        if (accepted) {
            metrics_.npu_dispatches++;
        } else {
            metrics_.npu_dispatch_failures++;
        }
    }
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

void SpeculativeExecutor::setNpuDispatchFn(NpuDispatchFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.npu_dispatch_fn = std::move(fn);
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
