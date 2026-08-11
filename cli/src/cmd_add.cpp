/**
 * @file cmd_add.cpp
 * @brief `sparx add skill <name>` — scaffold a new skill and register it.
 *
 * Two steps that a developer would otherwise do by hand and get wrong in the
 * same two ways every time: writing skills/<name>.yaml, and remembering to add
 * <name> to the skills: list in agent.yaml. A skill file that exists but is not
 * registered silently never fires, which is a confusing first failure.
 *
 * The command is deliberately conservative: it refuses to overwrite an existing
 * skill file, and if agent.yaml already lists the name it says so rather than
 * duplicating the entry.
 */

#include "sparx_commands.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace sparx {

namespace {

/// Template for a new skill. Patterns are left empty on purpose — a scaffold
/// that ships plausible-looking triggers invites the developer to leave them
/// in, and a skill matching words nobody said is worse than one matching none.
const char* SKILL_TEMPLATE = R"(name: %NAME%
description: "TODO: describe what this skill does."

# Deterministic trigger. If any pattern matches the input, this skill runs and
# the model is never invoked. Keep patterns specific: a broad pattern steals
# input from the inference path.
trigger:
  patterns:
    - "TODO"
  intent: %NAME%

# Parameters extracted from the input and passed to the handler.
# parameters:
#   target:
#     type: enum
#     values: [a, b]
#   amount:
#     type: number
#     range: [0, 100]

handler:
  type: deterministic
  response: "TODO: replace with the response, or switch type to mcp."

# For an MCP-backed skill instead:
# handler:
#   type: mcp
#   service: my.service
#   tool: my_tool
)";

std::string applyName(const std::string& tmpl, const std::string& name) {
    std::string out = tmpl;
    const std::string token = "%NAME%";
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::string::npos) {
        out.replace(pos, token.size(), name);
        pos += name.size();
    }
    return out;
}

/// A skill name becomes a filename and a YAML scalar, so restrict it to
/// characters that are safe in both without quoting.
bool validSkillName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) return false;
    }
    // Leading dot or dash would create a hidden file or look like a flag.
    return name.front() != '.' && name.front() != '-';
}

/// Inserts `- <name>` into the skills: block of agent.yaml.
///
/// The file is rewritten line by line rather than parsed and re-emitted: the
/// loader is a minimal flat-YAML reader, and round-tripping through it would
/// drop the comments that make the generated agent.yaml worth reading.
enum class RegisterResult { Added, AlreadyPresent, NoSkillsBlock, WriteFailed };

RegisterResult registerSkill(const fs::path& config_path,
                             const std::string& name) {
    std::ifstream in(config_path);
    if (!in.is_open()) return RegisterResult::WriteFailed;

    std::vector<std::string> lines;
    std::string line;
    bool in_skills = false;
    bool found_block = false;
    size_t insert_at = 0;

    while (std::getline(in, line)) {
        lines.push_back(line);

        if (line.rfind("skills:", 0) == 0) {
            in_skills = true;
            found_block = true;
            insert_at = lines.size();  // default: right after the key
            continue;
        }

        if (in_skills) {
            // A list item under skills:
            const auto first = line.find_first_not_of(" \t");
            const bool is_item = first != std::string::npos && line[first] == '-';
            if (is_item) {
                std::string value = line.substr(first + 1);
                const auto vstart = value.find_first_not_of(" \t");
                if (vstart != std::string::npos) {
                    value = value.substr(vstart);
                    const auto vend = value.find_last_not_of(" \t\r");
                    value = value.substr(0, vend + 1);
                    if (value == name) return RegisterResult::AlreadyPresent;
                }
                insert_at = lines.size();  // keep moving past the last item
            } else if (!line.empty() && first != std::string::npos) {
                // A non-item, non-blank line ends the block.
                in_skills = false;
            }
        }
    }
    in.close();

    if (!found_block) return RegisterResult::NoSkillsBlock;

    lines.insert(lines.begin() + static_cast<long>(insert_at), "  - " + name);

    std::ofstream out(config_path, std::ios::trunc);
    if (!out.is_open()) return RegisterResult::WriteFailed;
    for (const auto& l : lines) out << l << "\n";
    return out.good() ? RegisterResult::Added : RegisterResult::WriteFailed;
}

int addSkill(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "  ✗ skill name required\n";
        std::cerr << "    usage: sparx add skill <name>\n";
        return 1;
    }
    const std::string name = args[0];
    if (!validSkillName(name)) {
        std::cerr << "  ✗ invalid skill name: " << name << "\n";
        std::cerr << "    use letters, digits, '_', '-', '.'; "
                     "must not start with '.' or '-'\n";
        return 1;
    }

    const fs::path config_path = fs::current_path() / "agent.yaml";
    if (!fs::exists(config_path)) {
        std::cerr << "  ✗ no agent.yaml in current directory\n";
        std::cerr << "    run `sparx init <name>` first, then cd into it\n";
        return 1;
    }

    const fs::path skills_dir = fs::current_path() / "skills";
    const fs::path skill_file = skills_dir / (name + ".yaml");

    if (fs::exists(skill_file)) {
        std::cerr << "  ✗ skill already exists: skills/" << name << ".yaml\n";
        std::cerr << "    delete it first if you want to regenerate\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(skills_dir, ec);
    if (ec) {
        std::cerr << "  ✗ could not create skills/: " << ec.message() << "\n";
        return 1;
    }

    {
        std::ofstream f(skill_file);
        if (!f.is_open()) {
            std::cerr << "  ✗ could not write skills/" << name << ".yaml\n";
            return 1;
        }
        f << applyName(SKILL_TEMPLATE, name);
        if (!f.good()) {
            std::cerr << "  ✗ write failed: skills/" << name << ".yaml\n";
            return 1;
        }
    }
    std::cout << "  ✓ created  skills/" << name << ".yaml\n";

    switch (registerSkill(config_path, name)) {
        case RegisterResult::Added:
            std::cout << "  ✓ registered '" << name
                      << "' in agent.yaml\n";
            break;
        case RegisterResult::AlreadyPresent:
            std::cout << "  · '" << name
                      << "' was already listed in agent.yaml\n";
            break;
        case RegisterResult::NoSkillsBlock:
            // Not a failure of the scaffold: the file was written and is
            // usable. But say plainly that it will not fire until registered,
            // because a silent no-op skill is the confusing case.
            std::cout << "  ! agent.yaml has no `skills:` block — "
                         "add one to enable the skill:\n";
            std::cout << "      skills:\n        - " << name << "\n";
            break;
        case RegisterResult::WriteFailed:
            std::cerr << "  ✗ could not update agent.yaml — "
                         "add '" << name << "' to skills: manually\n";
            return 1;
    }

    std::cout << "\n  next: edit skills/" << name
              << ".yaml (patterns + response), then `sparx run`\n";
    return 0;
}

void printAddUsage() {
    std::cout << "\n  sparx add — add a component to the current project\n\n"
                 "  Usage:  sparx add <kind> <name>\n\n"
                 "  Kinds:\n"
                 "    skill <name>    Scaffold skills/<name>.yaml and register "
                 "it in agent.yaml\n\n"
                 "  Examples:\n"
                 "    sparx add skill climate_control\n"
                 "    sparx add skill navigation\n\n";
}

}  // namespace

int cmd_add(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printAddUsage();
        return args.empty() ? 1 : 0;
    }

    const std::string kind = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (kind == "skill") return addSkill(rest);

    std::cerr << "  ✗ unknown kind: " << kind << "\n";
    std::cerr << "    supported: skill\n";
    return 1;
}

}  // namespace sparx
