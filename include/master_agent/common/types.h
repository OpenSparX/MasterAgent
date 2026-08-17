#pragma once

/**
 * @file types.h
 * @brief Core type definitions shared across all Master Agent modules.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace master_agent {

/// Task priority levels (P0 = highest / preemptive, P3 = background).
enum class TaskPriority : std::uint8_t {
    P0 = 0,  // Safety-critical, preempts everything
    P1 = 1,  // Normal interactive
    P2 = 2,  // Batch / speculative
    P3 = 3   // Background maintenance
};

/// Identifies which kernel module is making an inference or tool call.
enum class CallerModuleId : std::uint8_t {
    IntentRecognitionEngine = 0,
    TaskOrchestrationEngine = 1,
    SkillExecutionEngine = 2,
    MemoryConsolidation = 3,
    SpeculativeExecution = 4,
    SubAgentBridge = 5,
    External = 255
};

/// Unique operation identifier (UUID v4 string).
using OperationId = std::string;

/// Session identifier.
using SessionId = std::string;

/// Request identifier (unique per inference/tool call).
using RequestId = std::string;

/// Structured error with code and message.
struct StructuredError {
    std::string code;
    std::string message;
    std::string detail;
    int http_status = 500;
};

/// Generic status indicator.
struct Status {
    bool ok = true;
    std::string error_code;
    std::string error_message;

    static Status Ok() { return Status{true, "", ""}; }
    static Status Error(std::string code, std::string msg = "") {
        return Status{false, std::move(code), std::move(msg)};
    }
    explicit operator bool() const { return ok; }
};

/// Generic result type (value or error).
template <typename T>
struct Result {
    std::optional<T> value;
    std::optional<StructuredError> error;

    bool ok() const { return value.has_value(); }
    explicit operator bool() const { return ok(); }
    const T& operator*() const { return *value; }
    T& operator*() { return *value; }

    static Result success(T val) {
        Result r;
        r.value = std::move(val);
        return r;
    }
    static Result failure(StructuredError err) {
        Result r;
        r.error = std::move(err);
        return r;
    }
};

/// Call context passed through the execution pipeline.
struct CallContext {
    std::string request_id;
    std::string session_id;
    std::string principal_id;
    std::string parent_operation_id;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    CallerModuleId caller = CallerModuleId::External;
};

}  // namespace master_agent

// Pull into inference namespace for convenience
namespace master_agent::inference {
using master_agent::TaskPriority;
using master_agent::CallerModuleId;
using master_agent::Status;
using master_agent::Result;
using master_agent::CallContext;
using master_agent::StructuredError;
}
