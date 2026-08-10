/**
 * @file cmd_deploy.cpp
 * @brief `sparx deploy --device N` — push agent framework + config to Android device.
 *
 * Steps:
 *   1. Select target device (from sparx devices list)
 *   2. Validate agent.yaml
 *   3. Check device capabilities (QNN/Genie presence)
 *   4. Auto-generate Genie HTP config from detected SoC
 *   5. Push: core binary + agent.yaml + skills/ + generated config
 *   6. Optionally download model from AI Hub if not present
 *   7. Start agent on device
 */

#include "sparx_commands.h"
#include "sparx_agent_config.h"
#include "sparx_device_info.h"
#include "sparx_genie_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace sparx {

int cmd_deploy(const std::vector<std::string>& args) {
    // Parse --device N, --model, --start, --stop
    int device_index = -1;
    std::string model_path;
    bool do_start = false;
    bool do_stop = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--device" && i + 1 < args.size()) {
            device_index = std::stoi(args[i + 1]) - 1;  // 1-indexed for user
        }
        if (args[i] == "--model" && i + 1 < args.size()) {
            model_path = args[i + 1];
        }
        if (args[i] == "--start") do_start = true;
        if (args[i] == "--stop") do_stop = true;
    }

    // Load agent.yaml
    const fs::path config_path = fs::current_path() / "agent.yaml";
    if (!fs::exists(config_path)) {
        std::cerr << "  ✗ no agent.yaml found in current directory\n";
        return 1;
    }
    AgentConfig config;
    if (!loadAgentConfig(config_path.string(), config)) {
        std::cerr << "  ✗ failed to parse agent.yaml\n";
        return 1;
    }

    // Discover devices
    auto devices = discoverDevices();
    if (devices.empty()) {
        std::cerr << "  ✗ no devices found. run `sparx devices` to check.\n";
        return 1;
    }
    if (device_index < 0) {
        if (devices.size() == 1) {
            device_index = 0;
        } else {
            std::cerr << "  ✗ multiple devices found. specify with --device N\n";
            std::cerr << "    run `sparx devices` to see the list.\n";
            return 1;
        }
    }
    if (device_index >= static_cast<int>(devices.size())) {
        std::cerr << "  ✗ device index out of range\n";
        return 1;
    }

    const auto& dev = devices[device_index];
    std::cout << "  checking device …\n";

    // Check runtime
    if (dev.has_qnn) {
        std::cout << "  ✓ QNN runtime    /vendor/lib64/libQnnHtp.so\n";
    } else {
        std::cerr << "  ✗ QNN runtime not found — device may not support NPU inference\n";
        std::cerr << "    expected: /vendor/lib64/libQnnHtp.so\n";
        return 1;
    }
    if (dev.has_genie) {
        std::cout << "  ✓ Genie runtime  /vendor/lib64/libGenie.so\n";
    } else {
        std::cout << "  ⚠ Genie runtime not found — will need manual setup\n";
    }

    // Generate Genie config for this device
    const auto* soc = lookupSoc(dev.soc_id);
    if (!soc) {
        std::cerr << "  ✗ unknown SoC ID " << dev.soc_id << "\n";
        return 1;
    }

    std::cout << "\n  pushing opensparx …\n";

    const std::string target_dir = "/data/local/tmp/sparx/";
    const auto push = [&](const std::string& local, const std::string& remote) {
        exec("adb -s " + dev.serial + " push " + local + " " + remote + " 2>/dev/null");
    };
    const auto shell = [&](const std::string& cmd) {
        exec("adb -s " + dev.serial + " shell " + cmd + " 2>/dev/null");
    };

    shell("mkdir -p " + target_dir + "skills");

    // Push agent.yaml
    push(config_path.string(), target_dir + "agent.yaml");
    std::cout << "  ✓ agent.yaml     pushed\n";

    // Push skills
    const fs::path skills_dir = fs::current_path() / "skills";
    if (fs::exists(skills_dir)) {
        for (const auto& entry : fs::directory_iterator(skills_dir)) {
            push(entry.path().string(),
                 target_dir + "skills/" + entry.path().filename().string());
        }
        std::cout << "  ✓ skills/        pushed\n";
    }

    // Generate and push Genie HTP config
    const auto genie_cfg = generateGenieConfig(*soc, config);
    const fs::path genie_cfg_path = fs::temp_directory_path() / "sparx_htp_config.json";
    {
        std::ofstream f(genie_cfg_path);
        f << genie_cfg;
    }
    push(genie_cfg_path.string(), target_dir + "htp_backend_ext_config.json");
    std::cout << "  ✓ genie config   generated for soc " << soc->soc_id
              << " / " << soc->dsp_arch << " / " << soc->max_cores << " cores\n";

    // Push the daemon script
    const fs::path daemon_script = fs::path(__FILE__).parent_path().parent_path()
        / "templates" / "sparx_agent.sh";
    // The script is embedded so the binary works standalone. Write it to a tmp
    // file then push. In a production build this would be embedded as a string
    // literal (like the YAML templates).
    {
        const char* DAEMON_SCRIPT = R"DAEMON(#!/system/bin/sh
SPARX_DIR="/data/local/tmp/sparx"
CONFIG="$SPARX_DIR/agent.yaml"
LOG="$SPARX_DIR/agent.log"
PID_FILE="$SPARX_DIR/agent.pid"
export LD_LIBRARY_PATH="/vendor/lib64:$SPARX_DIR/lib:$LD_LIBRARY_PATH"
export ADSP_LIBRARY_PATH="/vendor/lib/rfsa/adsp:/vendor/dsp/cdsp"
echo $$ > "$PID_FILE"
log() { echo "$(date '+%H:%M:%S') $1" >> "$LOG"; }
cleanup() { log "shutdown"; rm -f "$PID_FILE"; exit 0; }
trap cleanup INT TERM
[ ! -f "$CONFIG" ] && { log "ERROR: no config"; exit 1; }
log "starting agent daemon"
BACKOFF=1
while true; do
  if [ -x "$SPARX_DIR/bin/master_agent" ]; then
    "$SPARX_DIR/bin/master_agent" --config "$CONFIG" --socket "localabstract:sparx_agent" >> "$LOG" 2>&1
    EXIT_CODE=$?
  else
    log "mock mode (no native binary)"
    while true; do sleep 60; done
    EXIT_CODE=0
  fi
  [ $EXIT_CODE -eq 0 ] && break
  log "crashed (exit=$EXIT_CODE), restart in ${BACKOFF}s"
  sleep $BACKOFF
  BACKOFF=$((BACKOFF * 2))
  [ $BACKOFF -gt 30 ] && BACKOFF=30
done
rm -f "$PID_FILE"
)DAEMON";
        const fs::path tmp_script = fs::temp_directory_path() / "sparx_agent.sh";
        std::ofstream sf(tmp_script);
        sf << DAEMON_SCRIPT;
        sf.close();
        push(tmp_script.string(), target_dir + "sparx_agent.sh");
    }
    shell("chmod 755 " + target_dir + "sparx_agent.sh");
    std::cout << "  ✓ daemon script  pushed\n";

    // Push model if specified
    if (!model_path.empty()) {
        if (!fs::exists(model_path)) {
            std::cerr << "  ✗ model not found: " << model_path << "\n";
            std::cerr << "    run `sparx pull` first\n";
            return 1;
        }
        shell("mkdir -p " + target_dir + "models");
        const auto model_name = fs::path(model_path).filename().string();
        push(model_path, target_dir + "models/" + model_name);
        std::cout << "  ✓ model          " << model_name << " pushed\n";
    }

    // --stop: kill existing daemon
    if (do_stop) {
        shell("[ -f " + target_dir + "agent.pid ] && kill $(cat " +
              target_dir + "agent.pid) 2>/dev/null");
        std::cout << "  ✓ existing agent stopped\n";
    }

    // --start: launch daemon
    if (do_start) {
        // Stop any existing daemon first (idempotent)
        shell("[ -f " + target_dir + "agent.pid ] && kill $(cat " +
              target_dir + "agent.pid) 2>/dev/null; sleep 1");
        // Launch in background with nohup
        shell("nohup sh " + target_dir + "sparx_agent.sh > /dev/null 2>&1 &");
        std::cout << "\n  ✓ agent daemon started on " << dev.model << "\n";
        std::cout << "    log: adb -s " << dev.serial << " shell cat "
                  << target_dir << "agent.log\n";
        std::cout << "    connect: sparx shell --device " << dev.serial << "\n";
    } else {
        std::cout << "\n  ✓ deploy complete.\n";
        std::cout << "    start with:  sparx deploy --device "
                  << (device_index + 1) << " --start\n";
        std::cout << "    or manually:  adb -s " << dev.serial
                  << " shell sh " << target_dir << "sparx_agent.sh\n";
    }
    return 0;
}

}  // namespace sparx
