#pragma once
/**
 * @file sparx_device_info.h
 * @brief Device discovery and SoC/NPU capability detection via adb.
 *
 * Maps Qualcomm SoC IDs to human-readable names and DSP architectures.
 * This is the knowledge layer that `sparx doctor` and `sparx deploy` share.
 */

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace sparx {

struct DeviceInfo {
    std::string serial;           // adb serial
    std::string model;            // e.g. "Pixel 9 Pro", "SA8797P"
    std::string platform_label;   // e.g. "8 Gen 5", "auto"
    int soc_id = 0;               // Qualcomm SoC ID
    std::string dsp_arch;         // e.g. "v81"
    std::string transport;        // "adb" or "tcp"
    bool has_qnn = false;         // /vendor/lib64/libQnnHtp.so present
    bool has_genie = false;       // /vendor/lib64/libGenie.so present
};

/// Known Qualcomm SoC table — derived from public QNN documentation and
/// nsptargets.py patterns. This table is what sparx doctor validates against.
struct SocEntry {
    int soc_id;
    const char* dsp_arch;
    const char* label;
    int hvx_threads;
    int max_cores;
};

inline constexpr std::array<SocEntry, 8> KNOWN_SOCS = {{
    {36, "v69", "8 Gen 1",     4, 4},
    {43, "v73", "8 Gen 2",     4, 4},
    {52, "v75", "8775/Gen3",   6, 4},
    {57, "v75", "8 Gen 3",     6, 4},
    {60, "v73", "X Elite",     4, 4},
    {69, "v79", "8 Gen 4",     6, 4},
    {72, "v81", "SA8797P",     8, 4},
    {87, "v81", "8 Gen 5",     8, 4},
}};

inline const SocEntry* lookupSoc(int soc_id) {
    for (const auto& entry : KNOWN_SOCS) {
        if (entry.soc_id == soc_id) return &entry;
    }
    return nullptr;
}

/// Execute a shell command and capture stdout.
inline std::string exec(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe.get())) {
        result += buf;
    }
    return result;
}

/// Run an adb shell command on a specific device.
inline std::string adbShell(const std::string& serial,
                            const std::string& cmd) {
    return exec("adb -s " + serial + " shell " + cmd + " 2>/dev/null");
}

/// Discover connected Android devices and probe their capabilities.
inline std::vector<DeviceInfo> discoverDevices() {
    std::vector<DeviceInfo> devices;
    const auto raw = exec("adb devices -l 2>/dev/null");

    // Parse `adb devices -l` output
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("device ") == std::string::npos ||
            line.find("List of") != std::string::npos) {
            continue;
        }
        DeviceInfo dev;
        dev.serial = line.substr(0, line.find(' '));
        dev.transport = "adb";

        // Get model name
        auto model_str = adbShell(dev.serial,
            "getprop ro.product.model");
        if (!model_str.empty() && model_str.back() == '\n')
            model_str.pop_back();
        dev.model = model_str.empty() ? dev.serial : model_str;

        // Get SoC ID from /sys
        auto soc_str = adbShell(dev.serial,
            "cat /sys/devices/soc0/soc_id");
        if (!soc_str.empty()) {
            try { dev.soc_id = std::stoi(soc_str); } catch (...) {}
        }

        // Lookup capabilities
        if (const auto* entry = lookupSoc(dev.soc_id)) {
            dev.dsp_arch = entry->dsp_arch;
            dev.platform_label = entry->label;
        } else {
            dev.dsp_arch = "unknown";
            dev.platform_label = "unknown";
        }

        // Check runtime presence
        auto qnn_check = adbShell(dev.serial,
            "ls /vendor/lib64/libQnnHtp.so");
        dev.has_qnn = qnn_check.find("No such") == std::string::npos &&
                      !qnn_check.empty();

        auto genie_check = adbShell(dev.serial,
            "ls /vendor/lib64/libGenie.so");
        dev.has_genie = genie_check.find("No such") == std::string::npos &&
                        !genie_check.empty();

        devices.push_back(std::move(dev));
    }
    return devices;
}

}  // namespace sparx
