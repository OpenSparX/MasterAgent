#include <master_agent/runtime/reference_runtime.h>
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <memory>
using namespace master_agent;
using namespace master_agent::reference;
namespace {
ToolOutcome reserve(const std::string& endpoint, const ExecutionContext& context, const Json& args) {
    if (context.stopRequested()) return ToolOutcome::failed("Cancelled before sending");
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return ToolOutcome::failed("HTTP initialization failed before sending");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return ToolOutcome::failed("HTTP allocation failed before sending");
    const auto key = context.idempotencyKey();
    const std::string header = "Idempotency-Key: " + key;
    curl_slist* raw = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!raw) return ToolOutcome::failed("Header allocation failed before sending");
    auto* with_key = curl_slist_append(raw, header.c_str());
    if (!with_key) { curl_slist_free_all(raw); return ToolOutcome::failed("Header allocation failed before sending"); }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(with_key, curl_slist_free_all);
    const auto body = args.dump();
    std::string response;
    long timeout = 30000;
    if (context.options.deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*context.options.deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return ToolOutcome::failed("Deadline elapsed before sending");
        timeout = static_cast<long>(std::min<decltype(remaining)>(remaining, timeout));
    }
    const auto url = endpoint + "/reservations";
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char* bytes, size_t size, size_t count, void* output) -> size_t {
            auto& text = *static_cast<std::string*>(output);
            const auto length = size * count;
            if (length > 16384 - text.size()) return 0;
            try { text.append(bytes, length); } catch (...) { return 0; }
            return length;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<const ExecutionContext*>(data)->stopRequested() ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &context);
    auto transfer = curl_easy_perform(curl.get());
    // Transport errors cannot establish whether the remote mutation committed.
    if (transfer != CURLE_OK) return ToolOutcome::unknown(curl_easy_strerror(transfer));
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    try {
        auto receipt = Json::parse(response);
        if (receipt.at("key") != key) return ToolOutcome::unknown("Receipt identity mismatch");
        if (status == 200 && receipt.at("outcome") == "committed" && receipt.at("output").at("sku") == args.at("sku") &&
            receipt.at("output").at("quantity") == args.at("quantity"))
            return ToolOutcome::committed(receipt.at("output"));
        if (status == 409 && receipt.at("outcome") == "failed")
            return ToolOutcome::failed(receipt.at("error").get<std::string>());
    } catch (const Json::exception&) { /* A malformed receipt is not evidence of failure. */ }
    return ToolOutcome::unknown("No authoritative service receipt");
}
int integer(const char* text) {
    const std::string value(text);
    int result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result <= 0 || result > 300000)
        throw std::invalid_argument("Expected integer in 1..300000");
    return result;
}
}
int main(int argc, char** argv) {
    if (argc != 7 && argc != 8) {
        std::cerr << "Usage: inventory_agent BASE_URL STATE_DB SESSION REQUEST SKU QUANTITY [TIMEOUT_MS]\n";
        return 2;
    }
    try {
        const std::string endpoint(argv[1]);
        if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) return 2;
        Json arguments{{"sku", argv[5]}, {"quantity", integer(argv[6])}};
        const auto input = arguments.dump();
        Runtime runtime;
        auto opened = runtime.openStore(argv[2]);
        if (!opened) { std::cout << Json{{"ok", false}, {"error", opened.error_code}} << '\n'; return 1; }
        Json schema{{"type", "object"}, {"additionalProperties", false}, {"required", {"sku", "quantity"}},
            {"properties", {{"sku", {{"type", "string"}}}, {"quantity", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300000}}}}}};
        if (!runtime.registerContextTool({"inventory.reserve", "Reserve real service inventory", schema,
            [&](const ExecutionContext& context, const Json& args) { return reserve(endpoint, context, args); }})) return 2;
        if (!runtime.registerSkill(input, "inventory.reserve", arguments)) return 2;
        RunOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(argc == 8 ? integer(argv[7]) : 30000);
        auto result = runtime.run({argv[3], argv[4], input}, options);
        if (result) std::cout << Json{{"ok", true}, {"output", result.value->output}, {"replayed", result.value->replayed}} << '\n';
        else std::cout << Json{{"ok", false}, {"error", result.error->code}, {"message", result.error->message}} << '\n';
        return result ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
