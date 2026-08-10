/**
 * @file main.cpp
 * @brief Verifies that an installed package can be consumed by an external target.
 */

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "master_agent/common/types.h"
#include "master_agent/runtime/master_agent_runtime.h"

int main() {
    using namespace master_agent;

    const std::string digest = secureDigest("master-agent-install");
    if (digest.size() != 64 ||
        toString(TaskPriority::P0) != "P0" ||
        toString(SideEffectState::NotStarted) != "NOT_STARTED") {
        return 1;
    }

    const auto token = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    const auto runtime_directory =
        std::filesystem::temp_directory_path() /
        ("master-agent-package-consumer-" +
         std::to_string(token));
    auto created =
        runtime::MasterAgentRuntime::create(runtime_directory);
    if (!created.status.ok || !created.value) {
        return 2;
    }
    interaction::TextInput input;
    input.text = u8"你好，验证安装包";
    input.user_id = "package-consumer";
    input.session_id = "package-consumer-session";
    const auto turn = (*created.value)->submitText(input);
    if (!turn.success || turn.turn_summary != "direct_reply") {
        return 3;
    }
    created.value->reset();
    std::error_code cleanup_error;
    std::filesystem::remove_all(runtime_directory, cleanup_error);

    std::cout << "MasterAgent package OK: "
              << digest.substr(0, 12) << '\n';
    return 0;
}
