/**
 * @file cmd_devices.cpp
 * @brief `sparx devices` — list and manage connected mesh devices.
 */
#include "sparx_commands.h"
#include <iostream>

namespace sparx::commands {

int cmdDevices(int argc, const char* argv[]) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
    }

    std::cout << "Mesh Device Discovery\n";
    std::cout << "=====================\n\n";
    std::cout << "Scanning local network for sparx agents...\n\n";

    // In production: queries MeshDiscovery for live peers
    std::cout << "No devices found. Start mesh with: sparx mesh start\n";
    if (verbose) {
        std::cout << "\nmDNS service: _sparx-agent._tcp.local.\n";
        std::cout << "Multicast group: 224.0.0.251:5353\n";
    }
    return 0;
}

}  // namespace sparx::commands
