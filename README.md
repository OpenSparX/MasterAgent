# MasterAgent-v2 / sparx

## 安装

```bash
curl -fsSL https://openschbrid.dev/install.sh | sh
```

```bash
sparx init my-agent && cd my-agent
sparx doctor
```

其他渠道：`brew install openschbrid/sparx/sparx`、`npm i -g @sparx/cli`，或从
[Releases](https://github.com/openschbrid/sparx/releases) 直接下载。完整说明和
离线安装见 [docs/INSTALL.md](docs/INSTALL.md)。

NPU 加速需要自行获取 Qualcomm QNN runtime（受 Qualcomm AI Stack License 约束，
不可再分发，因此任何渠道都不附带）。缺失时自动走 CPU 后端，`sparx doctor`
会说明缺什么。

---

MasterAgent-v2 是面向车载智能交互的 C++17 参考实现。交付包覆盖文本接入、预处理、短期记忆、确定性技能、Prompt、最多两阶段的 Mock 推理、任务编排、MCP 原子服务、Agent 调度、结果返回、日志和异常治理。

默认部署为单进程。真实模型、车辆服务和具体业务子 Agent 通过边界接口接入，本包使用确定性 Mock 实现验证流程。

## 目录

- `app/`：默认单进程入口。
- `config/`：Prompt 模板和纯文本 Skill 库；构建和安装时一并部署。
- `include/master_agent/`：公共 C++ 接口。
- `src/`：按模块和职责拆分的实现；`transport/ipc` 为默认关闭的可选适配。
- `tests/`：模块、契约、持久化、故障和端到端测试。
- `third_party/`：随包提供的构建依赖。
- `scripts/`：Android arm64 构建与可选部署脚本。
- `docs/`：架构、接口、运行、测试和交付边界。

## 快速构建

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DMASTER_AGENT_BUILD_APP=ON `
  -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

运行 Mock 全流程：

```powershell
.\build\master_agent.exe `
  --runtime=.\runtime `
  "请把空调切换到内循环"
```

## 公共标识

- 软件版本：`2.0.0`
- C++ 命名空间：`master_agent`
- CMake 目标：`MasterAgent::Core`
- 默认拓扑：单进程
- 模型后端：Mock
- IPC：可选，默认关闭

从 [系统概述](docs/01_系统概述.md) 开始阅读。源码职责和阅读顺序见 [源码导航](docs/12_源码导航.md)，构建和验证方法见 [构建、运行与测试](docs/10_构建运行与测试.md)，平台限制见 [交付范围与限制](docs/11_交付范围与限制.md)。
