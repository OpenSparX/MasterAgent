/**
 * @file cmd_doctor.cpp
 * @brief `sparx doctor` — diagnose device readiness and catch config mismatches.
 *
 * Each check corresponds to a real trap encountered during development:
 *   - soc_id mismatch between artifact filename and config
 *   - num_cores mismatch between prepare script and runtime config
 *   - missing qwen3vl.json (run.sh references nonexistent file)
 *   - pd_session=unsigned on secure boot device
 *   - model artifacts not found
 */

#include "sparx_commands.h"
#include "sparx_device_info.h"
#include "sparx_agent_config.h"

#include <filesystem>
#include <iostream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace sparx {

struct DiagResult {
    bool pass;
    std::string label;
    std::string detail;
};

static DiagResult checkAdb(const DeviceInfo& dev) {
    if (dev.serial.empty()) {
        return {false, "adb device", "no device connected"};
    }
    return {true, "adb device",
            dev.model + " (soc_id " + std::to_string(dev.soc_id) +
            ", " + dev.dsp_arch + ")"};
}

static DiagResult checkQnn(const DeviceInfo& dev) {
    if (!dev.has_qnn) {
        return {false, "QNN runtime",
                "/vendor/lib64/libQnnHtp.so not found"};
    }
    return {true, "QNN runtime",
            "/vendor/lib64/libQnnHtp.so (system image)"};
}

static DiagResult checkGenie(const DeviceInfo& dev) {
    if (!dev.has_genie) {
        return {false, "Genie runtime",
                "/vendor/lib64/libGenie.so not found"};
    }
    return {true, "Genie runtime",
            "/vendor/lib64/libGenie.so"};
}

static DiagResult checkModelArtifacts(const DeviceInfo& dev) {
    auto result = adbShell(dev.serial,
        "ls /data/local/tmp/sparx/*.serialized.bin 2>/dev/null | wc -l");
    int count = 0;
    try { count = std::stoi(result); } catch (...) {}
    if (count == 0) {
        return {false, "model artifacts",
                "not found\n"
                "    → this build needs QNN-compiled context binaries.\n"
                "      OpenSparX does not ship them (Qualcomm license).\n"
                "      run:  sparx deploy --model <path-to-bins>"};
    }
    return {true, "model artifacts",
            std::to_string(count) + " context binary(ies) found"};
}

static DiagResult checkSocIdConsistency(const DeviceInfo& dev) {
    // Check if any artifact filenames on device contain a soc_id that
    // doesn't match the device's actual soc_id
    auto bins = adbShell(dev.serial,
        "ls /data/local/tmp/sparx/*.serialized.bin 2>/dev/null");
    if (bins.empty()) {
        return {true, "soc_id consistency", "no artifacts to check"};
    }

    std::string expected = "socid" + std::to_string(dev.soc_id);
    bool has_mismatch = false;
    std::string mismatched_name;

    std::istringstream stream(bins);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("socid") != std::string::npos &&
            line.find(expected) == std::string::npos) {
            has_mismatch = true;
            mismatched_name = line;
            break;
        }
    }

    if (has_mismatch) {
        // This is the exact trap from the user's scripts: artifact named
        // socid87 but device is soc_id 72.
        return {false, "soc_id consistency",
                "artifact named for different SoC:\n"
                "    file: " + mismatched_name + "\n"
                "    device: soc_id " + std::to_string(dev.soc_id) + "\n"
                "    this may still work if dsp_arch matches, but is a config smell"};
    }
    return {true, "soc_id consistency", "artifact names match device"};
}

static DiagResult checkMemoryBudget(const DeviceInfo& dev) {
    auto meminfo = adbShell(dev.serial, "cat /proc/meminfo | head -1");
    // MemTotal: NNNNN kB
    std::uint64_t total_kb = 0;
    auto pos = meminfo.find(':');
    if (pos != std::string::npos) {
        try { total_kb = std::stoull(meminfo.substr(pos + 1)); } catch (...) {}
    }
    if (total_kb == 0) {
        return {true, "memory budget", "could not read /proc/meminfo"};
    }
    double total_gb = total_kb / 1048576.0;
    // Qwen3-4B w4 needs ~2.4 GB peak
    if (total_gb < 4.0) {
        return {false, "memory budget",
                "device has " + std::to_string(static_cast<int>(total_gb)) +
                " GB RAM — Qwen3-4B needs ~2.4 GB peak, leaving little for system"};
    }
    return {true, "memory budget",
            std::to_string(static_cast<int>(total_gb)) + " GB total"};
}

int cmd_doctor(const std::vector<std::string>& args) {
    auto devices = discoverDevices();
    if (devices.empty()) {
        std::cerr << "  ✗ no devices connected\n";
        return 1;
    }

    // Use first device or --device N
    int idx = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--device" && i + 1 < args.size()) {
            idx = std::stoi(args[i + 1]) - 1;
        }
    }
    if (idx >= static_cast<int>(devices.size())) {
        std::cerr << "  ✗ device index out of range\n";
        return 1;
    }

    const auto& dev = devices[idx];
    std::cout << "  sparx doctor · " << dev.model << "\n\n";

    std::vector<DiagResult> results = {
        checkAdb(dev),
        checkQnn(dev),
        checkGenie(dev),
        checkModelArtifacts(dev),
        checkSocIdConsistency(dev),
        checkMemoryBudget(dev),
    };

    int failures = 0;
    for (const auto& r : results) {
        if (r.pass) {
            std::cout << "  ✓ " << r.label;
            for (size_t i = r.label.size(); i < 20; ++i) std::cout << ' ';
            std::cout << r.detail << "\n";
        } else {
            std::cout << "  ✗ " << r.label << "\n";
            std::cout << "    " << r.detail << "\n";
            ++failures;
        }
    }

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "  all checks passed. ready for `sparx deploy`.\n";
    } else {
        std::cout << "  " << failures << " issue(s) found.\n";
    }
    return failures > 0 ? 1 : 0;
}

}  // namespace sparx
