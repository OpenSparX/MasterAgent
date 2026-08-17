/**
 * @file cmd_mesh.cpp
 * @brief `sparx mesh` — Agent Mesh Protocol CLI surface.
 *
 * Subcommands:
 *   sparx mesh status    Show mesh health and connected peers
 *   sparx mesh peers     List discovered peers with capabilities
 *   sparx mesh sync      Show CRDT state sync status
 */

#include "sparx_commands.h"
#include "sparx_mesh.h"

#include <iostream>
#include <string>
#include <vector>

namespace sparx {

int cmd_mesh(const std::vector<std::string>& args) {
    std::string subcmd = (args.empty() || args[0] == "--help") ? "help" : args[0];

    if (subcmd == "help" || subcmd == "-h") {
        std::cout << R"(
  sparx mesh — Agent Mesh Protocol management.

  Usage:
    sparx mesh status     Show mesh health and connected peers
    sparx mesh peers      List discovered peers with capabilities
    sparx mesh sync       Show CRDT state sync status
    sparx mesh start      Start mesh discovery (auto-starts with `sparx run`)

  The mesh protocol enables zero-config multi-device collaboration:
    • mDNS/DNS-SD peer discovery on local network
    • Capability-based intent routing (NPU, model, skill matching)
    • CRDT-based state synchronization (corrections, memories)
    • Split inference across multiple NPU devices (experimental)

  Peers are discovered automatically when multiple Sparx instances
  run on the same network segment.

)" << std::endl;
        return 0;
    }

    // Create a mesh instance for status queries
    mesh::PeerId self_id;
    self_id.device_id = "sparx-mesh-cli";
    self_id.display_name = "localhost";
    self_id.sparx_version = "cli";
    mesh::DeviceCapabilities caps;
    auto mesh = mesh::MeshProtocol::create(self_id, caps);

    if (subcmd == "start") {
        if (mesh->start()) {
            std::cout << "  ✓ mesh discovery started on port "
                      << mesh::DiscoveryConfig{}.service_port << "\n";
            std::cout << "  listening for peers on "
                      << mesh::DiscoveryConfig{}.service_type << "\n";
        } else {
            std::cerr << "  ✗ failed to start mesh discovery\n";
            return 1;
        }
        // In a real CLI this would block; here just report success.
        mesh->stop();
        return 0;
    }

    if (subcmd == "status") {
        mesh->start();
        auto health = mesh->health();
        std::cout << "  Agent Mesh Protocol Status\n";
        std::cout << "  ══════════════════════════\n";
        std::cout << "  Discovery: "
                  << (health.discovery_active ? "active" : "inactive") << "\n";
        std::cout << "  State sync: "
                  << (health.sync_active ? "enabled" : "disabled") << "\n";
        std::cout << "  Peers discovered: " << health.alive_peers
                  << "/" << health.total_peers << " alive\n";
        std::cout << "  Synced keys: " << health.synced_keys << "\n";
        std::cout << "  Service type: "
                  << mesh::DiscoveryConfig{}.service_type << "\n";
        std::cout << "  Service port: "
                  << mesh::DiscoveryConfig{}.service_port << "\n";
        if (health.alive_peers == 0) {
            std::cout << "\n  No peers on this network segment.\n";
            std::cout << "  Start `sparx run` on another device to form a mesh.\n";
        }
        mesh->stop();
        return 0;
    }

    if (subcmd == "peers") {
        mesh->start();
        auto peers = mesh->peers();
        if (peers.empty()) {
            std::cout << "  No peers discovered. Run `sparx run` on another device.\n";
        } else {
            std::cout << "  Discovered Peers\n";
            std::cout << "  ════════════════\n";
            for (const auto& p : peers) {
                std::cout << "  • " << p.id.display_name
                          << " (" << p.id.device_id << ")\n";
                std::cout << "    NPU: " << (p.capabilities.has_npu ? "yes" : "no");
                if (p.capabilities.npu_tops > 0)
                    std::cout << " (" << p.capabilities.npu_tops << " TOPS)";
                std::cout << "  RAM: " << p.capabilities.ram_mb << "MB";
                std::cout << "  Battery: "
                          << static_cast<int>(p.capabilities.battery_level * 100) << "%";
                if (p.capabilities.is_idle) std::cout << "  [idle]";
                std::cout << "\n";
                if (!p.capabilities.loaded_models.empty()) {
                    std::cout << "    Models: ";
                    for (size_t i = 0; i < p.capabilities.loaded_models.size(); ++i) {
                        if (i) std::cout << ", ";
                        std::cout << p.capabilities.loaded_models[i];
                    }
                    std::cout << "\n";
                }
            }
        }
        mesh->stop();
        return 0;
    }

    if (subcmd == "sync") {
        std::cout << "  CRDT State Sync\n";
        std::cout << "  ═══════════════\n";
        std::cout << "  Type: operation-based CRDTs (op-CRDTs)\n";
        std::cout << "  Supported types: GCounter, PNCounter, GSet, ORSet, LWWRegister, MVRegister\n";
        std::cout << "  Consistency: eventual (conflict-free)\n";
        std::cout << "  Ordering: Lamport clocks + vector clocks\n";
        std::cout << "\n  Synced data categories:\n";
        std::cout << "    • Learned corrections (GSet — grow-only, shared across devices)\n";
        std::cout << "    • Agent preferences (LWWRegister — last-write-wins)\n";
        std::cout << "    • Intent history (GSet — union of observations)\n";
        std::cout << "    • Speculation cache invalidation (PNCounter)\n";
        return 0;
    }

    std::cerr << "  unknown mesh subcommand: " << subcmd << "\n"
              << "  try: sparx mesh status\n";
    return 1;
}

}  // namespace sparx
