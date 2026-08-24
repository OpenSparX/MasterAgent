# MasterAgent 实时链路控制台

这不是单纯的日志查看器，而是 MasterAgent 的本地交互界面。用户在网页中输入请求，
页面一边显示最终回复，一边实时显示规则匹配、端侧推理、端云仲裁、执行计划、Tool 与
子 Agent 的实际 DataLog 事件。点击任意步骤，会按“接收到什么、做了什么、输出了什么、
为什么这样处理”展示详细内容。

## 使用默认 Mock 模型

先构建项目，然后启动面板：

```bash
cd /path/to/MasterAgent
cmake --build build -j4
python3 tools/trace-viewer/server.py
```

浏览器访问 <http://127.0.0.1:8765>，直接输入问题。

## 使用本地 Qwen GGUF

先保持 `llama-server` 在 `127.0.0.1:8080` 运行，然后执行：

```bash
python3 tools/trace-viewer/server.py \
  --config ./agent-config \
  --model-profile model.local.intent.qwen2_5_3b \
  --model /path/to/models/qwen2.5-3b-instruct-q4_k_m.gguf \
  --endpoint 127.0.0.1:8080
```

## 数据与安全

- 服务只监听 `127.0.0.1`，不会对局域网开放。
- 浏览器和 MasterAgent 都在本机运行。
- 面板会显式开启 `--local-debug-trace`，显示原始请求、召回记忆、Skill、Tool、完整
  Prompt、模型原始输出、确定性计划和 Tool 返回值。
- 原始内容写入每次运行目录的 `local_debug/pipeline.jsonl` 和
  `local_debug/model_io.jsonl`；默认 DataLog 仍只保存脱敏摘要。
- 本地调试文件可能包含隐私，禁止在生产环境启用，也不要直接提交或分享运行目录。
- 每次请求的数据保存在系统临时目录下的 `masteragent-trace-console/`。
- 同时支持导入 `sparx trace export` 生成的历史 Trace JSON。

这是零第三方依赖的开发工具，只使用 Python 标准库，不需要 npm。
