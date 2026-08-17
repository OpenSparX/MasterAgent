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

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║ Model Registry — 统一模型配置                                               ║
# ║ Configure all models (local + cloud) in one place.                         ║
# ║ 在此统一配置所有模型（本地模型 + 云端模型），初始化时一次性完成。                    ║
# ╚══════════════════════════════════════════════════════════════════════════════╝
models:
  # ── 本地模型 (Local) ──────────────────────────────────────────────────────
  # 运行在设备端的 GGUF 模型，通过 llama-server 推理。
  # 如果 path 为空，可通过 --model 参数或 $SPARX_MODEL 环境变量指定。
  - name: qwen3-4b
    type: local
    role: default
    # path: "./models/qwen3-4b-q4_k_m.gguf"
    context_length: 4096
    max_output_tokens: 512
    temperature: 0.7
    supports_code: true
    supports_reasoning: true
    cost_tier: 0

  # ── 云端模型 (Cloud) ──────────────────────────────────────────────────────
  # OpenAI 兼容 API，用于复杂任务的端云融合调度。
  # 取消下方注释并填写你的 endpoint 和 API Key 环境变量名即可启用。

  # - name: qwen3-235b
  #   type: cloud
  #   role: cloud
  #   endpoint: "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
  #   api_key_env: "DASHSCOPE_API_KEY"
  #   context_length: 131072
  #   max_output_tokens: 4096
  #   temperature: 0.7
  #   supports_code: true
  #   supports_reasoning: true
  #   supports_tools: true
  #   cost_tier: 2

  # - name: deepseek-r1
  #   type: cloud
  #   role: reasoning
  #   endpoint: "https://api.deepseek.com/v1/chat/completions"
  #   api_key_env: "DEEPSEEK_API_KEY"
  #   context_length: 65536
  #   max_output_tokens: 8192
  #   temperature: 0.6
  #   supports_code: true
  #   supports_reasoning: true
  #   cost_tier: 2

  # - name: gpt-4o
  #   type: cloud
  #   role: cloud
  #   endpoint: "https://api.openai.com/v1/chat/completions"
  #   api_key_env: "OPENAI_API_KEY"
  #   context_length: 128000
  #   max_output_tokens: 4096
  #   temperature: 0.7
  #   supports_code: true
  #   supports_reasoning: true
  #   supports_tools: true
  #   supports_vision: true
  #   cost_tier: 3

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║ Routing — 路由策略                                                          ║
# ║ Controls how tasks are dispatched between models.                          ║
# ╚══════════════════════════════════════════════════════════════════════════════╝
routing:
  default_model: qwen3-4b
  # cloud_model: qwen3-235b         # 取消注释启用端云融合
  # reasoning_model: deepseek-r1    # 取消注释启用推理模型
  cloud_enabled: false               # 设为 true 激活云端调度
  complexity_threshold: 0.6          # 复杂度超过此值时路由到云端 [0.0-1.0]
  fallback_to_local: true            # 云端失败时回退本地
  timeout_ms: 30000                  # 云端请求超时 (ms)
  deterministic_first: true          # 优先匹配确定性技能
  confidence_threshold: 0.85         # 技能匹配置信度阈值

skills:
  - hello

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
