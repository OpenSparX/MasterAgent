/**
 * @file sparx_cloud_backend.cpp
 * @brief Cloud Backend implementations — HTTP clients for cloud LLM APIs.
 *
 * Minimal-design principle: the prompt is already compressed by IPromptEngine,
 * this module just handles the HTTP transport and response parsing.
 * No agent logic, no tool calling — raw model inference only.
 */

#include "sparx_cloud_backend.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

// Platform HTTP support (same pattern as sparx_cloud_fusion.cpp)
#ifndef SPARX_HAS_CURL
#ifdef __has_include
#if __has_include(<curl/curl.h>)
#define SPARX_HAS_CURL 1
#else
#define SPARX_HAS_CURL 0
#endif
#else
#define SPARX_HAS_CURL 0
#endif
#endif

#if SPARX_HAS_CURL
#include <curl/curl.h>
#endif

namespace sparx {
namespace harness {

// ═══════════════════════════════════════════════════════════════════════════════
// JSON Helpers (minimal, no external dependency)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

/// Escape a string for JSON embedding.
std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:   out << c; break;
        }
    }
    return out.str();
}

/// Extract a string value from JSON by key (simple, no nesting).
std::string jsonExtractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    // Find the colon after the key
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // Find the opening quote of the value
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";

    // Extract until closing quote (handling escapes)
    std::string value;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            switch (json[i + 1]) {
                case '"': value += '"'; ++i; break;
                case '\\': value += '\\'; ++i; break;
                case 'n': value += '\n'; ++i; break;
                case 'r': value += '\r'; ++i; break;
                case 't': value += '\t'; ++i; break;
                default: value += json[i + 1]; ++i; break;
            }
        } else if (json[i] == '"') {
            break;
        } else {
            value += json[i];
        }
    }
    return value;
}

/// Extract an integer value from JSON by key.
int jsonExtractInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0;

    // Skip whitespace
    pos = json.find_first_of("0123456789-", pos + 1);
    if (pos == std::string::npos) return 0;

    return std::atoi(json.c_str() + pos);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// OpenAICompatBackend
// ═══════════════════════════════════════════════════════════════════════════════

OpenAICompatBackend::OpenAICompatBackend(const CloudBackendConfig& config)
    : config_(config) {
    // Resolve API key: env var takes precedence
    if (!config_.api_key_env.empty()) {
        if (const char* key = std::getenv(config_.api_key_env.c_str())) {
            resolved_api_key_ = key;
        }
    }
    if (resolved_api_key_.empty()) {
        resolved_api_key_ = config_.api_key;
    }
}

bool OpenAICompatBackend::isReady() const {
    return !config_.endpoint.empty() && !resolved_api_key_.empty();
}

std::string OpenAICompatBackend::buildRequestBody(
    const std::string& user_prompt,
    const std::string& system_prompt) const {

    std::ostringstream json;
    json << "{\"model\":\"" << jsonEscape(config_.model) << "\","
         << "\"max_tokens\":" << config_.max_tokens << ","
         << "\"temperature\":" << config_.temperature << ","
         << "\"messages\":[";

    if (!system_prompt.empty()) {
        json << "{\"role\":\"system\",\"content\":\"" << jsonEscape(system_prompt) << "\"},";
    }
    json << "{\"role\":\"user\",\"content\":\"" << jsonEscape(user_prompt) << "\"}";
    json << "]}";

    return json.str();
}

CloudResult OpenAICompatBackend::parseResponse(const std::string& body, int latency_ms) const {
    CloudResult result;
    result.latency_ms = latency_ms;

    // Extract content from choices[0].message.content
    // Find "choices" array, then find "content" within it
    auto choices_pos = body.find("\"choices\"");
    if (choices_pos != std::string::npos) {
        auto content_pos = body.find("\"content\"", choices_pos);
        if (content_pos != std::string::npos) {
            // Re-parse from content position
            std::string sub = body.substr(content_pos);
            result.content = jsonExtractString(sub, "content");
            if (!result.content.empty()) {
                result.success = true;
            }
        }
    }

    // Extract usage info
    auto usage_pos = body.find("\"usage\"");
    if (usage_pos != std::string::npos) {
        std::string usage_sub = body.substr(usage_pos);
        result.input_tokens = jsonExtractInt(usage_sub, "prompt_tokens");
        result.output_tokens = jsonExtractInt(usage_sub, "completion_tokens");
    }

    // Extract model
    result.model = jsonExtractString(body, "model");

    // Extract request ID (varies by provider)
    result.request_id = jsonExtractString(body, "id");

    if (!result.success) {
        // Try to extract error message
        std::string error_msg = jsonExtractString(body, "message");
        if (error_msg.empty()) error_msg = jsonExtractString(body, "error");
        result.error = error_msg.empty()
            ? "failed to parse response: " + body.substr(0, 200)
            : error_msg;
    }

    return result;
}

CloudResult OpenAICompatBackend::doHttpPost(const std::string& body) const {
    CloudResult result;
    auto start = std::chrono::steady_clock::now();

#if SPARX_HAS_CURL
    std::string response_body;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "failed to initialize curl";
        return result;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth_header = "Authorization: Bearer " + resolved_api_key_;
    headers = curl_slist_append(headers, auth_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, config_.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* out = static_cast<std::string*>(userdata);
            out->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Follow redirects, verify SSL
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    auto elapsed = std::chrono::steady_clock::now() - start;
    int latency = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    if (res != CURLE_OK) {
        result.error = std::string("HTTP error: ") + curl_easy_strerror(res);
        result.latency_ms = latency;
        return result;
    }

    if (http_code != 200) {
        result.error = "HTTP " + std::to_string(http_code) + ": " +
                       response_body.substr(0, 200);
        result.latency_ms = latency;
        return result;
    }

    return parseResponse(response_body, latency);
#else
    (void)body;
    result.error = "cloud backend requires libcurl (build with -DSPARX_ENABLE_CURL=ON)";
    auto elapsed = std::chrono::steady_clock::now() - start;
    result.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return result;
#endif
}

CloudResult OpenAICompatBackend::infer(
    const std::string& user_prompt,
    const std::string& system_prompt) const {

    if (!isReady()) {
        return CloudResult{false, "", 0, 0, 0,
                           "cloud backend not configured (missing endpoint or API key)",
                           "", ""};
    }

    std::string body = buildRequestBody(user_prompt, system_prompt);
    return doHttpPost(body);
}

std::future<CloudResult> OpenAICompatBackend::inferAsync(
    const std::string& user_prompt,
    const std::string& system_prompt) const {

    // Capture by value for thread safety
    auto config = config_;
    auto api_key = resolved_api_key_;
    auto endpoint = config_.endpoint;
    auto body = buildRequestBody(user_prompt, system_prompt);

    return std::async(std::launch::async, [this, body]() {
        return doHttpPost(body);
    });
}

void OpenAICompatBackend::inferWithCallback(
    const std::string& user_prompt,
    const std::string& system_prompt,
    CloudCallback callback) const {

    auto body = buildRequestBody(user_prompt, system_prompt);

    // Fire in a detached thread (harness manages lifetime via deadline)
    std::thread([this, body, callback]() {
        auto result = doHttpPost(body);
        if (callback) callback(std::move(result));
    }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════════
// MockCloudBackend
// ═══════════════════════════════════════════════════════════════════════════════

MockCloudBackend::MockCloudBackend(const std::string& response, int latency_ms)
    : response_(response), latency_ms_(latency_ms) {}

CloudResult MockCloudBackend::infer(
    const std::string& /*user_prompt*/,
    const std::string& /*system_prompt*/) const {

    // Simulate latency
    std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms_));

    CloudResult result;
    result.success = true;
    result.content = response_;
    result.latency_ms = latency_ms_;
    result.model = "mock-model";
    result.output_tokens = 10;
    return result;
}

std::future<CloudResult> MockCloudBackend::inferAsync(
    const std::string& user_prompt,
    const std::string& system_prompt) const {

    return std::async(std::launch::async, [this, user_prompt, system_prompt]() {
        return infer(user_prompt, system_prompt);
    });
}

void MockCloudBackend::inferWithCallback(
    const std::string& user_prompt,
    const std::string& system_prompt,
    CloudCallback callback) const {

    std::thread([this, user_prompt, system_prompt, callback]() {
        auto result = infer(user_prompt, system_prompt);
        if (callback) callback(std::move(result));
    }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Factory
// ═══════════════════════════════════════════════════════════════════════════════

std::unique_ptr<ICloudBackend> createCloudBackend(const CloudBackendConfig& config) {
    switch (config.provider) {
        case CloudBackendConfig::Provider::OpenAICompatible:
            return std::make_unique<OpenAICompatBackend>(config);
        case CloudBackendConfig::Provider::Anthropic:
            // TODO: implement Anthropic backend
            return std::make_unique<OpenAICompatBackend>(config);
        case CloudBackendConfig::Provider::Custom:
            // TODO: implement custom template backend
            return std::make_unique<OpenAICompatBackend>(config);
    }
    return nullptr;
}

}  // namespace harness
}  // namespace sparx
