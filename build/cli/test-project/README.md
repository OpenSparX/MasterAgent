# test-project

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

# test-project（中文）

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
