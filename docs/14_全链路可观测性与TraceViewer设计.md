# MasterAgent 全链路可观测性与 Trace Viewer 设计

## 1. 目标

一次用户请求只分配一个 `trace_id`，从进入 Interaction 到最终回复，能够回答：经过了
哪些阶段、每一步的安全输入输出摘要是什么、使用了哪个配置与模型、为何选择端侧或
云端、执行了什么能力、在哪里失败以及最终状态是否有执行证据。

Trace 是事实协议，DataLog 是默认事实存储，CLI、Viewer 和 OTLP 都只是消费者。Viewer
不得读取 Runtime 私有对象，也不得成为业务状态来源。

## 2. 标准链路

标准阶段依次为：`ingress`、`config`、`preprocess`、`memory`、`rule_match`、
`local_classification`、`retrieval`、`prompt_assembly`、`local_inference`、
`decision_validation`、`cloud_arbitration`、`cloud_inference`、`orchestration`、
`execution`、`reconciliation`、`response`、`trace_finalize`。

没有执行的预期阶段记录为 `SKIPPED`，执行阶段使用 `STARTED`、`SUCCEEDED`、`FAILED`、
`CANCELLED` 或 `TIMEOUT`。事件以单调时钟、生产者序列和因果父事件共同排序，不能仅凭
跨进程墙上时间推断严格因果关系。

## 3. 协议

公共 Trace 投影使用 `masteragent.trace/v1alpha1`，包含：

- `trace_id`、`request_id`、`span_id`、可选父事件及 plan/execution 身份；
- `stage`、`event_type`、`operation`、`status` 和模块；
- UTC 时间、单调时间、持续时间（存在成对事件时）；
- 输入输出摘要、Artifact 引用与摘要哈希；
- 配置包、模型、Prompt 和策略的稳定 ID/摘要；
- 决策原因、错误引用、隐私标签和持久化等级。

当前 DataLog `LogEvent` 是唯一持久化记录。扩展信息放在受大小和脱敏规则约束的
`payload_summary_json` 中；TraceProjector 将其归一为公共视图。这样不建立第二套日志，
也不破坏已有 journal 恢复与审计机制。

## 4. 隐私模式

- `safe`：默认，只保存脱敏摘要、长度、类型、摘要哈希和引用。
- `local-debug`：显式开启后允许在本机保存受限原始 Prompt/模型输出，短期留存。
- `audit`：保存配置、决策、执行回执与摘要哈希，不保存会话原文。

凭证、密钥和认证头在所有模式下禁止记录。大载荷单独存为受控 Artifact，Trace 只保存
引用和哈希。开源发行版不默认上传任何 Trace。

## 5. 组件

```text
业务模块 -> TraceEmitter -> DataLog -> TraceProjector
                                      -> sparx trace
                                      -> Trace Viewer
                                      -> JSON/OTLP Exporter（可选）
```

端侧 Core 只依赖轻量内部协议。OpenTelemetry 通过可选 OTLP Exporter 接入，避免让嵌入式
Runtime 强依赖完整 SDK。

## 6. CLI 与 Viewer

CLI 支持从 Runtime 的 `data_log/events.jsonl` 读取：

```bash
sparx trace list <runtime>/data_log/events.jsonl
sparx trace show <runtime>/data_log/events.jsonl --last
sparx trace show <runtime>/data_log/events.jsonl --trace <trace-id>
sparx trace export <runtime>/data_log/events.jsonl --trace <trace-id>
```

Trace Viewer 同时提供两种方式：

- 实时交互：本地桥接服务调用真实 MasterAgent，并持续读取 DataLog journal，以 NDJSON
  将新事件推送到中文网页；用户在同一页面输入请求、查看回复和执行过程。
- 历史查看：读取 `trace export` 生成的 JSON，离线还原一次请求。

本地桥接服务只监听 `127.0.0.1`。常规 DataLog 投影仍只包含安全摘要；实时面板属于显式
`local-debug` 工具，同时读取独立、短期的本地调试 Artifact。调试 Artifact 展示原始请求、
预处理结果、记忆片段、Skill 候选、Tool 定义、完整 Prompt、模型原始输出、解析后决策、
执行计划和 Tool/子 Agent 返回值。页面把每个步骤统一呈现为“输入、操作、输出、原因”。

原始调试内容不得写入默认 `events.jsonl`，不得上传，也不得包含认证头或密钥；每次运行
存入独立临时目录的 `local_debug/pipeline.jsonl` 与 `local_debug/model_io.jsonl`。关闭显式
调试开关后不创建这些文件，保证开源版本和生产默认行为仍然是安全模式。

## 7. 兼容与演进

现有 Orchestrator TaskEvent JSONL 继续兼容读取。应用层手工拼接的 `inference_trace` 与
`cloud_trace` 在统一事件接入完成前保留，之后由相同 Trace 投影生成。协议新增字段保持
向后兼容；破坏性变化必须升级 schema version。

## 8. 验收

每个正常或失败请求必须可按 `trace_id` 查询；关键阶段必须有终态或 `SKIPPED`；模型与
执行事件必须包含运行时/能力身份和安全摘要；敏感字段测试必须通过；关闭观测后不能
改变业务语义；Runtime 异常退出后可从已提交 journal 恢复 Trace。
