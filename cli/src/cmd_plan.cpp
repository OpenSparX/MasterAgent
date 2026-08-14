/**
 * @file cmd_plan.cpp
 * @brief `sparx plan` — build, validate, and visualize an execution plan.
 *
 * This is the user-facing surface of DagBuilder. It turns the builder library
 * into an interactive tool: given an agent's available capabilities, it
 * constructs a plan from a YAML spec, validates it against the real
 * orchestrator, and exports it in the requested format.
 *
 * Three modes:
 *
 *   sparx plan show <file.yaml>        build + validate + print (default: text)
 *   sparx plan validate <file.yaml>    build + validate only (exit 0/1)
 *   sparx plan export <file.yaml>      build + export to stdout
 *
 * Format flag: --format=text|json|mermaid (default: text)
 *
 * Plan spec format (plan.yaml):
 *
 *   plan: turn-off-ac
 *   priority: p1             # p0 | p1 (default) | p2
 *   deadline_ms: 3000
 *   nodes:
 *     - id: read_temp
 *       action: vehicle.climate.getTemperature
 *     - id: set_ac
 *       action: vehicle.climate.setPower
 *       params:
 *         power: "off"
 *       after: [read_temp]
 *
 * The spec is intentionally simpler than the full IntentDAG — it maps 1:1 to
 * builder calls so there is exactly one way to express each plan.
 */

#include "sparx_commands.h"
#include "sparx_dag_builder.h"
#include "sparx_formal_verify.h"

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/orchestrator/orchestrator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace sparx {

namespace {

/// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

/// Remove surrounding quotes if present.
std::string unquote(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

/// Parse a YAML inline list: [a, b, c] → vector<string>
std::vector<std::string> parseInlineList(const std::string& val) {
    std::vector<std::string> result;
    if (val.empty() || val.front() != '[') return result;
    auto inner = val.substr(1, val.size() - 2);
    std::istringstream ss(inner);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto t = trim(item);
        if (!t.empty()) result.push_back(unquote(t));
    }
    return result;
}

struct PlanNode {
    std::string id;
    std::string action;
    std::map<std::string, std::string> params;
    std::vector<std::string> after;
};

struct PlanSpec {
    std::string name;
    std::string priority = "p1";  // p0 | p1 (default) | p2
    uint32_t deadline_ms = 5000;
    std::vector<PlanNode> nodes;
    // P0 fields (optional)
    std::string p0_authorization_ref;
    std::set<std::string> p0_capabilities;
};

/// Parse a plan YAML file. Returns false on error (message to stderr).
bool parsePlanSpec(const fs::path& path, PlanSpec& spec) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "  cannot open " << path << "\n";
        return false;
    }

    PlanNode* current_node = nullptr;
    bool in_params = false;

    std::string line;
    while (std::getline(f, line)) {
        // Skip comments and empty lines
        auto stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') continue;

        // Detect indentation level
        size_t indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) continue;

        // Top-level keys (indent 0-1)
        if (indent <= 1 && stripped.find(':') != std::string::npos) {
            in_params = false;
            auto colon = stripped.find(':');
            auto key = trim(stripped.substr(0, colon));
            auto val = trim(stripped.substr(colon + 1));
            val = unquote(val);

            if (key == "plan" || key == "name") spec.name = val;
            else if (key == "priority") spec.priority = val;
            else if (key == "deadline_ms") spec.deadline_ms = std::stoul(val);
            else if (key == "p0_authorization") spec.p0_authorization_ref = val;
            else if (key == "p0_capabilities") {
                auto items = parseInlineList(val);
                spec.p0_capabilities = std::set<std::string>(items.begin(), items.end());
            }
            // "nodes:" just starts the node list
        }
        // Node list item (starts with "- ")
        else if (indent >= 2 && stripped.rfind("- ", 0) == 0) {
            in_params = false;
            spec.nodes.emplace_back();
            current_node = &spec.nodes.back();
            // Could be "- id: foo" on same line
            auto rest = trim(stripped.substr(2));
            if (!rest.empty() && rest.find(':') != std::string::npos) {
                auto c = rest.find(':');
                auto k = trim(rest.substr(0, c));
                auto v = unquote(trim(rest.substr(c + 1)));
                if (k == "id") current_node->id = v;
                else if (k == "action") current_node->action = v;
                else if (k == "after") current_node->after = parseInlineList(v);
            }
        }
        // Node fields
        else if (current_node && indent >= 4) {
            if (stripped == "params:") { in_params = true; continue; }
            auto colon = stripped.find(':');
            if (colon == std::string::npos) continue;
            auto key = trim(stripped.substr(0, colon));
            auto val = unquote(trim(stripped.substr(colon + 1)));

            if (in_params && indent >= 6) {
                current_node->params[key] = val;
            } else {
                in_params = false;
                if (key == "id") current_node->id = val;
                else if (key == "action") current_node->action = val;
                else if (key == "after") current_node->after = parseInlineList(val);
            }
        }
    }

    if (spec.name.empty()) {
        std::cerr << "  plan spec must have a 'plan:' or 'name:' field\n";
        return false;
    }
    if (spec.nodes.empty()) {
        std::cerr << "  plan spec has no nodes\n";
        return false;
    }
    // Auto-generate ids if missing
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        if (spec.nodes[i].id.empty())
            spec.nodes[i].id = "step_" + std::to_string(i + 1);
        if (spec.nodes[i].action.empty()) {
            std::cerr << "  node '" << spec.nodes[i].id << "' has no action\n";
            return false;
        }
    }
    return true;
}

/// Build a DagBuilder from a parsed spec.
DagBuilder buildFromSpec(const PlanSpec& spec) {
    DagBuilder builder(spec.name);

    // Priority: p0, p1 (default), p2
    if (spec.priority == "p0") {
        builder.priority(master_agent::TaskPriority::P0);
    } else if (spec.priority == "p2") {
        builder.priority(master_agent::TaskPriority::P2);
    }
    // else: P1 is the default.

    builder.deadline(std::chrono::milliseconds(spec.deadline_ms));

    // Nodes
    for (const auto& n : spec.nodes) {
        builder.node(n.id, n.action, n.params);
        for (const auto& dep : n.after) {
            builder.after(dep);
        }
    }

    // P0 authorization (must come after nodes and priority)
    if (!spec.p0_authorization_ref.empty()) {
        builder.p0Authorization(spec.p0_authorization_ref, spec.p0_capabilities);
    }

    return builder;
}

void printUsage() {
    std::cout << R"(
  sparx plan — build, validate, and visualize execution plans.

  Usage:
    sparx plan show <plan.yaml>       Build, validate, and display the plan
    sparx plan validate <plan.yaml>   Check whether the plan passes validation
    sparx plan verify <plan.yaml>     Formal verification (CTL model checking)
    sparx plan export <plan.yaml>     Export the plan to stdout

  Options:
    --format=text|json|mermaid   Output format (default: text)
    --property=<name>            Verify only specific property (for verify)

  The plan spec is a YAML file describing nodes, dependencies, and priority.
  See examples/automotive_assistant/plans/ for the format.

  Built-in verification properties:
    auth-before-destructive    No destructive op without auth
    no-conflicting-destructive No concurrent destructive on same resource
    all-nodes-terminate        Every node eventually completes or fails
    no-resource-deadlock       No circular resource waits
    data-flow-integrity        No backward data flow

  Example:
    sparx plan show plans/turn-off-ac.yaml
    sparx plan verify plans/route.yaml
    sparx plan export plans/route.yaml --format=mermaid

)" << std::endl;
}

}  // namespace

int cmd_plan(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return 0;
    }

    std::string subcmd = args[0];
    if (subcmd != "show" && subcmd != "validate" &&
        subcmd != "verify" && subcmd != "export") {
        std::cerr << "  unknown plan subcommand: " << subcmd << "\n"
                  << "  try: sparx plan show <file.yaml>\n";
        return 1;
    }

    // Find the file arg and format flag
    std::string file_path;
    std::string format = "text";
    std::vector<std::string> verify_properties;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].rfind("--format=", 0) == 0) {
            format = args[i].substr(9);
        } else if (args[i] == "--format" && i + 1 < args.size()) {
            format = args[++i];
        } else if (args[i].rfind("--property=", 0) == 0) {
            verify_properties.push_back(args[i].substr(11));
        } else if (file_path.empty()) {
            file_path = args[i];
        }
    }

    if (file_path.empty()) {
        std::cerr << "  missing plan file argument.\n"
                  << "  usage: sparx plan " << subcmd << " <plan.yaml>\n";
        return 1;
    }

    if (format != "text" && format != "json" && format != "mermaid") {
        std::cerr << "  unknown format '" << format << "'\n"
                  << "  supported: text, json, mermaid\n";
        return 1;
    }

    // Parse
    PlanSpec spec;
    if (!parsePlanSpec(file_path, spec)) return 1;

    // Build
    auto builder = buildFromSpec(spec);
    auto [dag, admission] = builder.build();

    // Formal verification via bounded model checking (CTL properties).
    if (subcmd == "verify") {
        // Convert PlanSpec nodes to formal::PlanNode
        std::vector<formal::PlanNode> formal_nodes;
        for (const auto& n : spec.nodes) {
            formal::PlanNode fn;
            fn.id = n.id;
            fn.tool_name = n.action;
            fn.deps = n.after;
            // Heuristic: detect destructive operations by action name patterns
            fn.is_destructive = (n.action.find("delete") != std::string::npos ||
                                 n.action.find("remove") != std::string::npos ||
                                 n.action.find("drop") != std::string::npos ||
                                 n.action.find("setPower") != std::string::npos);
            fn.is_idempotent = (n.action.find("get") != std::string::npos ||
                                n.action.find("read") != std::string::npos);
            fn.requires_auth = fn.is_destructive;  // destructive → needs auth
            fn.timeout_ms = spec.deadline_ms;
            formal_nodes.push_back(fn);
        }

        formal::VerifierConfig vcfg;
        vcfg.properties = verify_properties;
        vcfg.generate_counterexamples = true;
        formal::PlanVerifier verifier(vcfg);
        auto verification = verifier.verify(formal_nodes);

        if (format == "json") {
            std::cout << verification.certificate();
        } else {
            std::cout << verification.report();
        }
        return verification.all_satisfied ? 0 : 1;
    }

    // Validate against a real orchestrator instance.
    using namespace master_agent;
    auto clock = std::make_shared<ManualRuntimeClock>();
    auto ids = std::make_shared<IdGenerator>("sparx-plan-cli");
    auto atomic = std::make_shared<atomic_service::AtomicServiceManager>(
        clock, ids, 1);
    auto dispatch_svc = std::make_shared<agent_dispatch::AgentDispatch>(clock, ids);
    orchestrator::Orchestrator orch(clock, ids, atomic, dispatch_svc);

    CallContext ctx{
        CallerModuleId::AgentService, dag.request_id,
        "trace-sparx-plan-cli",
        admission.principal_id_hash,
        admission.granted_priority,
        dag.deadline_mono_ns, {}, 0,
        admission.authorization_ref};
    auto result = orch.validateDAG(dag, admission, ctx);

    if (subcmd == "validate") {
        if (result.valid) {
            std::cout << "  ✓ plan '" << spec.name << "' is valid ("
                      << spec.nodes.size() << " nodes)\n";
            return 0;
        } else {
            std::cerr << "  ✗ plan '" << spec.name << "' rejected: "
                      << result.reject_code << "\n";
            for (const auto& d : result.details)
                std::cerr << "    " << d << "\n";
            return 1;
        }
    }

    // For show/export, warn if invalid but still output
    if (!result.valid) {
        std::cerr << "  ⚠ plan validation failed: " << result.reject_code << "\n";
        for (const auto& d : result.details)
            std::cerr << "    " << d << "\n";
        std::cerr << "\n";
    }

    // Export
    if (format == "json") {
        std::cout << dagToJson(dag).dump(2) << "\n";
    } else if (format == "mermaid") {
        std::cout << dagToMermaid(dag) << "\n";
    } else {
        // text
        if (subcmd == "show") {
            std::cout << "\n  Plan: " << spec.name;
            if (result.valid) std::cout << "  ✓ valid";
            std::cout << "\n";
            std::cout << "  Priority: " << spec.priority
                      << "  Deadline: " << spec.deadline_ms << "ms\n\n";
        }
        std::cout << dagToText(dag) << "\n";
    }
    return result.valid ? 0 : 1;
}

}  // namespace sparx
