# Contributing to Sparx

[English](#english) | [中文](#中文)

---

<a name="english"></a>

## Welcome!

Thank you for your interest in contributing to Sparx! We're building the first open-source AI Agent framework optimized for edge devices, and we'd love your help.

Whether you're fixing bugs, adding features, improving documentation, or sharing feedback, every contribution matters.

---

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Ways to Contribute](#ways-to-contribute)
- [Development Setup](#development-setup)
- [Code Style](#code-style)
- [Testing](#testing)
- [Pull Request Process](#pull-request-process)
- [Good First Issues](#good-first-issues)
- [Community](#community)

---

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md). By participating, you agree to uphold this code. Please report unacceptable behavior to dev@openschbrid.com.

---

## Ways to Contribute

### 🐛 Report Bugs

Found a bug? Please open an issue with:
- Clear title describing the problem
- Steps to reproduce
- Expected vs actual behavior
- Environment (OS, device, Sparx version)
- Logs or error messages

Use the [bug report template](.github/ISSUE_TEMPLATE/bug_report.md).

### ✨ Request Features

Have an idea? Open a feature request with:
- Use case: what problem does this solve?
- Proposed solution
- Alternatives considered

Use the [feature request template](.github/ISSUE_TEMPLATE/feature_request.md).

### 📖 Improve Documentation

- Fix typos or unclear explanations
- Add examples or tutorials
- Translate docs to other languages
- Improve code comments

### 💻 Contribute Code

- Fix bugs from the issue tracker
- Implement approved feature requests
- Optimize performance
- Add tests

---

## Development Setup

### Prerequisites

**Required:**
- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- CMake 3.20+
- Ninja build system
- Git

**Optional (for full NPU support):**
- Qualcomm QNN SDK 2.x (requires NDA with Qualcomm)

### Clone and Build

```bash
# Clone the repository
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent/v2

# Create build directory
mkdir -p build && cd build

# Configure
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
ninja

# Run tests
ctest --output-on-failure
```

### Project Structure

```
v2/
├── cli/                    # Command-line interface
│   └── src/
│       ├── sparx_main.cpp  # Entry point
│       └── cmd_*.cpp       # Individual commands
├── libagent/               # Core Agent framework
│   ├── preprocessing/      # Input validation
│   ├── memory/             # Context management
│   ├── skills/             # Skill system
│   ├── reasoning/          # Intent recognition
│   ├── orchestrator/       # Task orchestration
│   ├── atomic_service/     # MCP services
│   └── persistence/        # WAL recovery
├── tests/                  # Unit and integration tests
├── docs/                   # Documentation
├── packaging/              # Distribution (npm, Homebrew)
└── scripts/                # Build and release scripts
```

---

## Code Style

### C++ Guidelines

We follow modern C++17 practices:

**Naming:**
- Classes: `PascalCase` (e.g., `TaskOrchestrator`)
- Functions: `camelCase` (e.g., `executeTask()`)
- Variables: `snake_case` (e.g., `task_id`)
- Constants: `UPPER_SNAKE_CASE` (e.g., `MAX_RETRIES`)
- Files: `snake_case.cpp` / `.h`

**Formatting:**
- Indent: 4 spaces (no tabs)
- Line length: 100 characters max
- Braces: K&R style (opening brace on same line)

**Example:**
```cpp
namespace sparx {

class TaskOrchestrator {
public:
    bool executeTask(const std::string& task_id) {
        if (task_id.empty()) {
            return false;
        }
        // Implementation
        return true;
    }

private:
    static constexpr int MAX_RETRIES = 3;
    std::unordered_map<std::string, Task> tasks_;
};

}  // namespace sparx
```

**Best Practices:**
- Prefer `std::string_view` over `const std::string&` for read-only strings
- Use `const` liberally
- Avoid raw pointers; use smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- RAII for resource management
- Explicit is better than implicit: mark single-arg constructors `explicit`

### Shell Scripts

- Use `#!/usr/bin/env bash` or `#!/bin/sh` (POSIX)
- Add `set -euo pipefail` at the top
- Quote variables: `"$VAR"` not `$VAR`
- Use `[[` for conditionals in bash, `[` in POSIX sh

### Documentation

- Every public function/class needs a docstring
- Use Doxygen style:
```cpp
/**
 * @brief Execute a task by ID
 * @param task_id Unique identifier for the task
 * @return true if successful, false otherwise
 */
bool executeTask(const std::string& task_id);
```

---

## Testing

### Running Tests

```bash
# All tests
ctest --test-dir build --output-on-failure

# Specific test
ctest --test-dir build -R test_preprocess

# List every test target
ctest --test-dir build -N

# With verbose output
ctest --test-dir build --verbose
```

### Writing Tests

There is no Google Test dependency. Tests are plain executables that use the
`expect()` helper from `tests/test_support.h`, which throws on failure; `main()`
catches and returns non-zero. This keeps the test tree buildable with nothing but
a compiler and CMake, which matters because the kernel is meant to be embeddable
on targets where fetching a test framework is not an option.

```cpp
// tests/test_orchestrator_controls.cpp
#include <iostream>
#include "master_agent/orchestrator/orchestrator.h"
#include "test_support.h"

using master_agent::test_support::expect;

namespace {

void testRejectsEmptyDagId() {
    // ... build a DAG with no dag_id ...
    const auto result = orchestrator.validateDAG(dag, admission, call);
    expect(!result.valid, "a DAG with no dag_id must be rejected");
    expect(result.reject_code == "ORCHESTRATOR_DAG_INVALID",
           "rejection names the reason, got: " + result.reject_code);
}

}  // namespace

int main() {
    try {
        testRejectsEmptyDagId();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "orchestrator control tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
```

Register the target in `tests/CMakeLists.txt`:

```cmake
add_master_agent_test(test_my_feature test_my_feature.cpp)
```

**Write the failure message, not just the assertion.** `expect()` reports only the
string you give it, so include the value that was wrong — `"got: " +
result.reject_code` turns a red test into a diagnosis.

**Verify the test can fail.** An assertion that passes against broken code is
worse than no assertion, because it certifies the bug. Before trusting a new
test, break the thing it covers and confirm the test goes red.

**Test Requirements:**
- Every new feature must include tests
- Bug fixes should include a regression test
- Aim for >80% code coverage on new code

### Packaging Tests

```bash
# Build release artifacts
./scripts/build_release.sh

# Run packaging tests (install flow, npm, Homebrew)
./scripts/tests/run_all.sh
```

---

## Pull Request Process

### Before You Start

1. **Check existing issues**: avoid duplicate work
2. **Open an issue first** for large features (discuss design before coding)
3. **Fork the repository** and create a feature branch

### Branch Naming

- `feature/add-distributed-orchestration`
- `fix/wal-corruption-on-sigkill`
- `docs/update-mcp-guide`
- `refactor/simplify-skill-matcher`

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <description>

[optional body]

[optional footer]
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation only
- `refactor`: Code change that neither fixes a bug nor adds a feature
- `test`: Adding or updating tests
- `chore`: Build process, tooling, dependencies

**Examples:**
```
feat(orchestrator): add DAG-based task execution

Implements parallel task execution with dependency resolution.
Tasks with no dependencies run concurrently, reducing latency by 40%.

Closes #123
```

```
fix(wal): prevent corruption on SIGKILL

WAL now uses fdatasync() before marking records committed,
ensuring durability even on sudden power loss.

Fixes #456
```

### PR Checklist

Before submitting:

- [ ] Code follows the style guidelines
- [ ] Tests pass locally (`ctest`)
- [ ] New tests added for new features/fixes
- [ ] Documentation updated (if API changed)
- [ ] Commit messages follow Conventional Commits
- [ ] No merge conflicts with `main`

### Review Process

1. **Automated checks**: CI must pass (build, tests, licence gate)
2. **Code review**: A maintainer will review within 3 business days
3. **Revisions**: Address feedback and push updates
4. **Merge**: Maintainer will merge once approved

**What reviewers look for:**
- Correctness: does it work as intended?
- Edge cases: error handling, boundary conditions
- Performance: any regressions?
- Maintainability: is it readable and well-tested?
- Security: any potential vulnerabilities?

---

## Good First Issues

New to the project? Look for issues tagged [`good first issue`](https://github.com/OpenSparX/MasterAgent/labels/good%20first%20issue):

**Suggested starter tasks:**
- Add a new MCP service (e.g., weather API)
- Improve error messages in the CLI
- Write examples for the automotive demo
- Translate documentation to another language
- Add unit tests for uncovered modules

**Not sure where to start?** Comment on an issue saying "I'd like to work on this" and a maintainer will guide you.

---

## Community

- 💬 **Discussions**: [GitHub Discussions](https://github.com/OpenSparX/MasterAgent/discussions) — ask questions, share ideas
- 🐛 **Issues**: [GitHub Issues](https://github.com/OpenSparX/MasterAgent/issues) — report bugs, request features
- 📧 **Email**: dev@openschbrid.com — security issues, private inquiries

---

## License

By contributing, you agree that your contributions will be licensed under the [Apache 2.0 License](LICENSE).

---

<a name="中文"></a>

# 贡献指南

## 欢迎！

感谢您对Sparx项目的关注！我们正在打造首个面向边缘设备优化的开源AI Agent框架，期待您的参与。

无论是修复Bug、添加功能、改进文档，还是分享反馈，每一份贡献都很重要。

---

## 目录

- [行为准则](#行为准则)
- [贡献方式](#贡献方式)
- [开发环境搭建](#开发环境搭建)
- [代码风格](#代码风格)
- [测试](#测试)
- [Pull Request流程](#pull-request流程)
- [新手友好Issue](#新手友好issue)
- [社区](#社区)

---

## 行为准则

本项目遵循[贡献者公约行为准则](CODE_OF_CONDUCT.md)。参与项目即表示您同意遵守此准则。如遇不当行为，请发送邮件至 dev@openschbrid.com 举报。

---

## 贡献方式

### 🐛 报告Bug

发现Bug？请创建Issue并包含：
- 清晰描述问题的标题
- 复现步骤
- 预期行为 vs 实际行为
- 环境信息（操作系统、设备、Sparx版本）
- 日志或错误信息

使用[Bug报告模板](.github/ISSUE_TEMPLATE/bug_report.md)。

### ✨ 功能建议

有想法？创建功能请求并说明：
- 使用场景：解决什么问题？
- 建议方案
- 其他备选方案

使用[功能请求模板](.github/ISSUE_TEMPLATE/feature_request.md)。

### 📖 改进文档

- 修正拼写错误或不清晰的表述
- 添加示例或教程
- 翻译文档至其他语言
- 改进代码注释

### 💻 贡献代码

- 修复Issue追踪器中的Bug
- 实现已批准的功能请求
- 性能优化
- 添加测试

---

## 开发环境搭建

### 前置要求

**必需：**
- C++17编译器（GCC 9+、Clang 10+、MSVC 2019+）
- CMake 3.20+
- Ninja构建系统
- Git

**可选（完整NPU支持）：**
- Qualcomm QNN SDK 2.x（需与Qualcomm签订NDA）

### 克隆与构建

```bash
# 克隆仓库
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent/v2

# 创建构建目录
mkdir -p build && cd build

# 配置
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 构建
ninja

# 运行测试
ctest --output-on-failure
```

### 项目结构

```
v2/
├── cli/                    # 命令行界面
│   └── src/
│       ├── sparx_main.cpp  # 入口点
│       └── cmd_*.cpp       # 各个命令实现
├── libagent/               # 核心Agent框架
│   ├── preprocessing/      # 输入验证
│   ├── memory/             # 上下文管理
│   ├── skills/             # 技能系统
│   ├── reasoning/          # 意图识别
│   ├── orchestrator/       # 任务编排
│   ├── atomic_service/     # MCP服务
│   └── persistence/        # WAL恢复
├── tests/                  # 单元和集成测试
├── docs/                   # 文档
├── packaging/              # 分发包（npm、Homebrew）
└── scripts/                # 构建和发布脚本
```

---

## 代码风格

### C++规范

我们遵循现代C++17实践：

**命名：**
- 类：`PascalCase`（如 `TaskOrchestrator`）
- 函数：`camelCase`（如 `executeTask()`）
- 变量：`snake_case`（如 `task_id`）
- 常量：`UPPER_SNAKE_CASE`（如 `MAX_RETRIES`）
- 文件：`snake_case.cpp` / `.h`

**格式化：**
- 缩进：4个空格（不用Tab）
- 行宽：最多100字符
- 花括号：K&R风格（左花括号同行）

**示例：**
```cpp
namespace sparx {

class TaskOrchestrator {
public:
    bool executeTask(const std::string& task_id) {
        if (task_id.empty()) {
            return false;
        }
        // 实现
        return true;
    }

private:
    static constexpr int MAX_RETRIES = 3;
    std::unordered_map<std::string, Task> tasks_;
};

}  // namespace sparx
```

**最佳实践：**
- 优先使用`std::string_view`而非`const std::string&`（只读字符串）
- 大量使用`const`
- 避免裸指针；使用智能指针（`std::unique_ptr`、`std::shared_ptr`）
- RAII管理资源
- 单参数构造函数标记`explicit`

### Shell脚本

- 使用`#!/usr/bin/env bash`或`#!/bin/sh`（POSIX）
- 开头添加`set -euo pipefail`
- 引用变量：`"$VAR"`而非`$VAR`
- bash中用`[[`条件判断，POSIX sh中用`[`

### 文档注释

- 每个公开函数/类需要文档字符串
- 使用Doxygen风格：
```cpp
/**
 * @brief 根据ID执行任务
 * @param task_id 任务的唯一标识符
 * @return 成功返回true，失败返回false
 */
bool executeTask(const std::string& task_id);
```

---

## 测试

### 运行测试

```bash
# 所有测试
ctest --test-dir build --output-on-failure

# 特定测试
ctest --test-dir build -R preprocessing_test

# 详细输出
ctest --test-dir build --verbose
```

### 编写测试

我们使用Google Test。每个模块都有对应的测试文件：

```cpp
// tests/orchestrator_test.cpp
#include <gtest/gtest.h>
#include "orchestrator/task_orchestrator.h"

namespace sparx {
namespace test {

TEST(OrchestratorTest, ExecuteSimpleTask) {
    TaskOrchestrator orch;
    ASSERT_TRUE(orch.executeTask("task_1"));
}

TEST(OrchestratorTest, RejectEmptyTaskId) {
    TaskOrchestrator orch;
    ASSERT_FALSE(orch.executeTask(""));
}

}  // namespace test
}  // namespace sparx
```

**测试要求：**
- 每个新功能必须包含测试
- Bug修复应包含回归测试
- 新代码覆盖率目标>80%

### 打包测试

```bash
# 构建发布产物
./scripts/build_release.sh

# 运行打包测试（安装流程、npm、Homebrew）
./scripts/tests/run_all.sh
```

---

## Pull Request流程

### 开始之前

1. **检查已有Issue**：避免重复工作
2. **大型功能先开Issue**（先讨论设计再编码）
3. **Fork仓库**并创建功能分支

### 分支命名

- `feature/add-distributed-orchestration`
- `fix/wal-corruption-on-sigkill`
- `docs/update-mcp-guide`
- `refactor/simplify-skill-matcher`

### Commit信息

遵循[约定式提交](https://www.conventionalcommits.org/zh-hans/):

```
<类型>(<范围>): <描述>

[可选的正文]

[可选的脚注]
```

**类型：**
- `feat`：新功能
- `fix`：Bug修复
- `docs`：仅文档变更
- `refactor`：既不修Bug也不加功能的代码变更
- `test`：添加或更新测试
- `chore`：构建流程、工具、依赖

**示例：**
```
feat(orchestrator): 添加基于DAG的任务执行

实现依赖解析的并行任务执行。
无依赖的任务并发运行，延迟降低40%。

Closes #123
```

```
fix(wal): 防止SIGKILL导致的损坏

WAL现在在标记记录已提交前使用fdatasync()，
确保即使突然断电也能保证持久性。

Fixes #456
```

### PR检查清单

提交前：

- [ ] 代码遵循风格指南
- [ ] 测试在本地通过（`ctest`）
- [ ] 为新功能/修复添加了新测试
- [ ] 文档已更新（如果API变更）
- [ ] Commit信息遵循约定式提交
- [ ] 与`main`分支无冲突

### 审查流程

1. **自动检查**：CI必须通过（构建、测试、许可证检查）
2. **代码审查**：维护者将在3个工作日内审查
3. **修订**：处理反馈并推送更新
4. **合并**：批准后维护者将合并

**审查者关注点：**
- 正确性：是否按预期工作？
- 边界情况：错误处理、边界条件
- 性能：是否有性能回归？
- 可维护性：是否可读且测试充分？
- 安全性：是否有潜在漏洞？

---

## 新手友好Issue

项目新人？查找标记为[`good first issue`](https://github.com/OpenSparX/MasterAgent/labels/good%20first%20issue)的Issue：

**建议的入门任务：**
- 添加新的MCP服务（如天气API）
- 改进CLI的错误信息
- 为automotive demo编写示例
- 将文档翻译为其他语言
- 为未覆盖的模块添加单元测试

**不确定从哪开始？** 在Issue下评论"我想做这个"，维护者会指导您。

---

## 社区

- 💬 **讨论区**：[GitHub Discussions](https://github.com/OpenSparX/MasterAgent/discussions) — 提问、分享想法
- 🐛 **Issues**：[GitHub Issues](https://github.com/OpenSparX/MasterAgent/issues) — 报告Bug、请求功能
- 📧 **邮箱**：dev@openschbrid.com — 安全问题、私密咨询

---

## 许可证

贡献即表示您同意您的贡献将以[Apache 2.0许可证](LICENSE)发布。

---

**感谢您让Sparx变得更好！**
