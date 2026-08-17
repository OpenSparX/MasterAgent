/**
 * @file sparx_trace.cpp
 * @brief Distributed tracing / observability infrastructure.
 *
 * Records execution spans (start, end, metadata) for skills, tool calls,
 * and inference requests. Supports OpenTelemetry-compatible export.
 */
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sparx::tracing {

struct Span {
    std::string trace_id;
    std::string span_id;
    std::string parent_id;
    std::string operation;
    int64_t start_us = 0;
    int64_t end_us = 0;
    std::vector<std::pair<std::string, std::string>> tags;
};

class TraceCollector {
public:
    static TraceCollector& instance() {
        static TraceCollector tc;
        return tc;
    }

    Span beginSpan(const std::string& operation,
                   const std::string& parent_id = "") {
        Span s;
        s.span_id = generateId();
        s.trace_id = parent_id.empty() ? s.span_id : parent_id;
        s.parent_id = parent_id;
        s.operation = operation;
        s.start_us = nowMicros();
        return s;
    }

    void endSpan(Span& s) {
        s.end_us = nowMicros();
        std::lock_guard<std::mutex> lock(mutex_);
        spans_.push_back(s);
        if (spans_.size() > max_spans_) spans_.pop_front();
    }

    std::vector<Span> recentSpans(size_t limit = 100) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Span> result;
        size_t count = std::min(limit, spans_.size());
        auto it = spans_.rbegin();
        for (size_t i = 0; i < count && it != spans_.rend(); ++i, ++it) {
            result.push_back(*it);
        }
        return result;
    }

private:
    TraceCollector() = default;

    static int64_t nowMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string generateId() {
        static uint64_t counter = 0;
        return "span-" + std::to_string(++counter);
    }

    mutable std::mutex mutex_;
    std::deque<Span> spans_;
    size_t max_spans_ = 10000;
};

}  // namespace sparx::tracing
