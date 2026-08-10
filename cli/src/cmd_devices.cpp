/**
 * @file cmd_devices.cpp
 * @brief `sparx devices` — discover connected Android devices and their NPU capabilities.
 *
 * Uses `adb devices` to enumerate, then reads /sys/devices for SoC identification
 * and checks /vendor/lib64 for QNN/Genie runtime presence.
 */

#include "sparx_commands.h"
#include "sparx_device_info.h"

#include <iostream>
#include <vector>

namespace sparx {

int cmd_devices(const std::vector<std::string>& /*args*/) {
    auto devices = discoverDevices();

    if (devices.empty()) {
        std::cout << "  no devices found.\n";
        std::cout << "  ensure USB debugging is enabled and run `adb devices`\n";
        return 0;
    }

    std::cout << "  ┌──────────────────────────────────────────────────────────┐\n";
    int index = 1;
    for (const auto& dev : devices) {
        std::cout << "  │  " << index++ << " "
                  << dev.model;
        // Pad to alignment
        for (size_t i = dev.model.size(); i < 20; ++i) std::cout << ' ';
        std::cout << dev.platform_label
                  << "  soc " << dev.soc_id
                  << "  " << dev.dsp_arch
                  << "  " << dev.transport
                  << " │\n";
    }
    std::cout << "  └──────────────────────────────────────────────────────────┘\n";
    return 0;
}

}  // namespace sparx
