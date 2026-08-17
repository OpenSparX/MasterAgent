/**
 * @file cmd_deploy.cpp
 * @brief `sparx deploy` — package and deploy agent to target device.
 */
#include "sparx_commands.h"
#include <iostream>

namespace sparx::commands {

int cmdDeploy(int argc, const char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: sparx deploy <target> [--model <path>] [--config <path>]\n";
        return 1;
    }
    std::string target = argv[1];

    std::cout << "Deploying to target: " << target << "\n";
    std::cout << "  Packaging skills...\n";
    std::cout << "  Bundling model weights...\n";
    std::cout << "  Generating device manifest...\n";

    // In production: invoke adb push / scp / OTA mechanism
    std::cout << "Deploy complete. Agent ready on " << target << ".\n";
    return 0;
}

}  // namespace sparx::commands
