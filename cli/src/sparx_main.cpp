/**
 * @file sparx_main.cpp
 * @brief Entry point for the `sparx` CLI.
 *
 * Subcommands: init, add, pull, run, devices, deploy, doctor, demo, shell.
 * This is the developer-facing surface of OpenSparX — every UX decision here
 * matters more than any internal architectural choice.
 */

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "sparx_commands.h"

namespace fs = std::filesystem;

static void printUsage() {
    std::cout << R"(
  OpenSparX CLI — on-device Agent framework

  Usage:  sparx <command> [options]

  Commands:
    init <name>       Create a new agent project
    add skill <name>  Scaffold a new skill and register it in agent.yaml
    pull <model>      Download model artifacts (GGUF / context binary)
    run               Run the agent locally (CPU/GPU)
    devices           List connected devices and their NPU capabilities
    deploy            Deploy agent to a connected device
    doctor            Diagnose device readiness and config issues
    demo              Run built-in demos (automotive, crash recovery, streaming)
    shell             Interactive session with a deployed agent
    plan              Build, validate, and visualize execution plans

  Options:
    --help, -h        Show this help
    --version, -v     Show version

  Examples:
    sparx init my-agent
    sparx add skill climate_control
    sparx demo automotive              # 30-second killer demo
    sparx plan show plans/route.yaml   # visualize an execution plan
    sparx run
    sparx deploy --device 1
    sparx doctor

)" << std::endl;
}

// Stamped by the build system so a release binary reports the tag it was cut
// from. The fallback matters: a developer building from a plain `cmake ..`
// checkout gets "0.0.0-dev", which is deliberately not a valid release version
// so a dev build can never be mistaken for a published one in a bug report.
#ifndef SPARX_VERSION
#define SPARX_VERSION "0.0.0-dev"
#endif
#ifndef SPARX_GIT_SHA
#define SPARX_GIT_SHA "unknown"
#endif
#ifndef SPARX_BUILD_TARGET
#define SPARX_BUILD_TARGET "unknown"
#endif

static void printVersion() {
    std::cout << "sparx " << SPARX_VERSION << "\n"
              << "  commit:  " << SPARX_GIT_SHA << "\n"
              << "  target:  " << SPARX_BUILD_TARGET << "\n"
              << "  kernel:  master_agent v2.0.0" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 0;
    }
    const std::string cmd = argv[1];
    // Both the flag and the bare-word forms are accepted. Users type
    // `sparx version` and `sparx help` from muscle memory (git, go, docker all
    // take them) and being told "unknown command" for that is a bad first
    // impression on the very first command someone runs.
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-v" || cmd == "version") {
        printVersion();
        return 0;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (cmd == "init") return sparx::cmd_init(args);
    if (cmd == "add") return sparx::cmd_add(args);
    if (cmd == "pull") return sparx::cmd_pull(args);
    if (cmd == "run") return sparx::cmd_run(args);
    if (cmd == "devices") return sparx::cmd_devices(args);
    if (cmd == "deploy") return sparx::cmd_deploy(args);
    if (cmd == "doctor") return sparx::cmd_doctor(args);
    if (cmd == "demo") return sparx::cmd_demo(args);
    if (cmd == "shell") return sparx::cmd_shell(args);
    if (cmd == "plan") return sparx::cmd_plan(args);

    std::cerr << "  unknown command: " << cmd << "\n";
    std::cerr << "  run `sparx --help` for available commands\n";
    return 1;
}
