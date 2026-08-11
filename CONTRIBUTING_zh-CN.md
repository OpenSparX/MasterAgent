# 贡献指南

感谢您对Sparx的关注！我们欢迎所有形式的贡献，从文档改进到功能开发。

## 行为准则

参与本项目即表示您同意遵守我们的行为准则。我们致力于为所有人提供友好、尊重和包容的社区环境。

## 如何贡献

### 报告Bug

发现Bug？请[创建Issue](https://github.com/OpenSparX/MasterAgent/issues/new)并包含：

- **环境信息**：操作系统、硬件平台（SA8155P/SA8295P等）、Sparx版本
- **复现步骤**：清晰的步骤说明
- **预期行为**：您期望发生什么
- **实际行为**：实际发生了什么
- **日志输出**：相关的错误日志或调试信息

**示例**：
```markdown
### 环境
- OS: Linux 5.10
- Platform: SA8295P
- Sparx: v2.1.6

### 复现步骤
1. sparx init test-agent
2. cd test-agent
3. sparx run

### 预期
Agent启动并等待输入

### 实际
进程崩溃，错误信息：segmentation fault

### 日志
[附加.sparx/agent.log内容]
```

### 提出新功能

有功能建议？请先[创建讨论](https://github.com/OpenSparX/MasterAgent/discussions)：

- 描述功能目的和使用场景
- 说明为什么现有功能无法满足需求
- 如果可能，提供API设计草图或示例代码

功能讨论获得认可后，我们会将其转为Issue并标记`enhancement`。

### 改进文档

文档贡献同样重要！您可以：

- 修正错别字或语法错误
- 改进现有文档的清晰度
- 添加更多示例或用例
- 翻译文档（中英互译）

文档在`docs/`目录下，使用Markdown格式。小改动直接PR，大改动请先开Issue讨论。

### 贡献代码

#### 开发环境设置

```bash
# 1. Fork并克隆仓库
git clone https://github.com/YOUR_USERNAME/MasterAgent.git
cd MasterAgent/v2

# 2. 安装依赖
# macOS
brew install cmake ninja nlohmann-json spdlog

# Ubuntu
apt-get install cmake ninja-build nlohmann-json3-dev libspdlog-dev

# 3. 构建
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build --parallel

# 4. 运行测试
ctest --test-dir build --output-on-failure
```

#### 代码风格

- **C++**：遵循[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- **缩进**：2空格（不是Tab）
- **命名**：
  - 类/结构体：`PascalCase`
  - 函数/变量：`snake_case`
  - 常量/枚举：`UPPER_CASE`
  - 私有成员：`trailing_underscore_`
- **注释**：使用`//`单行注释，文档注释使用`///`

**示例**：
```cpp
/// 表示Agent的一次交互回合
class TurnContext {
 public:
  /// 构造函数
  TurnContext(std::string session_id, int turn_number);

  /// 获取会话ID
  std::string session_id() const { return session_id_; }

 private:
  std::string session_id_;
  int turn_number_;
};
```

#### 提交消息规范

使用[Conventional Commits](https://www.conventionalcommits.org/)格式：

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Type**：
- `feat`: 新功能
- `fix`: Bug修复
- `docs`: 文档更新
- `test`: 测试相关
- `refactor`: 代码重构
- `perf`: 性能优化
- `build`: 构建系统或依赖更新
- `ci`: CI配置更改

**示例**：
```
feat(mcp): add retry logic for MCP service calls

- Implement exponential backoff with jitter
- Add max_attempts configuration option
- Update tests to cover retry scenarios

Closes #123
```

#### Pull Request流程

1. **创建分支**：从`main`创建功能分支
   ```bash
   git checkout -b feat/my-feature
   ```

2. **开发并测试**：确保所有测试通过
   ```bash
   cmake --build build
   ctest --test-dir build
   ```

3. **提交更改**：遵循提交消息规范
   ```bash
   git add .
   git commit -m "feat(skill): add new skill type for IoT devices"
   ```

4. **推送到Fork**：
   ```bash
   git push origin feat/my-feature
   ```

5. **创建Pull Request**：
   - 清晰描述改动内容和目的
   - 关联相关Issue（`Closes #123`）
   - 添加测试覆盖
   - 确保CI通过

6. **代码审查**：维护者会审查您的代码并提供反馈

7. **合并**：审查通过后，维护者会合并您的PR

#### PR检查清单

提交PR前请确认：

- [ ] 所有测试通过（`ctest --test-dir build`）
- [ ] 代码遵循项目风格
- [ ] 添加了必要的测试
- [ ] 更新了相关文档
- [ ] 提交消息符合规范
- [ ] 没有引入Qualcomm专有代码（见下文）
- [ ] 构建脚本通过（`scripts/build_release.sh`）

### 特别注意：Qualcomm许可

**重要**：以下文件和目录**不得**进入公开仓库：

- `genai_lib/`目录下的所有文件
- 文件名匹配`qnn_model_prepare_*.py`的所有文件
- `utilities/`目录下的Qualcomm专有工具
- 任何带有 Qualcomm 专有版权头部的文件（形如 `Confidential and` + `Proprietary - Qualcomm` + `Technologies, Inc.` 的连续声明）

> 注意：上面刻意把该头部拆分书写。若在文档中原样粘贴完整头部，`scripts/check_license.sh` 会把这份文档本身判定为专有文件并阻止发布。

CI会自动检查这些限制（`scripts/tests/test_licence_gate.sh`）。

如果您需要与NPU相关的功能：
1. 使用抽象接口而不是直接依赖QNN SDK
2. 在`src/inference/`下添加接口层
3. 实际QNN集成留给商业部署

## 新手友好Issue

寻找入门任务？查看标记为[`good first issue`](https://github.com/OpenSparX/MasterAgent/labels/good%20first%20issue)的Issue。

这些Issue：
- 范围明确
- 不需要深入理解整个代码库
- 有清晰的验收标准
- 维护者会提供指导

## 开发技巧

### 运行单个测试

```bash
# 只运行特定测试
ctest --test-dir build -R test_mcp_wire_contracts -V

# 调试失败的测试
ctest --test-dir build -R test_orchestrator --output-on-failure
```

### 本地测试packaging脚本

```bash
# 测试安装脚本（不需要发布artifact）
./scripts/tests/run_all.sh

# 会运行：
# - test_triple_contract.sh  # 平台检测
# - test_licence_gate.sh     # Qualcomm许可检查
# - test_cli_commands.sh     # CLI功能
```

### Mock模式开发

开发Skill或MCP服务时，使用Mock模式避免依赖真实模型：

```yaml
# agent.yaml
inference:
  backend: mock
  mock_responses:
    intent: climate_control
    parameters:
      temperature: 22
```

### 调试日志

```yaml
# agent.yaml
logging:
  level: debug              # error | warn | info | debug | trace
  output: stdout
  format: pretty            # json | pretty
```

## 许可协议

贡献代码即表示您同意在[Apache 2.0许可](../LICENSE)下发布您的代码。

## 获取帮助

- **讨论区**：[GitHub Discussions](https://github.com/OpenSparX/MasterAgent/discussions)
- **实时聊天**：[Discord社区](https://discord.gg/opensparx)（待创建）
- **邮件**：dev@opensparx.org

---

再次感谢您的贡献！每一份改进都让Sparx变得更好。
