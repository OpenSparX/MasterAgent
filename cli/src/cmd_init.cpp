/**
 * @file cmd_init.cpp
 * @brief `sparx init <name>` — scaffold a new agent project.
 */

#include "sparx_commands.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace sparx {

static const char* AGENT_YAML = R"(name: %NAME%
version: "0.1.0"

model:
  id: qwen3-4b
  context_length: 4096
  max_output_tokens: 512

skills:
  - hello

routing:
  deterministic_first: true
  confidence_threshold: 0.85

reliability: D2
runtime: auto
)";

static const char* HELLO_SKILL = R"(name: hello
description: "Responds with a greeting."
trigger:
  patterns:
    - "你好"
    - "hello"
    - "hi"
handler:
  type: deterministic
  response: "你好！我是你的端侧 Agent，正在本地运行。有什么可以帮你的？"
)";

int cmd_init(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "  usage: sparx init <project-name>\n";
        return 1;
    }
    const auto& name = args[0];
    const fs::path project_dir = fs::current_path() / name;

    if (fs::exists(project_dir)) {
        std::cerr << "  ✗ directory already exists: " << name << "\n";
        return 1;
    }

    fs::create_directories(project_dir / "skills");

    // Write agent.yaml
    {
        std::string content = AGENT_YAML;
        auto pos = content.find("%NAME%");
        if (pos != std::string::npos) {
            content.replace(pos, 6, name);
        }
        std::ofstream f(project_dir / "agent.yaml");
        f << content;
    }

    // Write hello skill
    {
        std::ofstream f(project_dir / "skills" / "hello.yaml");
        f << HELLO_SKILL;
    }

    std::cout << "  ✓ Project created at ./" << name << "\n";
    std::cout << "  ✓ Template hello-world installed\n";
    std::cout << "  ✓ Ready to run:  cd " << name << " && sparx run\n";
    return 0;
}

}  // namespace sparx
