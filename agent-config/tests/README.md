# Config Tests

配置测试描述输入、允许路径和关键不变量。目标 `sparx config validate` 应至少检查：

- Rule 的匹配、槽位和期望 Decision。
- Skill 引用的 ContextSource、Capability 和 Agent 存在且启用。
- 规则/模型不能选择未注册能力。
- 缺槽位时不产生副作用。
- 官方没有 Provider 的领域不会伪造执行成功。
- 默认端云策略保持关闭。
- 模型只输出 Decision JSON，未知 Capability 和自由文本执行声明被拒绝。
- RequestCloud 使用理由白名单，云端输出回到本地 Validator。

这些文件是配置发布门禁，不替代 C++ 单元测试和真实 Provider 契约测试。

`smoke.yaml` 描述最小业务行为，`intent-json-protocol.yaml` 描述模型协议，`cloud-request-contract.yaml` 描述端云合同。ModelProfile 中的 `required_test_suites` 必须引用这里真实存在的 `metadata.id`。当前 Ruby 基础校验器检查引用完整性；用例执行器将在 ConfigBundle Loader 阶段接入。
