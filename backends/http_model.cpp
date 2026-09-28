#include "master_agent/runtime/http_model.h"
#include <curl/curl.h>
#include <memory>

namespace master_agent::reference {
ModelHandler makeHttpModel(HttpModelConfig config) {
    return [config = std::move(config)](const Json& messages) -> Result<std::string> {
        auto fail = [](const std::string& code, const std::string& message) {
            return Result<std::string>::failure({code, message, "", 502});
        };
        if (config.timeout_ms <= 0 || (config.endpoint.rfind("http://", 0) != 0 && config.endpoint.rfind("https://", 0) != 0) ||
            config.api_key.find_first_of("\r\n") != std::string::npos)
            return fail("INVALID_CONFIG", "Expected HTTP(S) endpoint, positive timeout and valid credentials");
        static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (initialized != CURLE_OK) return fail("HTTP_ERROR", "curl initialization failed");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) return fail("HTTP_ERROR", "Cannot allocate HTTP client");
        curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!raw_headers) return fail("HTTP_ERROR", "Cannot allocate HTTP headers");
        if (!config.api_key.empty()) {
            auto* extended = curl_slist_append(raw_headers, ("Authorization: Bearer " + config.api_key).c_str());
            if (!extended) { curl_slist_free_all(raw_headers); return fail("HTTP_ERROR", "Cannot allocate credential header"); }
            raw_headers = extended;
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
        const auto body = Json{{"model", config.model}, {"messages", messages}, {"stream", false}, {"temperature", 0}, {"max_tokens", 512}}.dump();
        std::string response;
        curl_easy_setopt(curl.get(), CURLOPT_URL, config.endpoint.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, config.timeout_ms);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, config.timeout_ms);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
            +[](char* data, size_t size, size_t count, void* output) -> size_t {
                auto& text = *static_cast<std::string*>(output);
                const auto bytes = size * count;
                if (bytes > 1024 * 1024 - text.size()) return 0;
                try { text.append(data, bytes); } catch (...) { return 0; }
                return bytes;
            });
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        auto status = curl_easy_perform(curl.get());
        if (status != CURLE_OK) return fail(status == CURLE_OPERATION_TIMEDOUT ? "TIMEOUT" : "HTTP_ERROR", curl_easy_strerror(status));
        long http_status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status != 200) return fail("HTTP_ERROR", "Model endpoint returned HTTP " + std::to_string(http_status));
        try {
            auto json = Json::parse(response);
            auto content = json.at("choices").at(0).at("message").at("content");
            if (!content.is_string() || content.get_ref<const std::string&>().empty()) return fail("INVALID_RESPONSE", "Missing model content");
            return Result<std::string>::success(content.get<std::string>());
        } catch (const Json::exception&) {
            return fail("INVALID_RESPONSE", "Expected choices[0].message.content string");
        }
    };
}
}  // namespace master_agent::reference
