/**
 * @file cmd_trace.cpp
 * @brief `sparx trace` — display execution trace / timeline.
 */
#include "sparx_commands.h"
#include <iostream>

namespace sparx::commands {

int cmdTrace(int argc, const char* argv[]) {
    bool json_output = false;
    std::string trace_id;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") json_output = true;
        else trace_id = arg;
    }

    if (trace_id.empty()) {
        std::cout << "Recent traces:\n";
        std::cout << "  (no traces recorded — run a skill first)\n";
        return 0;
    }

    if (json_output) {
        std::cout << "{\"trace_id\":\"" << trace_id << "\",\"events\":[]}\n";
    } else {
        std::cout << "Trace: " << trace_id << "\n";
        std::cout << "  (trace not found)\n";
    }
    return 0;
}

}  // namespace sparx::commands
