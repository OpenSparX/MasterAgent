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

static const char* PROJECT_README = R"(# %NAME%

On-device AI agent built with [Sparx](https://github.com/OpenSparX/MasterAgent).
Runs entirely locally — no cloud, no API keys.

## Run

```bash
sparx run              # interactive session
sparx run "hello"      # single turn
sparx doctor           # check environment and runtime
```

## Layout

- `agent.yaml` — model, routing and reliability settings
- `skills/` — one YAML file per skill; `hello.yaml` is a working example

## Add a skill

Create `skills/<name>.yaml`, then list `<name>` under `skills:` in `agent.yaml`:

```yaml
name: weather
description: "Reports current conditions."
trigger:
  patterns:
    - "weather"
    - "天气"
handler:
  type: deterministic
  response: "It is sunny."
```

Deterministic skills answer without invoking the model, so they cost no
inference time. `routing.deterministic_first` keeps that path preferred.

---

# %NAME%（中文）

基于 [Sparx](https://github.com/OpenSparX/MasterAgent) 的端侧 AI Agent，
完全本地运行，无需云端和 API Key。

## 运行

```bash
sparx run              # 交互式会话
sparx run "你好"        # 单轮对话
sparx doctor           # 检查环境与运行时
```

## 目录结构

- `agent.yaml` — 模型、路由与可靠性配置
- `skills/` — 每个技能一个 YAML 文件，`hello.yaml` 是可直接运行的示例

## 添加技能

新建 `skills/<name>.yaml`，然后在 `agent.yaml` 的 `skills:` 中登记 `<name>`。
确定性技能不调用模型，因此没有推理开销；`routing.deterministic_first`
会优先走这条路径。
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

    // Write README.md
    {
        std::string content = PROJECT_README;
        for (std::string::size_type pos = 0;
             (pos = content.find("%NAME%", pos)) != std::string::npos;) {
            content.replace(pos, 6, name);
            pos += name.size();
        }
        std::ofstream f(project_dir / "README.md");
        f << content;
    }

    std::cout << "  ✓ Project created at ./" << name << "\n";
    std::cout << "  ✓ Template hello-world skill installed\n";
    std::cout << "  ✓ README.md generated\n";
    std::cout << "  ✓ Ready to run:  cd " << name << " && sparx run\n";
    return 0;
}

}  // namespace sparx
