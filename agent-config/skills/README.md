# Skills

Skill 是任务知识和允许能力的组合，不是最小执行接口。

每个 Skill 必须明确：触发语义、槽位、允许的信息源、允许的写能力、允许的 SubAgent、最大查询/动作数和回退行为。Skill 只能引用同一配置快照中已存在且启用的资源。

新增 Skill 时优先复制一个现有文件并修改稳定 `metadata.id`。不要在 `instructions` 中写入一个未在 `allowed_capabilities` 或 `allowed_agents` 声明的执行入口。

