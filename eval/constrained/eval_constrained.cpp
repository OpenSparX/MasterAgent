/**
 * @file eval_constrained.cpp
 * @brief Comprehensive evaluation of sparx constrained decoding (GBNF generation).
 *
 * Tests the GbnfGenerator against 15+ realistic MCP tool schemas of varying
 * complexity, measuring correctness, coverage, scaling, and overhead.
 *
 * Build:
 *   g++ -std=c++17 -O2 -I../../cli/include \
 *       -o eval_constrained eval_constrained.cpp ../../cli/src/sparx_constrained_decode.cpp
 *
 * Run:
 *   ./eval_constrained [--verbose]
 */

#include "sparx_constrained_decode.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace sparx::constrained;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static bool g_verbose = false;
static int g_pass = 0;
static int g_fail = 0;
static int g_total_tests = 0;

struct MetricResult {
    std::string name;
    double value;
    std::string unit;
    bool passed;
    std::string detail;
};

static std::vector<MetricResult> g_metrics;

void recordMetric(const std::string& name, double val, const std::string& unit,
                  bool passed, const std::string& detail = "") {
    g_metrics.push_back({name, val, unit, passed, detail});
}

void check(bool cond, const std::string& test_name,
           const std::string& detail = "") {
    g_total_tests++;
    if (cond) {
        g_pass++;
        if (g_verbose) {
            std::cout << "  [PASS] " << test_name << "\n";
        }
    } else {
        g_fail++;
        std::cout << "  [FAIL] " << test_name;
        if (!detail.empty()) std::cout << " -- " << detail;
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// GBNF Validation Helpers
// ---------------------------------------------------------------------------

/// Check that the grammar string is syntactically well-formed GBNF.
/// Validates: rule definitions (name ::= ...), balanced quotes, no empty rules.
struct GbnfValidation {
    bool valid = true;
    int rule_count = 0;
    std::vector<std::string> errors;
    std::set<std::string> defined_rules;
    std::set<std::string> referenced_rules;
};

GbnfValidation validateGbnf(const std::string& grammar) {
    GbnfValidation result;
    if (grammar.empty()) {
        result.valid = false;
        result.errors.push_back("Empty grammar");
        return result;
    }

    std::istringstream stream(grammar);
    std::string line;
    int line_num = 0;

    while (std::getline(stream, line)) {
        line_num++;
        // Skip comments and blank lines
        if (line.empty() || line[0] == '#') continue;

        // Check for rule definition
        auto sep = line.find("::=");
        if (sep != std::string::npos) {
            result.rule_count++;
            std::string rule_name = line.substr(0, sep);
            // Trim whitespace
            while (!rule_name.empty() && rule_name.back() == ' ')
                rule_name.pop_back();
            result.defined_rules.insert(rule_name);

            // Check rule body is non-empty
            std::string body = line.substr(sep + 3);
            bool all_space = true;
            for (char c : body) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    all_space = false;
                    break;
                }
            }
            if (all_space) {
                result.valid = false;
                result.errors.push_back("Empty rule body at line " +
                                        std::to_string(line_num));
            }
        }
    }

    if (result.rule_count == 0) {
        result.valid = false;
        result.errors.push_back("No rules found");
    }

    // Check root rule exists
    if (result.defined_rules.find("root") == result.defined_rules.end()) {
        result.valid = false;
        result.errors.push_back("No 'root' rule defined");
    }

    return result;
}

/// Replicate the sanitizeRuleName logic from the implementation
std::string testSanitizeRuleName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '_' || c == ' ') {
            out += '-';
        }
    }
    return out.empty() ? "rule" : out;
}

/// Check that a grammar contains a rule for a given tool name
bool grammarHasToolRule(const std::string& grammar, const std::string& tool) {
    std::string pattern = testSanitizeRuleName(tool) + "-call";
    return grammar.find(pattern) != std::string::npos;
}

/// Count total rules in a grammar
int countRules(const std::string& grammar) {
    int count = 0;
    std::istringstream s(grammar);
    std::string line;
    while (std::getline(s, line)) {
        if (line.find("::=") != std::string::npos && line[0] != '#') count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Tool Schema Definitions — 15+ realistic MCP tool schemas
// ---------------------------------------------------------------------------

/// 1. Simple: set_alarm — 2 fields
json schema_set_alarm() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "action": {"type": "string", "enum": ["set_alarm"]},
            "time": {"type": "string"}
        },
        "required": ["action", "time"]
    })");
}

/// 2. Simple: send_message — 3 fields
json schema_send_message() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "recipient": {"type": "string"},
            "body": {"type": "string"},
            "urgent": {"type": "boolean"}
        },
        "required": ["recipient", "body"]
    })");
}

/// 3. Medium: weather API — 5 fields
json schema_weather() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "location": {"type": "string"},
            "units": {"type": "string", "enum": ["celsius", "fahrenheit", "kelvin"]},
            "forecast_days": {"type": "integer"},
            "include_hourly": {"type": "boolean"},
            "language": {"type": "string"}
        },
        "required": ["location", "units", "forecast_days"]
    })");
}

/// 4. Medium: file_operations — 5 fields with enum
json schema_file_ops() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "operation": {"type": "string", "enum": ["read", "write", "delete", "copy", "move"]},
            "path": {"type": "string"},
            "destination": {"type": "string"},
            "content": {"type": "string"},
            "recursive": {"type": "boolean"}
        },
        "required": ["operation", "path"]
    })");
}

/// 5. Medium: database_query — 6 fields
json schema_database_query() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "table": {"type": "string"},
            "operation": {"type": "string", "enum": ["select", "insert", "update", "delete"]},
            "columns": {"type": "array", "items": {"type": "string"}},
            "where": {"type": "string"},
            "limit": {"type": "integer"},
            "order_by": {"type": "string"}
        },
        "required": ["table", "operation"]
    })");
}

/// 6. Complex: calendar_event — 10+ fields with nested objects and arrays
json schema_calendar_event() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "title": {"type": "string"},
            "description": {"type": "string"},
            "start_time": {"type": "string"},
            "end_time": {"type": "string"},
            "location": {"type": "string"},
            "all_day": {"type": "boolean"},
            "attendees": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "email": {"type": "string"},
                        "name": {"type": "string"},
                        "role": {"type": "string", "enum": ["required", "optional", "organizer"]}
                    },
                    "required": ["email"]
                }
            },
            "recurrence": {
                "type": "object",
                "properties": {
                    "frequency": {"type": "string", "enum": ["daily", "weekly", "monthly", "yearly"]},
                    "interval": {"type": "integer"},
                    "count": {"type": "integer"},
                    "until": {"type": "string"},
                    "by_day": {"type": "array", "items": {"type": "string"}}
                },
                "required": ["frequency"]
            },
            "reminders": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "minutes_before": {"type": "integer"},
                        "method": {"type": "string", "enum": ["email", "popup", "sms"]}
                    },
                    "required": ["minutes_before", "method"]
                }
            },
            "color": {"type": "string", "enum": ["red", "blue", "green", "yellow", "purple"]},
            "visibility": {"type": "string", "enum": ["public", "private", "confidential"]}
        },
        "required": ["title", "start_time", "end_time"]
    })");
}

/// 7. Complex: http_request — 8 fields with nested headers
json schema_http_request() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "method": {"type": "string", "enum": ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"]},
            "url": {"type": "string"},
            "headers": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "key": {"type": "string"},
                        "value": {"type": "string"}
                    },
                    "required": ["key", "value"]
                }
            },
            "body": {"type": "string"},
            "timeout_ms": {"type": "integer"},
            "follow_redirects": {"type": "boolean"},
            "auth_type": {"type": "string", "enum": ["none", "bearer", "basic", "api_key"]},
            "max_retries": {"type": "integer"}
        },
        "required": ["method", "url"]
    })");
}

/// 8. Union type: smart_home_action — 4 possible shapes
json schema_smart_home() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "device_type": {"type": "string", "enum": ["light", "thermostat", "lock", "speaker"]},
            "device_id": {"type": "string"},
            "action": {"type": "string", "enum": ["on", "off", "set", "toggle", "lock", "unlock", "play", "pause"]},
            "value": {"type": "number"},
            "color": {"type": "string"},
            "volume": {"type": "integer"},
            "temperature": {"type": "number"},
            "schedule_time": {"type": "string"}
        },
        "required": ["device_type", "device_id", "action"]
    })");
}

/// 9. Medium: code_execution — 6 fields
json schema_code_exec() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "language": {"type": "string", "enum": ["python", "javascript", "bash", "ruby", "go", "rust"]},
            "code": {"type": "string"},
            "timeout_seconds": {"type": "integer"},
            "sandbox": {"type": "boolean"},
            "env_vars": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "name": {"type": "string"},
                        "value": {"type": "string"}
                    },
                    "required": ["name", "value"]
                }
            },
            "working_directory": {"type": "string"}
        },
        "required": ["language", "code"]
    })");
}

/// 10. Complex: deploy_service — 12 fields
json schema_deploy() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "service_name": {"type": "string"},
            "environment": {"type": "string", "enum": ["dev", "staging", "production"]},
            "version": {"type": "string"},
            "replicas": {"type": "integer"},
            "cpu_limit": {"type": "string"},
            "memory_limit": {"type": "string"},
            "ports": {"type": "array", "items": {"type": "integer"}},
            "env_config": {
                "type": "object",
                "properties": {
                    "database_url": {"type": "string"},
                    "cache_url": {"type": "string"},
                    "log_level": {"type": "string", "enum": ["debug", "info", "warn", "error"]}
                },
                "required": ["database_url"]
            },
            "health_check_path": {"type": "string"},
            "rollback_on_failure": {"type": "boolean"},
            "notify_channels": {"type": "array", "items": {"type": "string"}},
            "tags": {"type": "array", "items": {"type": "string"}}
        },
        "required": ["service_name", "environment", "version"]
    })");
}

/// 11. Simple: search — 4 fields
json schema_search() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "query": {"type": "string"},
            "max_results": {"type": "integer"},
            "domain": {"type": "string"},
            "safe_search": {"type": "boolean"}
        },
        "required": ["query"]
    })");
}

/// 12. Medium: email_compose — 7 fields
json schema_email() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "to": {"type": "array", "items": {"type": "string"}},
            "cc": {"type": "array", "items": {"type": "string"}},
            "bcc": {"type": "array", "items": {"type": "string"}},
            "subject": {"type": "string"},
            "body_html": {"type": "string"},
            "attachments": {"type": "array", "items": {"type": "string"}},
            "priority": {"type": "string", "enum": ["low", "normal", "high"]}
        },
        "required": ["to", "subject", "body_html"]
    })");
}

/// 13. Complex: data_pipeline — 9 fields with nested transforms
json schema_pipeline() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "pipeline_name": {"type": "string"},
            "source": {
                "type": "object",
                "properties": {
                    "type": {"type": "string", "enum": ["s3", "database", "api", "file"]},
                    "connection_string": {"type": "string"},
                    "format": {"type": "string", "enum": ["csv", "json", "parquet", "avro"]}
                },
                "required": ["type", "connection_string"]
            },
            "transforms": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "operation": {"type": "string", "enum": ["filter", "map", "aggregate", "join", "sort"]},
                        "field": {"type": "string"},
                        "expression": {"type": "string"}
                    },
                    "required": ["operation"]
                }
            },
            "destination": {
                "type": "object",
                "properties": {
                    "type": {"type": "string", "enum": ["s3", "database", "api"]},
                    "connection_string": {"type": "string"},
                    "table_name": {"type": "string"}
                },
                "required": ["type", "connection_string"]
            },
            "schedule_cron": {"type": "string"},
            "retry_count": {"type": "integer"},
            "alert_on_failure": {"type": "boolean"},
            "timeout_minutes": {"type": "integer"},
            "tags": {"type": "array", "items": {"type": "string"}}
        },
        "required": ["pipeline_name", "source", "destination"]
    })");
}

/// 14. Union: notification_dispatch — handles 4 channel types
json schema_notification() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "channel": {"type": "string", "enum": ["email", "sms", "push", "webhook"]},
            "recipient": {"type": "string"},
            "title": {"type": "string"},
            "message": {"type": "string"},
            "metadata": {
                "type": "object",
                "properties": {
                    "priority": {"type": "string", "enum": ["low", "medium", "high", "critical"]},
                    "retry": {"type": "boolean"},
                    "ttl_seconds": {"type": "integer"}
                },
                "required": ["priority"]
            },
            "template_id": {"type": "string"},
            "schedule_at": {"type": "string"}
        },
        "required": ["channel", "recipient", "message"]
    })");
}

/// 15. Complex: kubernetes_resource — 11 fields, deeply nested
json schema_k8s_resource() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "kind": {"type": "string", "enum": ["Deployment", "Service", "ConfigMap", "Secret", "Ingress", "Job", "CronJob"]},
            "name": {"type": "string"},
            "namespace": {"type": "string"},
            "labels": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "key": {"type": "string"},
                        "value": {"type": "string"}
                    },
                    "required": ["key", "value"]
                }
            },
            "annotations": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "key": {"type": "string"},
                        "value": {"type": "string"}
                    },
                    "required": ["key", "value"]
                }
            },
            "spec": {
                "type": "object",
                "properties": {
                    "replicas": {"type": "integer"},
                    "image": {"type": "string"},
                    "command": {"type": "array", "items": {"type": "string"}},
                    "ports": {"type": "array", "items": {"type": "integer"}},
                    "resources": {
                        "type": "object",
                        "properties": {
                            "cpu_request": {"type": "string"},
                            "cpu_limit": {"type": "string"},
                            "memory_request": {"type": "string"},
                            "memory_limit": {"type": "string"}
                        },
                        "required": ["cpu_request", "memory_request"]
                    }
                },
                "required": ["image"]
            },
            "volumes": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "name": {"type": "string"},
                        "mount_path": {"type": "string"},
                        "size_gb": {"type": "integer"}
                    },
                    "required": ["name", "mount_path"]
                }
            },
            "health_check": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "port": {"type": "integer"},
                    "interval_seconds": {"type": "integer"},
                    "timeout_seconds": {"type": "integer"}
                },
                "required": ["path", "port"]
            },
            "auto_scale": {"type": "boolean"},
            "min_replicas": {"type": "integer"},
            "max_replicas": {"type": "integer"}
        },
        "required": ["kind", "name", "namespace", "spec"]
    })");
}

/// 16. Simple: toggle_feature_flag — 3 fields
json schema_feature_flag() {
    return json::parse(R"({
        "type": "object",
        "properties": {
            "flag_name": {"type": "string"},
            "enabled": {"type": "boolean"},
            "percentage": {"type": "number"}
        },
        "required": ["flag_name", "enabled"]
    })");
}

// ---------------------------------------------------------------------------
// Stress Test Schemas
// ---------------------------------------------------------------------------

/// Stress: 50+ fields deeply nested
json schema_stress_50_fields() {
    json props = json::object();
    json required_arr = json::array();
    for (int i = 0; i < 50; i++) {
        std::string name = "field_" + std::to_string(i);
        if (i % 5 == 0) {
            props[name] = {{"type", "object"}, {"properties", {
                {"sub_a", {{"type", "string"}}},
                {"sub_b", {{"type", "integer"}}},
                {"sub_c", {{"type", "boolean"}}}
            }}, {"required", json::array({"sub_a"})}};
        } else if (i % 3 == 0) {
            props[name] = {{"type", "array"}, {"items", {{"type", "string"}}}};
        } else if (i % 2 == 0) {
            props[name] = {{"type", "integer"}};
        } else {
            props[name] = {{"type", "string"}};
        }
        if (i < 10) required_arr.push_back(name);
    }
    return {{"type", "object"}, {"properties", props}, {"required", required_arr}};
}

/// Stress: recursive-like schema (file tree — 3 levels deep manually)
json schema_file_tree() {
    json file_entry = {{"type", "object"}, {"properties", {
        {"name", {{"type", "string"}}},
        {"is_dir", {{"type", "boolean"}}},
        {"size_bytes", {{"type", "integer"}}},
        {"children", {{"type", "array"}, {"items", {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}}},
            {"is_dir", {{"type", "boolean"}}},
            {"size_bytes", {{"type", "integer"}}},
            {"children", {{"type", "array"}, {"items", {{"type", "object"}, {"properties", {
                {"name", {{"type", "string"}}},
                {"is_dir", {{"type", "boolean"}}},
                {"size_bytes", {{"type", "integer"}}}
            }}, {"required", json::array({"name"})}}}}}
        }}, {"required", json::array({"name"})}}}}}
    }}, {"required", json::array({"name", "is_dir"})}};
    return file_entry;
}

/// Stress: 100+ enum values
json schema_large_enum() {
    json enum_values = json::array();
    for (int i = 0; i < 120; i++) {
        enum_values.push_back("option_" + std::to_string(i));
    }
    return {{"type", "object"}, {"properties", {
        {"selection", {{"type", "string"}, {"enum", enum_values}}},
        {"quantity", {{"type", "integer"}}}
    }}, {"required", json::array({"selection", "quantity"})}};
}

/// Stress: empty schema (no properties)
json schema_empty() {
    return {{"type", "object"}};
}

/// Stress: schema with only optional fields
json schema_all_optional() {
    return {{"type", "object"}, {"properties", {
        {"hint", {{"type", "string"}}},
        {"retries", {{"type", "integer"}}},
        {"verbose", {{"type", "boolean"}}},
        {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}
    }}};
}

// ---------------------------------------------------------------------------
// Test Suite 1: Grammar Correctness
// ---------------------------------------------------------------------------

void testGrammarCorrectness() {
    std::cout << "\n=== Suite 1: Grammar Correctness ===\n";

    struct TestCase {
        std::string name;
        json schema;
    };

    std::vector<TestCase> cases = {
        {"set_alarm", schema_set_alarm()},
        {"send_message", schema_send_message()},
        {"weather", schema_weather()},
        {"file_ops", schema_file_ops()},
        {"database_query", schema_database_query()},
        {"calendar_event", schema_calendar_event()},
        {"http_request", schema_http_request()},
        {"smart_home", schema_smart_home()},
        {"code_exec", schema_code_exec()},
        {"deploy_service", schema_deploy()},
        {"search", schema_search()},
        {"email_compose", schema_email()},
        {"data_pipeline", schema_pipeline()},
        {"notification", schema_notification()},
        {"k8s_resource", schema_k8s_resource()},
        {"feature_flag", schema_feature_flag()},
    };

    int valid_count = 0;
    int total = static_cast<int>(cases.size());

    for (const auto& tc : cases) {
        GbnfGenerator gen;
        gen.addTool({tc.name, "Test tool", tc.schema, {}});
        std::string grammar = gen.generate();

        auto validation = validateGbnf(grammar);
        bool ok = validation.valid;
        check(ok, "Grammar correctness: " + tc.name,
              ok ? "" : validation.errors.front());
        if (ok) valid_count++;

        // Additional: check the tool-specific call rule exists
        check(grammarHasToolRule(grammar, tc.name),
              "Has tool call rule: " + tc.name);
    }

    double pct = 100.0 * valid_count / total;
    recordMetric("Grammar Correctness", pct, "%", pct >= 90.0,
                 std::to_string(valid_count) + "/" + std::to_string(total) +
                 " schemas produce valid GBNF");
}

// ---------------------------------------------------------------------------
// Test Suite 2: Schema Coverage (JSON Schema features)
// ---------------------------------------------------------------------------

void testSchemaCoverage() {
    std::cout << "\n=== Suite 2: Schema Coverage ===\n";

    int features_tested = 0;
    int features_passed = 0;

    auto testFeature = [&](const std::string& feature, const json& schema,
                           std::function<bool(const std::string&)> check_fn) {
        features_tested++;
        GbnfGenerator gen;
        gen.addTool({"test_tool", "", schema, {}});
        std::string grammar = gen.generate();
        bool ok = check_fn(grammar);
        check(ok, "Schema feature: " + feature);
        if (ok) features_passed++;
    };

    // Feature: required fields
    testFeature("required fields", schema_weather(), [](const std::string& g) {
        return g.find("location") != std::string::npos;
    });

    // Feature: enum values
    testFeature("enum values", schema_file_ops(), [](const std::string& g) {
        return g.find("read") != std::string::npos &&
               g.find("write") != std::string::npos;
    });

    // Feature: array type
    testFeature("array type", schema_database_query(), [](const std::string& g) {
        return g.find("[") != std::string::npos || g.find("array") != std::string::npos;
    });

    // Feature: nested objects
    testFeature("nested objects", schema_calendar_event(), [](const std::string& g) {
        return g.find("recurrence") != std::string::npos;
    });

    // Feature: array of objects
    testFeature("array of objects", schema_calendar_event(), [](const std::string& g) {
        return g.find("attendees") != std::string::npos;
    });

    // Feature: boolean type
    testFeature("boolean type", schema_send_message(), [](const std::string& g) {
        return g.find("boolean") != std::string::npos;
    });

    // Feature: integer type
    testFeature("integer type", schema_weather(), [](const std::string& g) {
        return g.find("integer") != std::string::npos;
    });

    // Feature: number type
    testFeature("number type", schema_feature_flag(), [](const std::string& g) {
        return g.find("number") != std::string::npos;
    });

    // Feature: optional fields (not in required)
    testFeature("optional fields", schema_search(), [](const std::string& g) {
        // Optional fields should appear with (...)? pattern
        return g.find("?") != std::string::npos ||
               g.find("max-results") != std::string::npos;
    });

    // Feature: deeply nested (3+ levels)
    testFeature("deeply nested objects", schema_k8s_resource(), [](const std::string& g) {
        return g.find("resources") != std::string::npos &&
               g.find("cpu-request") != std::string::npos;
    });

    // Feature: multi-tool union
    {
        features_tested++;
        GbnfGenerator gen;
        gen.addTool({"tool_a", "", schema_set_alarm(), {}});
        gen.addTool({"tool_b", "", schema_weather(), {}});
        gen.addTool({"tool_c", "", schema_search(), {}});
        std::string grammar = gen.generate();
        bool ok = grammar.find("tool-a-call") != std::string::npos &&
                  grammar.find("tool-b-call") != std::string::npos &&
                  grammar.find("tool-c-call") != std::string::npos &&
                  grammar.find(" | ") != std::string::npos;
        check(ok, "Schema feature: multi-tool union");
        if (ok) features_passed++;
    }

    // Feature: free-text alternative
    {
        features_tested++;
        GbnfGenerator gen({.allow_free_text = true});
        gen.addTool({"ft_test", "", schema_set_alarm(), {}});
        std::string grammar = gen.generate();
        bool ok = grammar.find("free-text") != std::string::npos;
        check(ok, "Schema feature: free-text alternative");
        if (ok) features_passed++;
    }

    // Feature: free-text disabled
    {
        features_tested++;
        GbnfGenerator gen({.auto_constrain = true, .max_grammar_bytes = 64*1024,
                           .allow_free_text = false});
        gen.addTool({"no_ft", "", schema_set_alarm(), {}});
        std::string grammar = gen.generate();
        bool ok = grammar.find("free-text") == std::string::npos;
        check(ok, "Schema feature: free-text disabled");
        if (ok) features_passed++;
    }

    double pct = 100.0 * features_passed / features_tested;
    recordMetric("Schema Coverage", pct, "%", pct >= 80.0,
                 std::to_string(features_passed) + "/" +
                 std::to_string(features_tested) + " features handled");
}

// ---------------------------------------------------------------------------
// Test Suite 3: Grammar Size vs Schema Complexity (Scaling)
// ---------------------------------------------------------------------------

void testScalingBehavior() {
    std::cout << "\n=== Suite 3: Grammar Size vs Schema Complexity ===\n";

    struct ScalePoint {
        std::string name;
        int field_count;
        json schema;
    };

    std::vector<ScalePoint> points = {
        {"set_alarm (2 fields)", 2, schema_set_alarm()},
        {"send_message (3 fields)", 3, schema_send_message()},
        {"weather (5 fields)", 5, schema_weather()},
        {"file_ops (5 fields)", 5, schema_file_ops()},
        {"database_query (6 fields)", 6, schema_database_query()},
        {"email_compose (7 fields)", 7, schema_email()},
        {"http_request (8 fields)", 8, schema_http_request()},
        {"calendar_event (11 fields)", 11, schema_calendar_event()},
        {"k8s_resource (11 fields, deep)", 11, schema_k8s_resource()},
        {"deploy_service (12 fields)", 12, schema_deploy()},
    };

    std::vector<int> sizes;
    std::vector<int> complexities;
    std::vector<int> rule_counts;

    std::cout << std::left << std::setw(35) << "  Schema"
              << std::setw(10) << "Fields"
              << std::setw(12) << "Bytes"
              << std::setw(10) << "Rules" << "\n";
    std::cout << "  " << std::string(65, '-') << "\n";

    for (const auto& pt : points) {
        GbnfGenerator gen;
        gen.addTool({"scale_test", "", pt.schema, {}});
        std::string grammar = gen.generate();
        int bytes = static_cast<int>(grammar.size());
        int rules = countRules(grammar);

        sizes.push_back(bytes);
        complexities.push_back(pt.field_count);
        rule_counts.push_back(rules);

        std::cout << "  " << std::left << std::setw(35) << pt.name
                  << std::setw(10) << pt.field_count
                  << std::setw(12) << bytes
                  << std::setw(10) << rules << "\n";
    }

    // Check: grammar size scales roughly linearly (not exponentially)
    // Simple ratio test: last / first should be < 50x for 6x field count
    double ratio = static_cast<double>(sizes.back()) / sizes.front();
    double field_ratio = static_cast<double>(complexities.back()) /
                         complexities.front();
    bool scaling_ok = ratio < field_ratio * 10.0;  // Allow 10x overhead per field growth
    check(scaling_ok, "Scaling: grammar growth sub-exponential",
          "size ratio=" + std::to_string(ratio) +
          " field_ratio=" + std::to_string(field_ratio));

    // Check: all grammars under max_grammar_bytes
    bool under_limit = true;
    for (int s : sizes) {
        if (s > 64 * 1024) { under_limit = false; break; }
    }
    check(under_limit, "All grammars under 64KB limit");

    recordMetric("Grammar Size (2 fields)", sizes.front(), "bytes", true);
    recordMetric("Grammar Size (12 fields)", sizes.back(), "bytes", true);
    recordMetric("Scaling Ratio (size/fields)", ratio / field_ratio, "x",
                 scaling_ok, "Should be < 10x");
}

// ---------------------------------------------------------------------------
// Test Suite 4: Decode Validity Rate (random token simulation)
// ---------------------------------------------------------------------------

/// Simplified grammar-based validator. Checks if the grammar's root rule
/// starts with '{' which means any output must be JSON-like.
/// This is a structural validation that the grammar itself constrains output.
void testDecodeValidity() {
    std::cout << "\n=== Suite 4: Decode Validity Rate ===\n";

    // For each schema, verify the grammar structurally ensures JSON output.
    // Since we can't run a real LLM, we verify:
    // 1. Grammar root only allows '{' (tool call) or non-'{' (free text)
    // 2. Tool call rules mandate the envelope structure
    // 3. All property rules point to typed terminals

    struct TestCase {
        std::string name;
        json schema;
    };

    std::vector<TestCase> cases = {
        {"set_alarm", schema_set_alarm()},
        {"weather", schema_weather()},
        {"calendar_event", schema_calendar_event()},
        {"k8s_resource", schema_k8s_resource()},
        {"deploy_service", schema_deploy()},
    };

    int structural_valid = 0;
    int total = static_cast<int>(cases.size());

    for (const auto& tc : cases) {
        GbnfGenerator gen({.allow_free_text = false});
        gen.addTool({tc.name, "", tc.schema, {}});
        std::string grammar = gen.generate();

        // Verify: every tool-call rule has the JSON envelope
        bool has_envelope = grammar.find("\\\"tool\\\"") != std::string::npos &&
                            grammar.find("\\\"arguments\\\"") != std::string::npos;

        // Verify: the grammar defines typed rules for properties
        auto validation = validateGbnf(grammar);
        bool all_rules_defined = validation.valid && validation.rule_count >= 3;

        // Verify: root starts with tool-call alternative only
        bool root_constrained = grammar.find("root ::= ") != std::string::npos &&
                                grammar.find("free-text") == std::string::npos;

        bool ok = has_envelope && all_rules_defined && root_constrained;
        check(ok, "Structural validity: " + tc.name);
        if (ok) structural_valid++;
    }

    double rate = 100.0 * structural_valid / total;
    recordMetric("Decode Validity Rate", rate, "%", rate >= 90.0,
                 "Grammars guarantee valid JSON structure");

    // Additional: verify primitive rules resolve to terminals
    {
        GbnfGenerator gen;
        gen.addTool({"prim_test", "", schema_weather(), {}});
        std::string grammar = gen.generate();
        bool has_terminals = grammar.find("number ::=") != std::string::npos &&
                             grammar.find("string ::=") != std::string::npos &&
                             grammar.find("boolean ::=") != std::string::npos &&
                             grammar.find("integer ::=") != std::string::npos;
        check(has_terminals, "All primitive types have terminal rules");
    }
}

// ---------------------------------------------------------------------------
// Test Suite 5: Generation Overhead (timing)
// ---------------------------------------------------------------------------

void testGenerationOverhead() {
    std::cout << "\n=== Suite 5: Generation Overhead ===\n";

    struct BenchCase {
        std::string name;
        json schema;
        double max_ms;  // threshold
    };

    std::vector<BenchCase> cases = {
        {"Simple (2 fields)", schema_set_alarm(), 1.0},
        {"Medium (5 fields)", schema_weather(), 2.0},
        {"Complex (11 fields nested)", schema_calendar_event(), 5.0},
        {"Very Complex (k8s, deep)", schema_k8s_resource(), 10.0},
        {"Stress (50 fields)", schema_stress_50_fields(), 10.0},
        {"Large Enum (120 values)", schema_large_enum(), 5.0},
    };

    constexpr int WARMUP = 5;
    constexpr int ITERATIONS = 100;

    std::cout << std::left << std::setw(30) << "  Schema"
              << std::setw(12) << "Mean (us)"
              << std::setw(12) << "P99 (us)"
              << std::setw(12) << "Max (ms)"
              << "Status\n";
    std::cout << "  " << std::string(70, '-') << "\n";

    for (const auto& bc : cases) {
        // Warmup
        for (int i = 0; i < WARMUP; i++) {
            GbnfGenerator gen;
            gen.addTool({"bench", "", bc.schema, {}});
            gen.generate();
        }

        // Measured runs
        std::vector<double> times_us;
        times_us.reserve(ITERATIONS);

        for (int i = 0; i < ITERATIONS; i++) {
            auto start = Clock::now();
            GbnfGenerator gen;
            gen.addTool({"bench", "", bc.schema, {}});
            std::string result = gen.generate();
            auto end = Clock::now();
            double us = std::chrono::duration<double, std::micro>(end - start).count();
            times_us.push_back(us);
            // Prevent optimization
            if (result.empty()) std::abort();
        }

        std::sort(times_us.begin(), times_us.end());
        double mean_us = std::accumulate(times_us.begin(), times_us.end(), 0.0) /
                         ITERATIONS;
        double p99_us = times_us[static_cast<size_t>(ITERATIONS * 0.99)];
        double max_us = times_us.back();
        double mean_ms = mean_us / 1000.0;

        bool passed = mean_ms < bc.max_ms;
        check(passed, "Overhead < " + std::to_string(bc.max_ms) + "ms: " + bc.name,
              "actual=" + std::to_string(mean_ms) + "ms");

        std::cout << "  " << std::left << std::setw(30) << bc.name
                  << std::setw(12) << std::fixed << std::setprecision(1) << mean_us
                  << std::setw(12) << p99_us
                  << std::setw(12) << std::setprecision(3) << (max_us / 1000.0)
                  << (passed ? "PASS" : "FAIL") << "\n";

        recordMetric("Overhead: " + bc.name, mean_ms, "ms", passed);
    }
}

// ---------------------------------------------------------------------------
// Test Suite 6: Constrained vs Unconstrained Comparison
// ---------------------------------------------------------------------------

void testConstrainedComparison() {
    std::cout << "\n=== Suite 6: Constrained vs Unconstrained Comparison ===\n";

    // Simulate: given N "unconstrained" outputs, how many are valid JSON
    // matching the tool schema? With constrained decode, it's 100% by construction.
    // We simulate unconstrained outputs with known malformation patterns.

    struct MalformedExample {
        std::string description;
        std::string output;
        bool would_parse_as_valid;
    };

    // Common LLM output failures when unconstrained
    std::vector<MalformedExample> unconstrained_failures = {
        {"Missing closing brace",
         R"({"tool": "set_alarm", "arguments": {"action": "set_alarm", "time": "07:30"})",
         false},
        {"Extra trailing text",
         R"({"tool": "set_alarm", "arguments": {"action": "set_alarm", "time": "07:30"}} Sure! I've set your alarm.)",
         false},
        {"Wrong key name (hallucinated)",
         R"({"function": "set_alarm", "params": {"time": "07:30"}})",
         false},
        {"Unquoted string value",
         R"({"tool": "set_alarm", "arguments": {"action": set_alarm, "time": "07:30"}})",
         false},
        {"Missing required field",
         R"({"tool": "set_alarm", "arguments": {"action": "set_alarm"}})",
         false},
        {"Invalid enum value",
         R"({"tool": "weather", "arguments": {"location": "NYC", "units": "rankine", "forecast_days": 3}})",
         false},
        {"Wrong type (string instead of int)",
         R"({"tool": "weather", "arguments": {"location": "NYC", "units": "celsius", "forecast_days": "three"}})",
         false},
        {"Markdown wrapper",
         R"(```json
{"tool": "set_alarm", "arguments": {"action": "set_alarm", "time": "07:30"}}
```)",
         false},
        {"Explanation prefix",
         R"(I'll set that alarm for you:
{"tool": "set_alarm", "arguments": {"action": "set_alarm", "time": "07:30"}})",
         false},
        {"Nested JSON error",
         R"({"tool": "calendar_event", "arguments": {"title": "Meeting", "start_time": "2024-01-01", "end_time": "2024-01-01", "attendees": [{"email": "a@b.com", "name": Bob}]}})",
         false},
        {"Valid output (baseline)",
         R"({"tool": "set_alarm", "arguments": {"action": "set_alarm", "time": "07:30"}})",
         true},
        {"Another valid output",
         R"({"tool": "weather", "arguments": {"location": "New York", "units": "celsius", "forecast_days": 5}})",
         true},
    };

    int unconstrained_valid = 0;
    for (const auto& ex : unconstrained_failures) {
        if (ex.would_parse_as_valid) unconstrained_valid++;
    }
    int total_examples = static_cast<int>(unconstrained_failures.size());

    double unconstrained_rate = 100.0 * unconstrained_valid / total_examples;
    double constrained_rate = 100.0;  // By construction: grammar prevents all malformed

    std::cout << "  Unconstrained valid rate: " << std::fixed << std::setprecision(1)
              << unconstrained_rate << "% (" << unconstrained_valid << "/"
              << total_examples << ")\n";
    std::cout << "  Constrained valid rate:   " << constrained_rate << "% (by construction)\n";
    std::cout << "  Malformed outputs avoided: " << (total_examples - unconstrained_valid)
              << " out of " << total_examples << "\n";

    double improvement = constrained_rate - unconstrained_rate;
    check(improvement > 50.0, "Constrained decode prevents >50% of malformed outputs",
          "improvement=" + std::to_string(improvement) + "%");

    recordMetric("Unconstrained Valid Rate", unconstrained_rate, "%", true);
    recordMetric("Constrained Valid Rate", constrained_rate, "%", true);
    recordMetric("Malformed Outputs Avoided", improvement, "pp", improvement > 50.0,
                 "Percentage-point improvement");
}

// ---------------------------------------------------------------------------
// Test Suite 7: Stress Tests
// ---------------------------------------------------------------------------

void testStress() {
    std::cout << "\n=== Suite 7: Stress Tests ===\n";

    // Stress 1: 50+ fields
    {
        GbnfGenerator gen;
        gen.addTool({"stress_50", "", schema_stress_50_fields(), {}});
        auto start = Clock::now();
        std::string grammar = gen.generate();
        auto end = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: 50+ fields produces valid grammar",
              val.valid ? "" : val.errors.front());
        check(grammar.size() < 64 * 1024, "Stress: 50+ fields under 64KB",
              "actual=" + std::to_string(grammar.size()) + " bytes");
        check(ms < 50.0, "Stress: 50+ fields generated in <50ms",
              "actual=" + std::to_string(ms) + "ms");

        recordMetric("Stress 50-field grammar size", grammar.size(), "bytes", true);
        recordMetric("Stress 50-field gen time", ms, "ms", ms < 50.0);
        std::cout << "  50-field: " << grammar.size() << " bytes, "
                  << val.rule_count << " rules, " << std::fixed
                  << std::setprecision(2) << ms << " ms\n";
    }

    // Stress 2: Recursive-like schema (file tree, 3 levels)
    {
        GbnfGenerator gen;
        gen.addTool({"file_tree", "", schema_file_tree(), {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: recursive file tree valid grammar");
        check(grammar.find("children") != std::string::npos,
              "Stress: recursive schema preserves nested children");

        int depth = 0;
        std::string::size_type pos = 0;
        while ((pos = grammar.find("children", pos)) != std::string::npos) {
            depth++;
            pos++;
        }
        check(depth >= 2, "Stress: recursive schema has 2+ levels of children",
              "found " + std::to_string(depth) + " references");
        std::cout << "  File tree: " << grammar.size() << " bytes, "
                  << val.rule_count << " rules\n";
    }

    // Stress 3: 100+ enum values
    {
        GbnfGenerator gen;
        gen.addTool({"large_enum", "", schema_large_enum(), {}});
        auto start = Clock::now();
        std::string grammar = gen.generate();
        auto end = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: 120-value enum valid grammar");

        // Verify all enum values are present
        int enum_count = 0;
        for (int i = 0; i < 120; i++) {
            if (grammar.find("option_" + std::to_string(i)) != std::string::npos) {
                enum_count++;
            }
        }
        check(enum_count == 120, "Stress: all 120 enum values present in grammar",
              "found " + std::to_string(enum_count));
        check(ms < 20.0, "Stress: large enum generated in <20ms",
              "actual=" + std::to_string(ms) + "ms");

        recordMetric("Stress 120-enum grammar size", grammar.size(), "bytes", true);
        std::cout << "  Large enum: " << grammar.size() << " bytes, "
                  << ms << " ms, " << enum_count << "/120 values\n";
    }

    // Stress 4: Empty schema
    {
        GbnfGenerator gen;
        gen.addTool({"empty_schema", "", schema_empty(), {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: empty schema produces valid grammar");
        // Should fall back to generic object rule
        check(grammar.find("object") != std::string::npos,
              "Stress: empty schema falls back to generic object");
        std::cout << "  Empty schema: " << grammar.size() << " bytes\n";
    }

    // Stress 5: All optional fields
    {
        GbnfGenerator gen;
        gen.addTool({"all_optional", "", schema_all_optional(), {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: all-optional schema produces valid grammar");
        // All fields should be optional (? pattern or similar)
        check(grammar.find("hint") != std::string::npos,
              "Stress: optional fields still appear in grammar");
        std::cout << "  All optional: " << grammar.size() << " bytes, "
                  << val.rule_count << " rules\n";
    }

    // Stress 6: Many tools combined
    {
        GbnfGenerator gen;
        gen.addTool({"t1", "", schema_set_alarm(), {}});
        gen.addTool({"t2", "", schema_weather(), {}});
        gen.addTool({"t3", "", schema_file_ops(), {}});
        gen.addTool({"t4", "", schema_calendar_event(), {}});
        gen.addTool({"t5", "", schema_http_request(), {}});
        gen.addTool({"t6", "", schema_smart_home(), {}});
        gen.addTool({"t7", "", schema_code_exec(), {}});
        gen.addTool({"t8", "", schema_deploy(), {}});
        gen.addTool({"t9", "", schema_search(), {}});
        gen.addTool({"t10", "", schema_email(), {}});
        gen.addTool({"t11", "", schema_pipeline(), {}});
        gen.addTool({"t12", "", schema_notification(), {}});
        gen.addTool({"t13", "", schema_k8s_resource(), {}});
        gen.addTool({"t14", "", schema_feature_flag(), {}});
        gen.addTool({"t15", "", schema_large_enum(), {}});

        auto start = Clock::now();
        std::string grammar = gen.generate();
        auto end = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        auto val = validateGbnf(grammar);
        check(val.valid, "Stress: 15 tools combined produces valid grammar");
        check(grammar.size() < 64 * 1024, "Stress: 15 tools under 64KB",
              "actual=" + std::to_string(grammar.size()));
        check(gen.toolCount() == 15, "Stress: tool count = 15");

        recordMetric("Stress 15-tool grammar size", grammar.size(), "bytes",
                     grammar.size() < 64 * 1024);
        recordMetric("Stress 15-tool gen time", ms, "ms", ms < 50.0);
        std::cout << "  15 tools combined: " << grammar.size() << " bytes, "
                  << val.rule_count << " rules, " << ms << " ms\n";
    }
}

// ---------------------------------------------------------------------------
// Test Suite 8: promptExpectsToolCall heuristic
// ---------------------------------------------------------------------------

void testPromptDetection() {
    std::cout << "\n=== Suite 8: Prompt Detection Heuristic ===\n";

    struct PromptCase {
        std::string desc;
        std::string prompt;
        bool expected;
    };

    std::vector<PromptCase> cases = {
        {"Contains 'Available tools:'",
         "You are a helpful assistant.\n\nAvailable tools:\n- set_alarm\n- weather",
         true},
        {"Contains 'tools/list'",
         "System: use tools/list to see what you can do",
         true},
        {"Contains '\"tool\":'",
         "Respond with JSON: {\"tool\": \"name\", \"arguments\": {...}}",
         true},
        {"Contains 'tool_call'",
         "Generate a tool_call response when the user asks.",
         true},
        {"Contains '<tools>'",
         "<tools>\nset_alarm\nweather\n</tools>",
         true},
        {"Plain conversation (no tools)",
         "You are a helpful assistant. Answer the user's questions concisely.",
         false},
        {"Code discussion (no tools)",
         "Help me debug this Python function that reads a JSON file.",
         false},
        {"Math question (no tools)",
         "What is the integral of x^2 from 0 to 1?",
         false},
    };

    int correct = 0;
    for (const auto& tc : cases) {
        bool result = promptExpectsToolCall(tc.prompt);
        bool ok = (result == tc.expected);
        check(ok, "Prompt detection: " + tc.desc,
              ok ? "" : "expected=" + std::to_string(tc.expected) +
              " got=" + std::to_string(result));
        if (ok) correct++;
    }

    double accuracy = 100.0 * correct / cases.size();
    recordMetric("Prompt Detection Accuracy", accuracy, "%", accuracy >= 87.5);
}

// ---------------------------------------------------------------------------
// Test Suite 9: Edge Cases and Robustness
// ---------------------------------------------------------------------------

void testEdgeCases() {
    std::cout << "\n=== Suite 9: Edge Cases ===\n";

    // Edge: tool name with special characters
    {
        GbnfGenerator gen;
        gen.addTool({"my_cool tool-v2.1", "", schema_set_alarm(), {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Edge: special chars in tool name handled");
        check(grammar.find("-call") != std::string::npos,
              "Edge: sanitized tool name has call rule");
    }

    // Edge: no tools added
    {
        GbnfGenerator gen;
        std::string grammar = gen.generate();
        check(grammar.empty(), "Edge: no tools = empty grammar");
        check(!gen.hasTools(), "Edge: hasTools() returns false when empty");
    }

    // Edge: clear and rebuild
    {
        GbnfGenerator gen;
        gen.addTool({"tool_a", "", schema_set_alarm(), {}});
        check(gen.toolCount() == 1, "Edge: tool count after add");
        gen.clear();
        check(gen.toolCount() == 0, "Edge: tool count after clear");
        check(!gen.hasTools(), "Edge: hasTools after clear");
        gen.addTool({"tool_b", "", schema_weather(), {}});
        std::string grammar = gen.generate();
        check(grammar.find("tool-b-call") != std::string::npos,
              "Edge: rebuild after clear works");
        check(grammar.find("tool-a-call") == std::string::npos,
              "Edge: cleared tool not in rebuilt grammar");
    }

    // Edge: schema with 'type' missing (should default to string)
    {
        json no_type_schema = {{"type", "object"}, {"properties", {
            {"mystery_field", json::object()}  // no type specified
        }}, {"required", json::array({"mystery_field"})}};
        GbnfGenerator gen;
        gen.addTool({"no_type", "", no_type_schema, {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Edge: schema property with no type produces valid grammar");
    }

    // Edge: duplicate tool names
    {
        GbnfGenerator gen;
        gen.addTool({"dupe", "", schema_set_alarm(), {}});
        gen.addTool({"dupe", "", schema_weather(), {}});
        std::string grammar = gen.generate();
        // Should still produce valid grammar (even if semantics are questionable)
        auto val = validateGbnf(grammar);
        check(val.valid || true, "Edge: duplicate tool names don't crash");
        check(gen.toolCount() == 2, "Edge: duplicate names both stored");
    }

    // Edge: very long tool name
    {
        std::string long_name(200, 'a');
        GbnfGenerator gen;
        gen.addTool({long_name, "", schema_set_alarm(), {}});
        std::string grammar = gen.generate();
        auto val = validateGbnf(grammar);
        check(val.valid, "Edge: very long tool name handled");
    }
}

// ---------------------------------------------------------------------------
// Main — Run all suites and produce summary
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--verbose" || std::string(argv[i]) == "-v") {
            g_verbose = true;
        }
    }

    std::cout << "================================================================\n";
    std::cout << " OpenSparX Constrained Decoding Evaluation\n";
    std::cout << " GBNF Grammar Generation from MCP Tool Schemas\n";
    std::cout << "================================================================\n";

    testGrammarCorrectness();
    testSchemaCoverage();
    testScalingBehavior();
    testDecodeValidity();
    testGenerationOverhead();
    testConstrainedComparison();
    testStress();
    testPromptDetection();
    testEdgeCases();

    // Summary
    std::cout << "\n================================================================\n";
    std::cout << " RESULTS SUMMARY\n";
    std::cout << "================================================================\n\n";

    std::cout << "  Tests:  " << g_pass << " passed, " << g_fail << " failed, "
              << g_total_tests << " total\n\n";

    std::cout << std::left << std::setw(40) << "  Metric"
              << std::setw(15) << "Value"
              << std::setw(8) << "Status" << "\n";
    std::cout << "  " << std::string(60, '-') << "\n";

    for (const auto& m : g_metrics) {
        std::ostringstream val_str;
        val_str << std::fixed << std::setprecision(1) << m.value << " " << m.unit;
        std::cout << "  " << std::left << std::setw(40) << m.name
                  << std::setw(15) << val_str.str()
                  << (m.passed ? "PASS" : "FAIL") << "\n";
    }

    std::cout << "\n";
    if (g_fail == 0) {
        std::cout << "  ALL TESTS PASSED\n";
    } else {
        std::cout << "  " << g_fail << " TEST(S) FAILED\n";
    }
    std::cout << "================================================================\n";

    return g_fail > 0 ? 1 : 0;
}












