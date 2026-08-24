# Schemas

这里保存配置资源和模型 Decision 的 JSON Schema。YAML 加载后先转换为普通对象，再执行 Schema、未知字段、唯一 ID 和跨资源引用校验。

- `resource-envelope.schema.json` 是公共外壳。
- `model-profile.schema.json` 定义端侧/云端模型身份、Runtime、外部引用和协议基线。
- `model-routing-policy.schema.json` 定义角色默认模型、端侧/云端候选、路由顺序和生产门槛。
- `prompts/decision-protocol.schema.json` 是候选 Decision 的闭集协议。

正式 Loader 实现时应继续拆分每种 `kind` 的完整 Schema，并把 Schema 版本绑定到配置快照摘要。
