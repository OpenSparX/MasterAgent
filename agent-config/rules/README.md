# Rules

Rule 是模型之前的确定性快速路径。规则产出 `Reply`、`Ask` 或 `ExecutionPlan`，但不能直接执行代码。

要求：

- 多条规则冲突时必须使用显式冲突策略。
- 必填槽位缺失必须 Ask，不得猜测高风险参数。
- `ExecutionPlan` 只能引用启用的 Capability。
- 规则结果仍要通过统一 Decision Validator、Schema、权限和可用性校验。

当前文件表达的是目标 `v1alpha1` 配置格式；旧 Runtime 中的规则加载器仍使用摘要封装的 JSON artifact。

