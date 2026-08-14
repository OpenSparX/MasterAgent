/**
 * @file sparx_constrained_decode.cpp
 * @brief GBNF grammar generation from MCP tool schemas.
 *
 * Converts JSON Schema → GBNF production rules. The generated grammar
 * ensures the LLM can only output valid tool-call JSON or free text.
 */

#include "sparx_constrained_decode.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace sparx::constrained {

namespace {

/// Sanitize a name for use as a GBNF rule identifier (alphanumeric + dash).
std::string sanitizeRuleName(const std::string& name) {
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

/// Standard GBNF primitives shared across all grammars.
const char* kSharedRules = R"(
ws ::= [ \t\n]*
number ::= "-"? [0-9]+ ("." [0-9]+)? (("e" | "E") ("+" | "-")? [0-9]+)?
integer ::= "-"? [0-9]+
string ::= "\"" ([^"\\] | "\\" .)* "\""
boolean ::= "true" | "false"
null ::= "null"
value ::= string | number | boolean | null | object | array
object ::= "{" ws (string ws ":" ws value ws ("," ws string ws ":" ws value ws)*)? "}"
array ::= "[" ws (value ws ("," ws value ws)*)? "]"
)";

}  // namespace

// ---------------------------------------------------------------------------
// GbnfGenerator
// ---------------------------------------------------------------------------

GbnfGenerator::GbnfGenerator(ConstrainedConfig config)
    : config_(std::move(config)) {}

void GbnfGenerator::addTool(const ToolSchema& tool) {
    tools_.push_back(tool);
}

std::string GbnfGenerator::generate() const {
    if (tools_.empty()) return {};

    std::ostringstream grammar;

    // Root rule: union of all tool calls (+ optional free text)
    grammar << "# Auto-generated GBNF grammar for " << tools_.size()
            << " tool(s)\n";
    grammar << "# Constrains LLM output to valid tool-call JSON.\n\n";

    grammar << "root ::= ";
    for (size_t i = 0; i < tools_.size(); ++i) {
        if (i > 0) grammar << " | ";
        grammar << sanitizeRuleName(tools_[i].name) << "-call";
    }
    if (config_.allow_free_text) {
        grammar << " | free-text";
    }
    grammar << "\n\n";

    // Generate rules for each tool
    for (const auto& tool : tools_) {
        auto name = sanitizeRuleName(tool.name);
        auto args_rule = name + "-args";

        // Tool call envelope: {"tool": "<name>", "arguments": {...}}
        grammar << name << "-call ::= \"{\" ws "
                << "\"\\\"tool\\\"\" ws \":\" ws \"\\\"" << tool.name << "\\\"\" ws "
                << "\",\" ws "
                << "\"\\\"arguments\\\"\" ws \":\" ws " << args_rule << " ws "
                << "\"}\"\n";

        // Generate argument rules from input_schema
        if (tool.input_schema.is_object() &&
            tool.input_schema.contains("properties")) {
            grammar << objectToGbnf(args_rule, tool.input_schema);
        } else {
            // No schema or empty: accept any object
            grammar << args_rule << " ::= object\n";
        }
        grammar << "\n";
    }

    // Free text alternative
    if (config_.allow_free_text) {
        grammar << "free-text ::= [^{] [^\\x00]*\n\n";
    }

    // Shared primitive rules
    grammar << kSharedRules;

    return grammar.str();
}

std::string GbnfGenerator::objectToGbnf(
    const std::string& rule_name,
    const nlohmann::json& schema) const {

    std::ostringstream out;
    const auto& props = schema["properties"];
    std::vector<std::string> required;
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& r : schema["required"]) {
            required.push_back(r.get<std::string>());
        }
    }

    // Build the object rule with known property names
    out << rule_name << " ::= \"{\" ws ";

    std::vector<std::string> prop_names;
    for (auto it = props.begin(); it != props.end(); ++it) {
        prop_names.push_back(it.key());
    }

    for (size_t i = 0; i < prop_names.size(); ++i) {
        const auto& pname = prop_names[i];
        auto prop_rule = rule_name + "-" + sanitizeRuleName(pname);
        bool is_required = std::find(required.begin(), required.end(),
                                     pname) != required.end();

        if (i > 0) out << "\",\" ws ";

        if (is_required) {
            out << "\"\\\"" << pname << "\\\"\" ws \":\" ws "
                << prop_rule << " ws ";
        } else {
            // Optional: either present or absent
            out << "(\"\\\"" << pname << "\\\"\" ws \":\" ws "
                << prop_rule << " ws \",\" ws)? ";
        }
    }
    out << "\"}\"\n";

    // Generate sub-rules for each property type
    for (const auto& pname : prop_names) {
        auto prop_rule = rule_name + "-" + sanitizeRuleName(pname);
        const auto& prop_schema = props[pname];
        out << schemaToGbnf(prop_rule, prop_schema);
    }

    return out.str();
}

std::string GbnfGenerator::schemaToGbnf(
    const std::string& rule_name,
    const nlohmann::json& schema) const {

    std::ostringstream out;

    std::string type = "string";  // default
    if (schema.contains("type") && schema["type"].is_string()) {
        type = schema["type"].get<std::string>();
    }

    if (type == "object" && schema.contains("properties")) {
        out << objectToGbnf(rule_name, schema);
    } else if (type == "array") {
        auto item_rule = rule_name + "-item";
        out << rule_name << " ::= \"[\" ws (" << item_rule
            << " ws (\",\" ws " << item_rule << " ws)*)? \"]\"\n";
        if (schema.contains("items")) {
            out << schemaToGbnf(item_rule, schema["items"]);
        } else {
            out << item_rule << " ::= value\n";
        }
    } else if (schema.contains("enum")) {
        // Enum: literal string alternatives
        out << rule_name << " ::= ";
        bool first = true;
        for (const auto& val : schema["enum"]) {
            if (!first) out << " | ";
            first = false;
            if (val.is_string()) {
                out << "\"\\\"" << val.get<std::string>() << "\\\"\"";
            } else {
                out << "\"" << val.dump() << "\"";
            }
        }
        out << "\n";
    } else {
        out << primitiveToGbnf(rule_name, type);
    }

    return out.str();
}

std::string GbnfGenerator::primitiveToGbnf(
    const std::string& rule_name,
    const std::string& type) const {

    std::ostringstream out;
    if (type == "string") {
        out << rule_name << " ::= string\n";
    } else if (type == "number") {
        out << rule_name << " ::= number\n";
    } else if (type == "integer") {
        out << rule_name << " ::= integer\n";
    } else if (type == "boolean") {
        out << rule_name << " ::= boolean\n";
    } else if (type == "null") {
        out << rule_name << " ::= null\n";
    } else {
        // Unknown type: accept any value
        out << rule_name << " ::= value\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::vector<ToolSchema> extractToolSchemas(const std::string& skills_dir) {
    std::vector<ToolSchema> schemas;
    fs::path dir(skills_dir);
    if (!fs::exists(dir)) return schemas;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".yaml" &&
            entry.path().extension() != ".yml") continue;

        // Read YAML and look for mcp_tool + input_schema fields.
        // Minimal YAML parsing: look for key lines.
        std::ifstream in(entry.path());
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

        // Check if this skill has an input_schema JSON block
        auto schema_pos = content.find("input_schema:");
        if (schema_pos == std::string::npos) continue;

        // Extract tool name from filename
        ToolSchema ts;
        ts.name = entry.path().stem().string();

        // Try to parse the schema from the YAML content.
        // In a full implementation this would use a YAML parser;
        // for now, look for an inline JSON block.
        auto json_start = content.find('{', schema_pos);
        if (json_start != std::string::npos) {
            // Find matching closing brace
            int depth = 0;
            size_t json_end = json_start;
            for (size_t i = json_start; i < content.size(); ++i) {
                if (content[i] == '{') ++depth;
                else if (content[i] == '}') {
                    --depth;
                    if (depth == 0) { json_end = i + 1; break; }
                }
            }
            try {
                ts.input_schema = nlohmann::json::parse(
                    content.substr(json_start, json_end - json_start));
                schemas.push_back(std::move(ts));
            } catch (...) {
                // Skip malformed schemas
            }
        }
    }
    return schemas;
}

bool promptExpectsToolCall(const std::string& prompt) {
    // Heuristic: if the prompt contains tool-use markers, the model is
    // expected to respond with a tool call JSON.
    static const std::vector<std::string> markers = {
        "Available tools:",
        "tools/list",
        "\"tool\":",
        "tool_call",
        "function_call",
        "You have access to the following tools",
        "<tools>",
    };
    for (const auto& marker : markers) {
        if (prompt.find(marker) != std::string::npos) return true;
    }
    return false;
}

}  // namespace sparx::constrained
