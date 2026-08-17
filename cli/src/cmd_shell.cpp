/**
 * @file cmd_shell.cpp
 * @brief `sparx shell` — interactive REPL for agent interaction.
 */
#include "sparx_commands.h"
#include <iostream>
#include <string>

namespace sparx::commands {

int cmdShell(int /*argc*/, const char* /*argv*/[]) {
    std::cout << "sparx shell — Interactive Agent REPL\n";
    std::cout << "Type 'help' for commands, 'exit' to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << "sparx> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;
        if (line == "help") {
            std::cout << "Commands:\n";
            std::cout << "  run <skill>  — execute a skill\n";
            std::cout << "  plan <goal>  — generate execution plan\n";
            std::cout << "  mesh status  — show mesh peers\n";
            std::cout << "  exit         — quit shell\n";
            continue;
        }
        // Forward to agent runtime
        std::cout << "[agent] Processing: " << line << "\n";
    }
    return 0;
}

}  // namespace sparx::commands
