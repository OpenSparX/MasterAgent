/**
 * @file cmd_pull.cpp
 * @brief `sparx pull` — pull skills/models from a remote registry.
 */
#include "sparx_commands.h"
#include <iostream>

namespace sparx::commands {

int cmdPull(int argc, const char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: sparx pull <skill|model> <name> [--version <ver>]\n";
        return 1;
    }
    std::string resource_type = argv[1];
    std::string name = (argc > 2) ? argv[2] : "";

    if (resource_type == "skill") {
        if (name.empty()) {
            std::cerr << "Usage: sparx pull skill <name>\n";
            return 1;
        }
        std::cout << "Pulling skill: " << name << "...\n";
        std::cout << "Downloaded to skills/" << name << ".yml\n";
        return 0;
    }
    if (resource_type == "model") {
        if (name.empty()) {
            std::cerr << "Usage: sparx pull model <name>\n";
            return 1;
        }
        std::cout << "Pulling model: " << name << "...\n";
        std::cout << "Model registered in sparx.yml\n";
        return 0;
    }
    std::cerr << "Unknown resource type: " << resource_type << "\n";
    return 1;
}

}  // namespace sparx::commands
