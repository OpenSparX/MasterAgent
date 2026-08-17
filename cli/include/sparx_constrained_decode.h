/**
 * @file sparx_constrained_decode.h
 * @brief Constrained Decoding — GBNF grammar generation from MCP tool schemas.
 *
 * When tools are registered, this module auto-generates a GBNF grammar that
 * forces the LLM to produce only valid JSON matching the union of all tool
 * call schemas. Result: zero hallucinated tool calls.
 *
 * The grammar is passed to llama-server via the `grammar` field in the
 * /v1/chat/completions request body. llama.cpp's sampler then masks logits
 * at each step, making it impossible to generate tokens outside the grammar.
 *
 * Architecture:
 *   1. Tool schemas (JSON Schema subset) are read from skill YAML or MCP
 *   2. Each tool's input_schema is converted to GBNF production rules
 *   3. A top-level rule unions all tools: root ::= tool1-call | tool2-call
 *   4. The grammar string is injected into inference requests when tool-use
 *      is expected (detected by prompt template)
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sparx::constrained {

/// Represents one tool's schema for grammar generation.
struct ToolSchema {
    std::string name;
    std::string description;
    nlohmann::json input_schema;   // JSON Schema object
    nlohmann::json output_schema;  // optional
};

/// Configuration for constrained decoding behavior.
struct ConstrainedConfig {
    /// When true, all inference calls that follow a tool-use system prompt
    /// automatically have the grammar applied.
    bool auto_constrain = true;
    /// Maximum grammar size (bytes). Very complex schemas may exceed this.
    std::size_t max_grammar_bytes = 64 * 1024;
    /// Allow free-text responses alongside tool calls (adds a text alternative).
    bool allow_free_text = true;
};

/**
 * @brief Generates GBNF grammars from tool schemas.
 *
 * GBNF (GGML BNF) is the grammar format supported by llama.cpp for
 * constrained sampling. It is a variant of BNF with regex-like character
 * classes and repetition operators.
 *
 * Example output for a tool "set_temperature(degrees: number)":
 *
 *   root ::= tool-call | free-text
 *   tool-call ::= "{" ws "\"tool\"" ws ":" ws "\"set_temperature\"" ws ","
 *                 ws "\"arguments\"" ws ":" ws set-temperature-args ws "}"
 *   set-temperature-args ::= "{" ws "\"degrees\"" ws ":" ws number ws "}"
 *   number ::= [0-9]+ ("." [0-9]+)?
 *   ws ::= [ \t\n]*
 *   free-text ::= [^\x00]+
 */
class GbnfGenerator {
public:
    explicit GbnfGenerator(ConstrainedConfig config = {});

    /// Add a tool schema to the grammar.
    void addTool(const ToolSchema& tool);

    /// Generate the complete GBNF grammar string.
    std::string generate() const;

    /// Returns true if at least one tool has been added.
    bool hasTools() const { return !tools_.empty(); }

    /// Clears all tools (for rebuild when tool registry changes).
    void clear() { tools_.clear(); }

    /// Number of registered tools.
    std::size_t toolCount() const { return tools_.size(); }

private:
    /// Generates GBNF rules for a JSON Schema property.
    std::string schemaToGbnf(const std::string& rule_name,
                             const nlohmann::json& schema) const;

    /// Generates the rule for a JSON object with known properties.
    std::string objectToGbnf(const std::string& rule_name,
                             const nlohmann::json& schema) const;

    /// Generates rules for JSON primitive types.
    std::string primitiveToGbnf(const std::string& rule_name,
                                const std::string& type) const;

    ConstrainedConfig config_;
    std::vector<ToolSchema> tools_;
};

/// Converts skill YAML definitions to ToolSchema objects.
/// This bridges the sparx skill loader with the grammar generator.
std::vector<ToolSchema> extractToolSchemas(
    const std::string& skills_dir);

/// Quick check: does the prompt suggest the model should produce a tool call?
/// Heuristic based on system prompt content (looks for tool-use markers).
bool promptExpectsToolCall(const std::string& prompt);

}  // namespace sparx::constrained
