# 交互接入与 AgentService

## 1. InteractionLayer

`InteractionLayer` 是外部文本的唯一标准入口，负责：

- 校验文本大小、UTF-8、用户和会话字段。
- 生成 `request_id`、`trace_id` 和持久化 `turn_id`。
- 固化 `priority` 和绝对单调时钟 deadline。
- 恢复重启前的会话轮次下界。

输出为不可变的 `StandardRequest`。下游不得从业务 payload 重新构造可信身份。

## 2. AgentService

`AgentService` 协调一个 Turn：

1. 校验调用方必须为 `InteractionIngress`。
2. 调用 `IPreprocess.process` 和 Memory；只有业务确实需要运行状态时，才单独调用 `IStateQuery.getCapabilities/queryRuntimeState`。
3. 调用 Intent 获取闭合结果。
4. 对 `PLAN` 提交 Orchestrator。
5. 对 `ASK`、`REPLY`、`FAIL` 直接形成 `TurnResult`。
6. 写入短期记忆、事件和异常。

AgentService 不直接执行 Tool，不修改计划内部状态，也不把模型原始文本直接返回用户。

预处理基础流程不读取车辆或环境实时值。每个 `StateDomain` 只能注册一个 Provider；重复注册会使状态查询接口返回 `PREPROCESS_STATE_PROVIDER_DOMAIN_CONFLICT`，不会合并字段或按注册顺序选择。能力发现只返回 Provider 声明的状态类型和字段；按需查询只读取调用方明确列出的字段，状态查询失败是否阻断后续流程由 AgentService 决定。

## 3. 输出语义

`TurnResult` 包含：

- 请求、trace、会话和轮次身份。
- 用户可见 `reply`。
- `success` 和 `pending`。
- `plan_id` 和可选 `plan_state`。
- 受控错误码和安全错误信息。

`pending=true` 表示请求已可靠接收但尚未终结。`plan_state=Unknown` 表示副作用事实无法确认；调用方应按 `plan_id` 查询或对账，不应重放原始命令。

## 4. 会话并发

`MasterAgentRuntime` 在分配 TurnID 前获得会话 lane。同一用户和会话保持因果顺序，不同会话可以并发。lane 使用弱引用，闲置会话不会永久占用内存。
