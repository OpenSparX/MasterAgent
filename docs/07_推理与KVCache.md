# 推理框架与 KV Cache

## 1. 推理作业

`submitInference()` 只确认作业进入调度域。`queryInference()` 返回权威快照。请求包含：

- job、request、trace 和父任务身份。
- priority 和 absolute deadline。
- model/profile、Prompt 协议版本和 Prompt 摘要。
- 幂等键、资源预算和父级 fencing。

## 2. 调度

Inference Framework 管理逻辑 Replica、队列和资源租约。选择顺序为优先级、deadline、入队序号。P0 到达时可以请求 P2 在 checkpoint-safe 边界让出 Replica。

## 3. 外部调用封印

模型调用在状态锁外执行。调用前冻结：

- job 和 attempt。
- replica_id 和 replica_epoch。
- fencing_token 和 control_epoch。
- KV lease。
- Prompt 摘要和协议版本。

模型返回后全部匹配才可提交输出。取消、抢占或 Replica rebuild 会使旧回调失效。

## 4. Mock 模型

交付包使用 `MockModelRuntime`，不会访问真实模型。它提供确定性输出、可控工作单元和调用追踪，用于验证：

- 第一阶段 QUERY/FINAL 分支。
- 第二阶段 final-only 约束。
- P0/P2 安全抢占。
- deadline、取消和恶意输出。

## 5. KV Cache

KV key 同时绑定模型、adapter、Prompt 协议、token/cache ABI 和上下文摘要。不同协议版本不能错误复用。

lease 在使用完成前保护 entry。失效、过期、取消和 publish 失败都有独立状态；清理使用轮转游标，单个故障 lease 不能阻塞其他清理任务。
