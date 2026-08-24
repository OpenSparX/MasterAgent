# Context Sources

ContextSource 把记忆、车辆状态、环境状态、Skill 检索和只读 Tool 查询纳入同一个受控 QueryBatch 视图。

每项信息源必须声明：只读属性、允许字段、访问者、权限、时效、大小、隐私分类和是否允许上云。模型不能使用 ContextSource 执行写操作。

这里的车辆和环境值来自当前参考 Runtime 的 Host Test Provider，生产集成必须用真实 Provider 替换，不能把演示值当成车辆事实。

