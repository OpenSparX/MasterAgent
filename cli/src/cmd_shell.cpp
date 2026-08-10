/**
 * @file cmd_shell.cpp
 * @brief `sparx shell` — interactive REPL with the on-device agent.
 *
 * Connects to a running agent on a connected device via adb forward and
 * unix socket, or falls back to the local mock runtime.
 */

#include "sparx_commands.h"
#include "sparx_agent_config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace sparx {

int cmd_shell(const std::vector<std::string>& args) {
    bool local = false;
    std::string device_serial;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--local") {
            local = true;
        } else if (args[i] == "--device" && i + 1 < args.size()) {
            device_serial = args[++i];
        }
    }

    if (local) {
        // Load project config and run the local REPL (same as `sparx run`)
        AgentConfig config;
        if (!loadAgentConfig("agent.yaml", config)) {
            std::cerr << "  ✗ no agent.yaml here. run `sparx init <name>` first.\n";
            return 1;
        }

        std::cout << "\n  " << config.name << " v" << config.version
                  << " · local shell (mock runtime)\n";
        std::cout << "  type .quit to exit\n\n";

        std::string line;
        while (true) {
            std::cout << "  > ";
            if (!std::getline(std::cin, line)) break;
            if (line == ".quit" || line == ".exit") break;
            if (line.empty()) continue;

            bool handled = false;
            for (const auto& skill : config.skills) {
                if (matchesDeterministicSkill(skill, line)) {
                    executeDeterministicSkill(skill, line);
                    handled = true;
                    break;
                }
            }
            if (!handled) {
                std::cout << "  [mock] would route to inference: \""
                          << line << "\"\n\n";
            }
        }
        return 0;
    }

    // Remote shell: forward a unix socket from the device
    if (device_serial.empty()) {
        std::cout << "\n  sparx shell connects to a running agent on-device.\n";
        std::cout << "  use --device <serial> or --local for the mock REPL.\n\n";
        std::cout << "  example:\n";
        std::cout << "    sparx shell --device R5CT1234567\n\n";
        return 1;
    }

    // Set up adb forward
    std::string fwd_cmd = "adb -s " + device_serial +
        " forward tcp:19393 localabstract:sparx_agent";
    int ret = std::system(fwd_cmd.c_str());
    if (ret != 0) {
        std::cerr << "  ✗ adb forward failed (is the agent running on device?)\n";
        return 1;
    }

    std::cout << "\n  connected to agent on " << device_serial
              << " via localhost:19393\n";
    std::cout << "  type .quit to exit\n\n";

    // In a real implementation this would open a TCP socket to localhost:19393
    // and implement the agent wire protocol. For now this stub demonstrates
    // the intended UX.
    std::string line;
    while (true) {
        std::cout << "  > ";
        if (!std::getline(std::cin, line)) break;
        if (line == ".quit" || line == ".exit") break;
        if (line.empty()) continue;

        std::cout << "  [stub] would send to device agent: \"" << line << "\"\n\n";
    }

    // Clean up forward
    std::string rm_cmd = "adb -s " + device_serial +
        " forward --remove tcp:19393";
    std::system(rm_cmd.c_str());

    return 0;
}

}  // namespace sparx
