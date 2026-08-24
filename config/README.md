# Legacy Runtime Configuration

`config/` 是当前 C++ Runtime 仍在使用的过渡配置，不是新的 MasterAgent 配置包。

当前依赖：

- `src/prompt/` 默认读取 `config/prompt/templates.json`。
- `src/skill/` 默认读取 `config/skill/`。
- `tests/test_prompt_skill.cpp` 直接验证这两类资源。

新功能应写入 `agent-config/`，不要继续扩展这个目录。统一 ConfigBundle Loader 完成以前不能删除此目录，否则 Prompt/Skill 启动和相关测试会失败。

删除门槛：

1. `agent-config` 已经能编译成不可变配置快照。
2. PromptEngine 和 SkillEngine 只从快照读取。
3. 官方包与旧配置的行为对等测试通过。
4. 安装包不再安装 `config/`。

完整迁移计划见 `docs/13_项目目录整改与配置包迁移计划.md`。
