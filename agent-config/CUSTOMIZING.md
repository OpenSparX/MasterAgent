# 自定义 MasterAgent 配置包

## 1. 复制官方基线

```bash
cp -R agent-config my-car-config
```

修改 `my-car-config/manifest.yaml` 中的名称和版本。目录名不参与身份判断。

修改后先运行当前基础校验器：

```bash
ruby scripts/validate_agent_config.rb ./my-car-config
```

## 2. 从小范围开始

推荐先选一个业务域：

1. 在 `capabilities/` 声明已经真实注册的 Tool。
2. 在 `context_sources/` 声明所需只读信息。
3. 在 `skills/` 声明允许使用这些资源的 Skill。
4. 在 `rules/` 为高频明确表达建立确定性规则。
5. 在 `models/` 选择已有 Runtime Adapter 的端侧/云端模型，权重和凭证只写外部引用。
6. 在 `policies/` 设置风险、权限、模型选择和端云策略。
7. 在 `tests/` 增加成功、缺槽位、越权、模型协议和 Provider 不可用用例。

## 3. 替换和切换

开源项目推荐“新建并切换”，不推荐覆盖官方 `agent-config`。目标查找顺序为：

1. `sparx run --config <path>` 显式路径。
2. `.sparx/active-config.json` 指向的已激活配置。
3. 项目 `./agent-config`。
4. 安装包内置只读官方配置。

显式配置无效时必须失败，不能静默退回官方配置。

目标命令：

```bash
sparx config init my-car-config
sparx config validate ./my-car-config
sparx config use ./my-car-config
sparx config current
sparx config pack ./my-car-config -o my-car-config.masbundle
sparx config use default
```

这些命令目前是下一阶段 CLI 契约；在实现前通过复制目录和版本控制管理自定义包。

## 4. 不允许通过配置完成的事情

- 执行 Shell、任意脚本、动态库或未注册 URL。
- 绕过权限、Schema、幂等、WAL、fencing、取消或对账。
- 把异车接口直接标记为本车可用。
- 让模型自由生成 Capability 或 Agent 名称。
- 把云端候选输出当成本地执行成功。
- 把大模型权重、API 密钥或真实凭证提交进配置包。
