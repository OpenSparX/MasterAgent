# Capabilities

Capability 是 Provider 边界上可独立校验、鉴权、执行和确认结果的最小能力。

官方包只启用了当前参考 Runtime 实际注册的两个 climate Tool。`docs/mcpTools_second_level_classified.xlsx` 中的异车能力只可作为语义和分类参考；没有本车 Provider、输出 Schema、权限、幂等和完成策略时不得复制后直接启用。

新增 Capability 的最低要求：

1. 稳定 ID 和 MCP Tool 名称。
2. 封闭 input/output JSON Schema。
3. 明确读写、副作用、风险和权限。
4. 明确超时、重试、幂等、取消、完成证据和对账策略。
5. 对应 Provider 已在 Runtime 注册并通过契约测试。

