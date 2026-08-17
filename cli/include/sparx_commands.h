#pragma once
/**
 * @file sparx_commands.h
 * @brief Declarations for all sparx CLI subcommands.
 */

#include <string>
#include <vector>

namespace sparx {

int cmd_init(const std::vector<std::string>& args);
int cmd_add(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_pull(const std::vector<std::string>& args);
int cmd_devices(const std::vector<std::string>& args);
int cmd_deploy(const std::vector<std::string>& args);
int cmd_doctor(const std::vector<std::string>& args);
int cmd_demo(const std::vector<std::string>& args);
int cmd_shell(const std::vector<std::string>& args);
int cmd_plan(const std::vector<std::string>& args);
int cmd_trace(const std::vector<std::string>& args);
int cmd_learn(const std::vector<std::string>& args);
int cmd_mesh(const std::vector<std::string>& args);

}  // namespace sparx
