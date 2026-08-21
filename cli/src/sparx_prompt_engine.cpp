/**
 * @file sparx_prompt_engine.cpp
 * @brief Prompt Engine implementation — intent distillation + context pruning.
 *
 * Key design goal: reduce cloud token consumption by 70-90% while preserving
 * semantic completeness. A raw 1300-token context should compress to <200 tokens
 * for typical cockpit queries.
 */

#include "sparx_prompt_engine.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace sparx {
namespace harness {

namespace {

// ─── Token Estimation ───────────────────────────────────────────────────────

/// Rough token estimation: ~4 chars per token for English, ~1.5 for CJK.
int roughTokenCount(const std::string& text) {
    int count = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (c <= 0x20) {
            if (in_word) { ++count; in_word = false; }
        } else if (c >= 0xE0) {
            // CJK characters ~= 1 token each
            ++count;
            in_word = false;
        } else {
            in_word = true;
        }
    }
    if (in_word) ++count;
    return std::max(count, 1);
}

// ─── Intent Classification (rule-based, fast) ───────────────────────────────

struct IntentPattern {
    std::string type;
    std::vector<std::string> keywords;
};

static const std::vector<IntentPattern> kIntentPatterns = {
    {"navigation", {"导航", "路线", "怎么走", "去哪", "navigate", "route", "directions",
                    "加油站", "充电桩", "停车场", "parking", "gas station", "POI"}},
    {"vehicle_control", {"空调", "温度", "车窗", "座椅", "音量", "开灯", "关灯",
                         "AC", "temperature", "window", "seat", "volume", "light"}},
    {"media", {"播放", "音乐", "歌", "电台", "play", "music", "song", "radio",
               "podcast", "暂停", "下一首", "pause", "next", "stop"}},
    {"phone", {"电话", "打给", "接听", "挂断", "call", "phone", "dial", "hang up",
               "contacts", "短信", "message"}},
    {"weather", {"天气", "温度", "下雨", "weather", "forecast", "rain", "snow"}},
    {"general_qa", {"什么是", "为什么", "怎么", "解释", "what is", "why", "how",
                    "explain", "tell me about"}},
};

std::string classifyIntent(const std::string& input) {
    int best_score = 0;
    std::string best_type = "general_qa";

    for (const auto& pattern : kIntentPatterns) {
        int score = 0;
        for (const auto& kw : pattern.keywords) {
            if (input.find(kw) != std::string::npos) ++score;
        }
        if (score > best_score) {
            best_score = score;
            best_type = pattern.type;
        }
    }
    return best_type;
}

// ─── Parameter Extraction ───────────────────────────────────────────────────

std::unordered_map<std::string, std::string> extractParams(
    const std::string& input, const std::string& intent_type) {

    std::unordered_map<std::string, std::string> params;

    // Temperature numbers
    std::regex temp_regex(R"((\d+)\s*[°度℃])");
    std::smatch match;
    if (std::regex_search(input, match, temp_regex)) {
        params["temperature"] = match[1].str();
    }

    // Location names (after "到/去/到达")
    std::regex dest_regex("(?:到|去|到达|navigate to|go to)\\s*(.+?)(?:[，。,.]|$)");
    if (std::regex_search(input, match, dest_regex)) {
        params["destination"] = match[1].str();
    }

    // Song/artist names (after "播放/play")
    std::regex media_regex("(?:播放|play|听)\\s*(.+?)(?:[，。,.]|$)");
    if (intent_type == "media" && std::regex_search(input, match, media_regex)) {
        params["media_query"] = match[1].str();
    }

    return params;
}

// ─── Relevance Scoring for History Pruning ──────────────────────────────────

float computeRelevance(const ConversationTurn& turn,
                       const std::string& intent_type,
                       const std::string& current_input) {
    float score = 0.0f;

    // Recency bonus (more recent = more relevant)
    // This is a simplified heuristic; real impl would use timestamp deltas
    score += 0.3f;

    // Keyword overlap with current input
    int overlap = 0;
    for (size_t i = 0; i < current_input.size() && i < 50; ++i) {
        if (turn.content.find(current_input.substr(i, 4)) != std::string::npos) {
            ++overlap;
            break;
        }
    }
    if (overlap > 0) score += 0.4f;

    // Same intent type bonus
    if (turn.content.find(intent_type) != std::string::npos) {
        score += 0.2f;
    }

    return std::min(1.0f, score);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// CompressedPromptEngine
// ═══════════════════════════════════════════════════════════════════════════════

CompressedPromptEngine::CompressedPromptEngine(const PromptEngineConfig& config)
    : config_(config) {}

DistilledIntent CompressedPromptEngine::distill(const std::string& input) const {
    DistilledIntent intent;
    intent.task_type = classifyIntent(input);
    intent.query = input;  // Will be compressed in template rendering
    intent.params = extractParams(input, intent.task_type);
    intent.confidence = intent.params.empty() ? 0.5f : 0.8f;
    return intent;
}

std::vector<ConversationTurn> CompressedPromptEngine::prune(
    const std::vector<ConversationTurn>& history,
    const DistilledIntent& intent) const {

    if (history.empty()) return {};

    // Score all turns
    std::vector<std::pair<float, size_t>> scored;
    scored.reserve(history.size());
    for (size_t i = 0; i < history.size(); ++i) {
        float rel = computeRelevance(history[i], intent.task_type, intent.query);
        scored.emplace_back(rel, i);
    }

    // Sort by relevance descending
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Keep top N above threshold
    std::vector<ConversationTurn> pruned;
    int budget = config_.max_history_turns;
    for (const auto& [score, idx] : scored) {
        if (budget <= 0) break;
        if (score < config_.relevance_threshold) break;
        ConversationTurn turn = history[idx];
        turn.relevance = score;
        pruned.push_back(std::move(turn));
        --budget;
    }

    // Re-sort by original order (chronological)
    std::sort(pruned.begin(), pruned.end(),
              [&history](const ConversationTurn& a, const ConversationTurn& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });

    return pruned;
}

int CompressedPromptEngine::estimateTokens(const std::string& text) const {
    return roughTokenCount(text);
}

std::string CompressedPromptEngine::loadTemplate(const std::string& template_name) const {
    std::string path = config_.template_dir + "/" + template_name + ".txt";
    std::ifstream file(path);
    if (!file.is_open()) {
        // Fallback to built-in minimal template
        return "Task: {{task_type}}\nQuery: {{query}}\n{{#params}}Params: {{params}}{{/params}}\n{{#history}}Context: {{history}}{{/history}}";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string CompressedPromptEngine::renderTemplate(
    const std::string& tmpl,
    const DistilledIntent& intent,
    const std::vector<ConversationTurn>& pruned_history,
    const std::unordered_map<std::string, std::string>& context_vars) const {

    std::string result = tmpl;

    // Replace simple placeholders
    auto replace = [&result](const std::string& key, const std::string& value) {
        std::string placeholder = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    };

    replace("task_type", intent.task_type);
    replace("query", intent.query);

    // Build params string
    if (config_.structured_output && !intent.params.empty()) {
        std::ostringstream params_json;
        params_json << "{";
        bool first = true;
        for (const auto& [k, v] : intent.params) {
            if (!first) params_json << ",";
            params_json << "\"" << k << "\":\"" << v << "\"";
            first = false;
        }
        params_json << "}";
        replace("params", params_json.str());
    } else {
        replace("params", "");
    }

    // Build history string (compressed)
    if (!pruned_history.empty()) {
        std::ostringstream hist;
        for (const auto& turn : pruned_history) {
            hist << turn.role << ": " << turn.content.substr(0, 100) << "\n";
        }
        replace("history", hist.str());
    } else {
        replace("history", "");
    }

    // Context variables
    for (const auto& [k, v] : context_vars) {
        replace(k, v);
    }

    // Remove unfilled conditional blocks {{#...}}...{{/...}}
    std::regex block_regex(R"(\{\{#\w+\}\}.*?\{\{/\w+\}\})");
    result = std::regex_replace(result, block_regex, "");

    // Remove empty lines
    std::regex empty_lines(R"(\n\s*\n)");
    result = std::regex_replace(result, empty_lines, "\n");

    return result;
}

CompressedPrompt CompressedPromptEngine::compress(
    const std::string& user_input,
    const std::vector<ConversationTurn>& history,
    const std::unordered_map<std::string, std::string>& context_vars) const {

    CompressedPrompt output;

    // Step 1: Distill intent
    output.intent = distill(user_input);

    // Step 2: Prune history
    auto pruned = prune(history, output.intent);

    // Step 3: Load and render template
    std::string tmpl = loadTemplate(config_.default_template);
    output.user_prompt = renderTemplate(tmpl, output.intent, pruned, context_vars);

    // Step 4: System prompt (minimal for cloud)
    output.system_prompt = "You are a concise vehicle assistant. Respond in the user's language. Be brief and actionable.";

    // Step 5: Token budget check — if over budget, truncate history
    output.estimated_tokens = estimateTokens(output.user_prompt) +
                              estimateTokens(output.system_prompt);

    if (output.estimated_tokens > config_.max_cloud_input_tokens && !pruned.empty()) {
        // Drop history and re-render
        output.user_prompt = renderTemplate(tmpl, output.intent, {}, context_vars);
        output.estimated_tokens = estimateTokens(output.user_prompt) +
                                  estimateTokens(output.system_prompt);
    }

    return output;
}

std::string CompressedPromptEngine::renderLocal(
    const std::string& user_input,
    const std::vector<ConversationTurn>& history,
    const std::unordered_map<std::string, std::string>& context_vars) const {

    // Local rendering: fuller context, no aggressive compression
    std::ostringstream prompt;
    prompt << "You are a helpful vehicle assistant running on-device.\n\n";

    // Include recent history (up to 5 turns)
    int hist_count = 0;
    for (auto it = history.rbegin(); it != history.rend() && hist_count < 5; ++it, ++hist_count) {
        prompt << it->role << ": " << it->content << "\n";
    }

    // Context variables
    if (!context_vars.empty()) {
        prompt << "\n[Context]\n";
        for (const auto& [k, v] : context_vars) {
            prompt << k << ": " << v << "\n";
        }
    }

    prompt << "\nuser: " << user_input << "\nassistant: ";
    return prompt.str();
}

// ═══════════════════════════════════════════════════════════════════════════════
// VerbosePromptEngine
// ═══════════════════════════════════════════════════════════════════════════════

VerbosePromptEngine::VerbosePromptEngine(const PromptEngineConfig& config)
    : config_(config) {}

CompressedPrompt VerbosePromptEngine::compress(
    const std::string& user_input,
    const std::vector<ConversationTurn>& history,
    const std::unordered_map<std::string, std::string>& context_vars) const {

    // Verbose mode: send everything (for debugging)
    CompressedPrompt output;
    output.intent.task_type = "general";
    output.intent.query = user_input;
    output.system_prompt = "You are a helpful vehicle assistant.";

    std::ostringstream prompt;
    for (const auto& turn : history) {
        prompt << turn.role << ": " << turn.content << "\n";
    }
    for (const auto& [k, v] : context_vars) {
        prompt << "[" << k << ": " << v << "]\n";
    }
    prompt << "user: " << user_input;
    output.user_prompt = prompt.str();
    output.estimated_tokens = roughTokenCount(output.user_prompt);
    return output;
}

std::string VerbosePromptEngine::renderLocal(
    const std::string& user_input,
    const std::vector<ConversationTurn>& history,
    const std::unordered_map<std::string, std::string>& context_vars) const {

    // Same as compress for verbose mode
    auto result = compress(user_input, history, context_vars);
    return result.system_prompt + "\n" + result.user_prompt;
}

}  // namespace harness
}  // namespace sparx
