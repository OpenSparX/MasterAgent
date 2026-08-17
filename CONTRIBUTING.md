# Contributing to OAK (Open Agent Kernel)

Thank you for your interest in contributing! OAK is an open-source project
and we welcome contributions from the community.

感谢你对 OAK 项目的关注！我们欢迎社区贡献。

## Quick Links

- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Security Policy](SECURITY.md)
- [Issue Tracker](https://github.com/OpenSparX/MasterAgent/issues)
- [Discussions](https://github.com/OpenSparX/MasterAgent/discussions)

## Getting Started

### Prerequisites

- C++17 compiler (GCC 9+, Clang 11+, MSVC 2019+)
- CMake 3.18+
- Git

### Build from Source

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run Tests

```bash
cmake --build build --target test
# Or directly:
cd build && ctest --output-on-failure
```

## How to Contribute

### Reporting Bugs

1. Search [existing issues](https://github.com/OpenSparX/MasterAgent/issues)
   to avoid duplicates
2. Use the bug report template
3. Include: OS, compiler version, steps to reproduce, expected vs actual behavior

### Suggesting Features

Open a [Discussion](https://github.com/OpenSparX/MasterAgent/discussions/categories/ideas)
with your proposal. Include use cases and potential implementation approach.

### Submitting Code

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/your-feature`
3. Write your code following our style guide (below)
4. Add tests for new functionality
5. Ensure all tests pass: `ctest --output-on-failure`
6. Commit with clear messages (see below)
7. Push and open a Pull Request

### Commit Messages

Format:
```
<type>(<scope>): <short description>

<body: what and why, not how>
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `ci`, `chore`

Examples:
```
feat(scheduler): add priority aging to prevent starvation
fix(mesh): handle mDNS timeout on slow networks
docs: add IoT edge example project
```

## Code Style

### C++ Guidelines

- **Standard**: C++17
- **Naming**: `snake_case` for functions/variables, `PascalCase` for types,
  `UPPER_CASE` for constants
- **Headers**: `#pragma once`, minimal includes, forward-declare when possible
- **Comments**: Doxygen for public API, inline comments for non-obvious logic
- **Safety**: RAII, no raw `new`/`delete`, prefer `std::unique_ptr`/`std::shared_ptr`
- **Threading**: `std::mutex` + `std::lock_guard`, no raw `pthread`

### Project Structure

```
src/                    # Core kernel (master_agent_core library)
  agent_dispatch/       # Agent scheduling and dispatch
  agent_service/        # Agent lifecycle management
  atomic_service/       # Atomic operations and MCP
cli/                    # sparx CLI tool
  include/              # CLI headers
  src/                  # CLI implementation
third_party/            # Vendored dependencies
examples/               # Example projects
docs/                   # Documentation
```

### Do's and Don'ts

✅ Do:
- Write tests for new features
- Keep PRs focused (one feature/fix per PR)
- Update documentation when changing behavior
- Run the full test suite before submitting

❌ Don't:
- Break backward compatibility without discussion
- Add external dependencies without justification
- Commit generated files or build artifacts
- Include platform-specific code without `#ifdef` guards

## ⚠️ Important: Qualcomm Licensed Code

Files under `genai_lib/`, `qnn_model_prepare_*.py`, and `utilities/` are
**Qualcomm AI Stack Licensed** and MUST NOT be included in pull requests
or any public distribution. See [SECURITY.md](SECURITY.md) for details.

If your change touches NPU integration, ensure no Qualcomm-headered files
migrate into the open-source tree.

## Review Process

1. A maintainer will review your PR within 3 business days
2. Address review feedback with fixup commits
3. Once approved, a maintainer will squash-merge

## License

By contributing, you agree that your contributions will be licensed under
the [Apache License 2.0](LICENSE).

---

# 贡献指南

## 快速开始

1. Fork 本仓库
2. 创建特性分支: `git checkout -b feature/your-feature`
3. 编写代码并添加测试
4. 确保所有测试通过
5. 提交 Pull Request

## 代码规范

- C++17 标准
- 函数/变量使用 `snake_case`，类型使用 `PascalCase`
- 公共 API 使用 Doxygen 注释
- 每个 PR 只做一件事

## 注意事项

⚠️ `genai_lib/`、`qnn_model_prepare_*.py`、`utilities/` 目录下的文件为高通授权代码，
**禁止**包含在 PR 或任何公开发布中。
