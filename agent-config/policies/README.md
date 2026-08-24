# Policies

`observability.yaml` 控制全链路追踪、载荷脱敏、留存和可选导出。官方包默认使用
`safe` 模式，只记录摘要与摘要哈希；`local-debug` 必须由使用者明确开启，凭证在任何
模式下都禁止进入 Trace。

Policy 描述安全、检索、推理、端云、权限和 deadline 策略。

`immutable_safety_floor` 是对代码不变量的公开说明，不代表用户能够通过改成 `false` 关闭它。生产加载器必须拒绝任何弱化安全底线的配置。

官方包默认关闭云端。当前开发期假云端只能由显式运行参数开启，真实云 Provider、授权和隐私策略接入前不应默认启用。
