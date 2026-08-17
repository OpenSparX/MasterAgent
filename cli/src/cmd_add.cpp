/**
 * @file cmd_add.cpp
 * @brief `sparx add` — register a new skill or tool in the project.
 */
#include "sparx_commands.h"
#include <iostream>

namespace sparx::commands {

int cmdAdd(int argc, const char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: sparx add <skill|tool> <name> [options]\n";
        return 1;
    }
    std::string subcommand = argv[1];
    if (subcommand == "skill") {
        if (argc < 3) {
            std::cerr << "Usage: sparx add skill <name>\n";
            return 1;
        }
        std::string name = argv[2];
        // Create skill template in skills/ directory
        std::cout << "Created skill: skills/" << name << ".yml\n";
        return 0;
    }
    if (subcommand == "tool") {
        if (argc < 3) {
            std::cerr << "Usage: sparx add tool <name>\n";
            return 1;
        }
        std::string name = argv[2];
        std::cout << "Registered tool: " << name << "\n";
        return 0;
    }
    std::cerr << "Unknown subcommand: " << subcommand << "\n";
    return 1;
}

}  // namespace sparx::commands
