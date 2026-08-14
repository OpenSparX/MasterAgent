/**
 * @file cmd_learn.cpp
 * @brief `sparx learn` — On-Device Continual Learning CLI.
 *
 * Subcommands:
 *   sparx learn correct <input> <preferred>   — record a correction
 *   sparx learn status                        — show learning status
 *   sparx learn train                         — manually trigger training
 *   sparx learn reset                         — delete all adapters
 *   sparx learn export                        — export training data as JSONL
 *
 * The interactive REPL (cmd_run) also captures corrections via the /correct
 * command during a session. This CLI is for batch operations and inspection.
 */

#include "sparx_commands.h"
#include "sparx_agent_config.h"
#include "sparx_learning.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace sparx {

namespace {

fs::path learningBaseDir() {
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".sparx" / "learning";
    }
    return fs::current_path() / ".sparx" / "learning";
}

fs::path adaptersBaseDir() {
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".sparx" / "adapters";
    }
    return fs::current_path() / ".sparx" / "adapters";
}

void printUsage() {
    std::cout <<
        "Usage: sparx learn <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  correct   Record a correction (teaches the agent your preference)\n"
        "  status    Show learning status for the current agent\n"
        "  train     Manually trigger adapter training\n"
        "  reset     Remove all trained adapters (start fresh)\n"
        "  export    Export training pairs as JSONL\n"
        "\n"
        "Examples:\n"
        "  sparx learn correct \"turn on AC\" \"Setting AC to 22°C\"\n"
        "  sparx learn status\n"
        "  sparx learn train --model ~/.sparx/models/qwen2.5-0.5b-instruct-q8_0.gguf\n"
        "\n"
        "The agent learns from your corrections and builds a personalized\n"
        "LoRA adapter that improves responses over time. All data stays\n"
        "on-device, encrypted at rest.\n";
}

}  // namespace

int cmd_learn(const std::vector<std::string>& args) {
    if (args.empty()) {
        printUsage();
        return 0;
    }

    const auto& subcmd = args[0];

    // Load agent name from agent.yaml if available
    std::string agent_name = "default";
    const fs::path config_path = fs::current_path() / "agent.yaml";
    AgentConfig config;
    if (fs::exists(config_path) && loadAgentConfig(config_path.string(), config)) {
        agent_name = config.name;
    }

    learning::TrainingPairStore store(learningBaseDir());
    learning::AdapterManager adapters(adaptersBaseDir());
    learning::TrainingOrchestrator orchestrator(store, adapters);

    // --- correct ---
    if (subcmd == "correct") {
        if (args.size() < 3) {
            std::cerr << "  ✗ usage: sparx learn correct <input> <preferred_output>\n";
            std::cerr << "    example: sparx learn correct \"turn on AC\" \"Setting AC to 22°C\"\n";
            return 1;
        }
        learning::TrainingPair pair;
        pair.agent_name = agent_name;
        pair.input = args[1];
        pair.preferred = args[2];
        pair.model_output = (args.size() > 3) ? args[3] : "";
        pair.model_id = config.model_id.empty() ? "unknown" : config.model_id;

        auto id = store.append(pair);
        auto count = store.count(agent_name);
        auto threshold = orchestrator.config().min_pairs;

        std::cout << "  ✓ correction recorded (id=" << id.substr(0, 8) << "...)\n";
        std::cout << "    pairs: " << count << "/" << threshold
                  << " for next training\n";

        if (orchestrator.shouldTrain(agent_name)) {
            std::cout << "    → threshold reached! run `sparx learn train` "
                         "to build a new adapter\n";
        }
        return 0;
    }

    // --- status ---
    if (subcmd == "status") {
        auto s = orchestrator.status(agent_name);
        std::cout << "  On-Device Continual Learning\n";
        std::cout << "  ─────────────────────────────\n";
        std::cout << "  agent: " << s.agent_name << "\n";
        std::cout << "  training pairs: " << s.total_pairs << "\n";
        std::cout << "  threshold: " << s.training_threshold
                  << " pairs to trigger training\n";
        if (s.adapter_available) {
            std::cout << "  adapter: v" << s.adapter_version
                      << " (" << s.adapter_path << ")\n";
            std::cout << "  last trained: " << s.last_trained_utc << "\n";
        } else {
            std::cout << "  adapter: none (not enough corrections yet)\n";
        }
        std::cout << "\n";
        // Privacy budget
        std::cout << "  Privacy (Differential Privacy)\n";
        std::cout << "  ε spent: " << s.privacy_epsilon_spent
                  << " / " << s.privacy_epsilon_budget << "\n";
        if (s.privacy_budget_exhausted) {
            std::cout << "  ⚠ budget exhausted — refreshes weekly\n";
        } else {
            std::cout << "  ✓ budget available for training\n";
        }
        std::cout << "\n";
        // Quality
        if (s.last_perplexity_before > 0) {
            std::cout << "  Quality Metrics\n";
            std::cout << "  perplexity: " << s.last_perplexity_before
                      << " → " << s.last_perplexity_after << "\n";
            std::cout << "  improvement: " << s.quality_improvement_pct << "%\n";
            std::cout << "\n";
        }
        // Scheduling
        if (orchestrator.scheduler().isIdle()) {
            std::cout << "  Device: idle ✓ (training allowed)\n";
        } else {
            std::cout << "  Device: busy ("
                      << orchestrator.scheduler().blockReason() << ")\n";
        }
        std::cout << "\n";
        if (s.total_pairs > 0 && !s.adapter_available &&
            s.total_pairs < s.training_threshold) {
            std::cout << "  " << (s.training_threshold - s.total_pairs)
                      << " more corrections needed before first training.\n";
        }
        if (s.adapter_available) {
            std::cout << "  The adapter is automatically loaded during "
                         "`sparx run`.\n";
        }
        return 0;
    }

    // --- train ---
    if (subcmd == "train") {
        std::string model_path;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--model" && i + 1 < args.size()) {
                model_path = args[++i];
            }
        }
        if (model_path.empty()) {
            model_path = config.model_path;
        }
        if (model_path.empty()) {
            if (const char* env = std::getenv("SPARX_MODEL")) {
                model_path = env;
            }
        }
        if (model_path.empty()) {
            std::cerr << "  ✗ no base model specified\n";
            std::cerr << "    use --model <path> or set model.path in agent.yaml\n";
            return 1;
        }
        if (!fs::exists(model_path)) {
            std::cerr << "  ✗ model not found: " << model_path << "\n";
            return 1;
        }
        if (!orchestrator.shouldTrain(agent_name)) {
            auto count = store.count(agent_name);
            std::cerr << "  ✗ not enough training pairs (" << count << "/"
                      << orchestrator.config().min_pairs << ")\n";
            std::cerr << "    record more corrections with `sparx learn correct`\n";
            return 1;
        }

        auto result = orchestrator.train(agent_name, model_path);
        if (!result) return 1;

        std::cout << "  adapter will be loaded automatically on next "
                     "`sparx run`.\n";
        return 0;
    }

    // --- reset ---
    if (subcmd == "reset") {
        std::cout << "  This will delete all trained adapters for \""
                  << agent_name << "\".\n";
        std::cout << "  Training pairs are preserved. Continue? [y/N] "
                  << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "  cancelled.\n";
            return 0;
        }
        adapters.reset(agent_name);
        std::cout << "  ✓ adapters removed for \"" << agent_name << "\"\n";
        return 0;
    }

    // --- export ---
    if (subcmd == "export") {
        auto path = orchestrator.exportTrainingData(agent_name);
        auto count = store.count(agent_name);
        if (count == 0) {
            std::cout << "  no training pairs to export.\n";
            return 0;
        }
        std::cout << "  ✓ exported " << count << " pairs to " << path << "\n";
        return 0;
    }

    std::cerr << "  ✗ unknown subcommand: " << subcmd << "\n";
    printUsage();
    return 1;
}

}  // namespace sparx
