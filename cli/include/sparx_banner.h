#pragma once
/// @file sparx_banner.h
/// Startup banner for OpenSparX AgentOS CLI

#include <cstdio>
#include <string>

namespace sparx {

inline void print_banner(const char* version = "2.2.2") {
    // ANSI color codes
    constexpr const char* GREEN  = "\033[38;5;46m";
    constexpr const char* CYAN   = "\033[38;5;51m";
    constexpr const char* DIM    = "\033[38;5;240m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* RESET  = "\033[0m";

    std::fprintf(stderr,
        "%s%s"
        "   ____                  _____                 _  __\n"
        "  / __ \\___  ___ ___    / ___/__  ___ _____   | |/ /\n"
        " / /_/ / _ \\/ -_) _ \\  _\\__ \\/ _ \\/ _ `/ __/   >  <\n"
        " \\____/ .__/\\__/_//_/ /____/ .__/\\_,_/_/ ><  /_/\\_\\\n"
        "     /_/                   /_/\n"
        "       ___                __  ____  ___\n"
        "      / _ |___ ____ ___  / /_/ __ \\/ __/\n"
        "     / __ / _ `/ -_) _ \\/ __/ /_/ /\\ \\\n"
        "    /_/ |_\\_, /\\__/_//_/\\__/\\____/___/\n"
        "         /___/\n"
        "%s"
        "  %s──────────────────────────────────────────────%s\n"
        "  %s  Edge AI Agent Framework │ v%s%s\n"
        "  %s  On-Device NPU │ Multi-Agent │ Formal Verify%s\n"
        "  %s──────────────────────────────────────────────%s\n"
        "\n",
        GREEN, BOLD,
        RESET,
        DIM, RESET,
        CYAN, version, RESET,
        DIM, RESET,
        DIM, RESET
    );
}

} // namespace sparx
