#pragma once
/**
 * @file sparx_skill_loader.h
 * @brief Loads skill YAML files from skills/ so scaffolded skills actually fire.
 *
 * Before this existed, deterministic matching was a hardcoded check for the
 * string "hello". That made `sparx add skill <name>` produce a file the runtime
 * ignored: the developer edits patterns, runs the agent, and nothing happens
 * with no error to explain why.
 *
 * The parser handles the subset of YAML the skill template emits — a flat
 * mapping with one nested `trigger.patterns` sequence and a `handler.response`
 * scalar. It is deliberately not a general YAML implementation; anything it
 * cannot understand is reported rather than guessed at.
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sparx {

struct SkillDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> patterns;
    /// "deterministic" or "mcp". Only deterministic runs locally today.
    std::string handler_type = "deterministic";
    std::string response;
    /// `handler.response_template` — a response containing {placeholder} tokens
    /// that parameter extraction is meant to fill. Kept separate from
    /// `response` so the caller can say plainly that the placeholders are
    /// unfilled instead of printing braces at the user as if they were text.
    std::string response_template;
    /// For handler_type == "mcp".
    std::string mcp_service;
    std::string mcp_tool;
    /// Path the definition came from, for error messages.
    std::string source_path;
};

namespace detail {

inline std::string trimSkill(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Strips one layer of matching quotes, if present.
inline std::string unquoteSkill(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/// Indentation width of a line, treating a tab as one column. Only used to
/// tell "nested under the previous key" from "back at the top level", so the
/// exact tab width does not matter as long as it is consistent.
inline std::size_t indentOfSkill(const std::string& line) {
    std::size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return n;
}

}  // namespace detail

/// Parses one skills/<name>.yaml. Returns false if the file cannot be read or
/// declares no name.
inline bool loadSkillDefinition(const std::string& path,
                                SkillDefinition& skill) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    skill.source_path = path;

    // Section tracking. `trigger:` and `handler:` open nested blocks; a key at
    // column 0 closes whichever was open.
    enum class Section { None, Trigger, TriggerPatterns, Handler };
    Section section = Section::None;

    // Set while consuming a `key: |` literal block. The block ends at the first
    // line indented no deeper than the key that opened it.
    bool in_block_scalar = false;
    std::size_t block_indent = 0;
    std::string* block_target = nullptr;

    std::string line;
    while (std::getline(file, line)) {
        if (in_block_scalar) {
            const std::string probe = detail::trimSkill(line);
            const std::size_t ind = detail::indentOfSkill(line);
            // Blank lines belong to the block; a shallower non-blank line ends it.
            if (probe.empty()) {
                if (block_target && !block_target->empty()) *block_target += "\n";
                continue;
            }
            if (ind > block_indent) {
                if (block_target) {
                    if (!block_target->empty()) *block_target += "\n";
                    *block_target += probe;
                }
                continue;
            }
            in_block_scalar = false;
            block_target = nullptr;
            // Fall through: this line is regular content.
        }

        // Strip comments. Skill patterns are quoted in the template, so a '#'
        // inside quotes would be mangled here; guard against that by only
        // treating '#' as a comment when it is outside quotes.
        bool in_quote = false;
        char quote_char = '\0';
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (!in_quote && (c == '"' || c == '\'')) {
                in_quote = true;
                quote_char = c;
            } else if (in_quote && c == quote_char) {
                in_quote = false;
            } else if (!in_quote && c == '#') {
                line = line.substr(0, i);
                break;
            }
        }

        const std::string trimmed = detail::trimSkill(line);
        if (trimmed.empty()) continue;

        const std::size_t indent = detail::indentOfSkill(line);

        // A top-level key ends any open nested section.
        if (indent == 0 && trimmed.back() != ':' &&
            trimmed.find(':') != std::string::npos) {
            section = Section::None;
        }

        // Sequence item — only meaningful inside trigger.patterns.
        if (trimmed.front() == '-') {
            if (section == Section::TriggerPatterns) {
                const std::string value =
                    detail::unquoteSkill(detail::trimSkill(trimmed.substr(1)));
                if (!value.empty()) skill.patterns.push_back(value);
            }
            continue;
        }

        const auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = detail::trimSkill(trimmed.substr(0, colon));
        const std::string raw_value = detail::trimSkill(trimmed.substr(colon + 1));
        const std::string value = detail::unquoteSkill(raw_value);

        // Literal block scalar opener: `key: |` (or `|-`).
        if (raw_value == "|" || raw_value == "|-") {
            if (section == Section::Handler && key == "response_template") {
                in_block_scalar = true;
                block_indent = indent;
                block_target = &skill.response_template;
            } else if (section == Section::Handler && key == "response") {
                in_block_scalar = true;
                block_indent = indent;
                block_target = &skill.response;
            }
            continue;
        }

        // Block openers
        if (value.empty()) {
            if (indent == 0 && key == "trigger") {
                section = Section::Trigger;
                continue;
            }
            if (indent == 0 && key == "handler") {
                section = Section::Handler;
                continue;
            }
            if (section == Section::Trigger && key == "patterns") {
                section = Section::TriggerPatterns;
                continue;
            }
            continue;
        }

        // Scalars
        if (indent == 0 && key == "name") {
            skill.name = value;
        } else if (indent == 0 && key == "description") {
            skill.description = value;
        } else if (section == Section::Handler) {
            if (key == "type") skill.handler_type = value;
            else if (key == "response") skill.response = value;
            else if (key == "response_template") skill.response_template = value;
            else if (key == "service") skill.mcp_service = value;
            else if (key == "tool") skill.mcp_tool = value;
        }
    }

    return !skill.name.empty();
}

/// Loads every skills/<name>.yaml listed in `names`. A name with no file is
/// skipped silently: agent.yaml is the registry, and a registered-but-absent
/// skill is reported by the caller, which knows how it wants to present it.
inline std::vector<SkillDefinition> loadSkills(
    const std::filesystem::path& project_dir,
    const std::vector<std::string>& names) {
    std::vector<SkillDefinition> skills;
    const auto dir = project_dir / "skills";
    for (const auto& name : names) {
        const auto path = dir / (name + ".yaml");
        if (!std::filesystem::exists(path)) continue;
        SkillDefinition skill;
        if (loadSkillDefinition(path.string(), skill)) {
            skills.push_back(std::move(skill));
        }
    }
    return skills;
}

/// Collects `{placeholder}` names still present in a template. Parameter
/// extraction is not implemented, so these are what a skill would need filled
/// before its response is usable — worth showing rather than printing braces.
inline std::vector<std::string> unfilledPlaceholders(const std::string& tmpl) {
    std::vector<std::string> names;
    std::size_t pos = 0;
    while ((pos = tmpl.find('{', pos)) != std::string::npos) {
        const auto end = tmpl.find('}', pos + 1);
        if (end == std::string::npos) break;
        auto name = tmpl.substr(pos + 1, end - pos - 1);
        if (!name.empty() &&
            std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(std::move(name));
        }
        pos = end + 1;
    }
    return names;
}

/// True when any of the skill's patterns occurs in the input.
///
/// Substring rather than equality: patterns are phrases like "空调" that appear
/// inside a longer utterance ("把空调打开"). Matching is case-insensitive for
/// ASCII so "AC" matches "ac"; non-ASCII bytes are compared as-is, which is
/// correct for CJK where there is no case to fold.
inline bool skillMatches(const SkillDefinition& skill,
                         const std::string& input) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return (c < 0x80) ? static_cast<char>(std::tolower(c))
                              : static_cast<char>(c);
        });
        return s;
    };
    const std::string haystack = lower(input);
    for (const auto& pattern : skill.patterns) {
        if (pattern.empty()) continue;
        if (haystack.find(lower(pattern)) != std::string::npos) return true;
    }
    return false;
}

}  // namespace sparx
