# MasterAgent 官方配置包

`agent-config/` 是开源仓库随附的官方 `MasterAgentConfigBundle`。它有两个用途：

1. 说明规则、Skill、Capability、信息源、SubAgent、Model 和 Policy 应如何组成一个完整 Agent。
2. 作为用户自定义配置包的安全起点。

配置包身份由 `manifest.yaml` 决定，不依赖目录名。用户可以把目录命名为 `my-car-config`、`robot-config` 或其他名称。

## 当前状态

这个目录是目标配置协议的 `v1alpha1` 官方基线，内容与当前参考 Runtime 的真实边界保持一致：

- 已实现的本地写能力：空气循环模式、自动空调风速。
- 已注册的 SubAgent：`trip-agent`。
- 已有的信息接口：短期记忆、车辆/环境状态、Skill 检索和只读 Tool 网关。
- 已声明的模型：当前 Mock intent/classifier、Qwen2.5-3B GGUF，以及默认关闭的 Fake Cloud/OpenAI-compatible 云模型。
- 已有的端云能力：默认关闭的受控假云端 fallback。

当前完整 `master_agent` 参考应用已可从配置包解析 ModelProfile/ModelRoutingPolicy，选择默认 Mock 或显式 Qwen llama.cpp 模型。C++ Runtime 的 Prompt 和 Skill 仍从旧 `config/` 加载，Tool 仍在组合根中注册；完整 ConfigSnapshot、激活以及 `sparx config ...` 命令属于下一阶段实现。这里不会把未接入的异车能力声明为已可执行。

## 目录结构

```text
agent-config/
├── manifest.yaml
├── rules/                 # 规则优先，产出统一 Decision
├── skills/                # 任务知识和允许能力
├── capabilities/          # 最小 MCP 执行能力
├── context_sources/       # 记忆、状态和其他只读信息
├── agents/                # 可调度 SubAgent
├── models/                # 端侧/云端模型身份、Runtime 和协议
├── policies/              # 安全、推理、检索和端云策略
├── prompts/               # 模型指令和输出协议
├── schemas/               # 各类资源的 JSON Schema
└── tests/                 # 配置级行为样例
```

官方包的 `policies/observability.yaml` 同时控制全链路 Trace。默认 `safe` 模式只保存
摘要、哈希、阶段状态和执行身份；如果复制配置包，观测策略也会随包一起替换。

## 创建自己的配置包

当前即可复制官方目录：

```bash
cd /path/to/MasterAgent
cp -R agent-config my-car-config
```

当前可使用仓库内的零依赖基础校验器检查 YAML/JSON、重复 ID 和跨资源引用：

```bash
ruby scripts/validate_agent_config.rb ./my-car-config
```

完整参考应用已可以用配置包选择模型：

```bash
./build/master_agent --config=./my-car-config
./build/master_agent --config=./my-car-config \
  --model-profile=model.local.intent.qwen2_5_3b \
  --model=/path/to/model.gguf \
  "车里有点闷，帮我舒服点"
```

这只加载模型选择切片；程序 JSON 输出会标记 `loading_scope: MODEL_SELECTION_ONLY`。

至少修改：

```yaml
metadata:
  name: my-car-agent
  version: 0.1.0
```

不要直接把异车 Tool 设为 `enabled: true`。必须先实现并注册 Provider、补齐输入输出契约、权限、幂等和完成确认策略。

目标 CLI 体验为：

```bash
sparx config init my-car-config
sparx config validate ./my-car-config
sparx run --config ./my-car-config
sparx config use ./my-car-config
```

这些 `sparx config` 命令是已冻结的产品接口方向，目前尚未接入 `sparx` CLI；上面的是完整 `master_agent` 参考应用命令。

## 配置规则

- YAML 用于人工维护的资源；JSON Schema 用于类型和字段闭集校验；Markdown 用于长 Prompt/Skill 指令。
- 模型文件、endpoint 和凭证通过外部引用解析，不直接进入配置包。
- ID 在一个配置包中必须唯一且稳定，文件名可以变化。
- 资源引用使用 ID，不使用数组下标或展示名称。
- Rule、Skill 和模型只能引用 `enabled` 且存在于同一冻结快照的 Capability、ContextSource 或 Agent。
- 模型输出、规则输出和云端输出必须进入同一个确定性 Validator。
- 配置不能增加任意代码、Shell、动态库或未注册 URL 执行入口。
- 配置变更必须整体校验后原子激活；显式指定无效配置时不得静默回退官方包。

详细协议设计见 [`../docs/12_配置驱动Skill与确定性执行单元设计.md`](../docs/12_配置驱动Skill与确定性执行单元设计.md)，代码目录整改、改/删/增清单和迁移顺序见 [`../docs/13_项目目录整改与配置包迁移计划.md`](../docs/13_项目目录整改与配置包迁移计划.md)。
