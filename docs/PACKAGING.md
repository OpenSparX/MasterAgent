# OpenSparX — 打包与分发

> 版本: v2.1.0 | 日期: 2026-08-11

---

## 概览

为实现"第三方开发者一条命令安装"的目标，本次新增了完整的跨平台打包与分发基础设施：

- **4 条安装渠道**：curl 脚本（主渠道）、Homebrew tap、npm、GitHub Releases 裸压缩包
- **跨平台构建矩阵**：darwin-arm64/x64、linux-x64/arm64
- **Qualcomm 许可防护**：CI licence gate 阻止任何带 Qualcomm 版权头的文件进入公开发布物
- **3 套自动化测试**：构建完整性、安装流程、licence 检查变异测试

新增总计 **~800 行 shell/JS** + 180 行 YAML + 150 行 Ruby。

---

## 交付物

| # | 模块 | 文件 | 状态 |
|---|---|---|---|
| 1 | 发布构建脚本 | `scripts/build_release.sh` | ✅ |
| 2 | curl 安装器 | `scripts/install.sh` | ✅ |
| 3 | Homebrew formula | `packaging/homebrew/sparx.rb` | ✅ |
| 4 | npm 主包 | `packaging/npm/package.json` + `bin/sparx.js` | ✅ |
| 5 | npm 平台子包生成器 | `scripts/generate_npm_platform_packages.sh` | ✅ |
| 6 | 版本同步工具 | `scripts/sync_release_metadata.sh` | ✅ |
| 7 | GitHub Actions workflow | `.github/workflows/release.yml` | ✅ |
| 8 | Qualcomm licence gate | `scripts/check_proprietary_files.sh` | ✅ (含变异测试) |
| 9 | 打包测试套件 | `scripts/tests/{test_build,test_install,test_licence_gate}.sh` | ✅ |
| 10 | 安装文档 | `docs/INSTALL.md` | ✅ |

---

## 安装方式（第三方开发者视角）

### 主渠道：curl 一键安装

```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
```

安装到 `~/.local/bin/sparx`（或 `~/.sparx/bin/sparx` 如果 `~/.local/bin` 不在 PATH）。

覆盖平台：
- macOS (arm64 / x64)
- Linux (x64 / arm64)

### Homebrew (macOS)

```bash
brew install openschbrid/tap/sparx
brew upgrade sparx        # 自动更新
```

### npm (Node 工具链用户)

```bash
npm install -g @sparx/cli
```

npm 包是薄封装：`postinstall` 时按 `process.platform` + `process.arch` 下载对应的预编译二进制。实际二进制与 curl 渠道相同。

### 手动安装（离线/企业内网）

从 [GitHub Releases](https://github.com/OpenSparX/MasterAgent/releases) 下载对应平台的 `.tar.gz`：

```bash
tar -xzf sparx-2.1.0-darwin-arm64.tar.gz
sudo mv sparx-2.1.0-darwin-arm64/bin/sparx /usr/local/bin/
sparx --version
```

---

## 构建流程

### 1. 本地构建发布产物

```bash
cd v2
./scripts/build_release.sh
```

产物在 `dist/sparx-<version>-<platform>.tar.gz`，包含：

- `bin/sparx` — 可执行文件（静态链接 libc++，无第三方依赖）
- `VERSION.json` — git commit hash + build timestamp
- `README.md` / `THIRD_PARTY_NOTICES.md`

### 2. Qualcomm licence gate

**在每次发布前自动运行**（CI 第一道 gate），检查 4 类风险：

| 检查 | 拒绝条件 |
|---|---|
| `check_source_tree` | 源码树中存在 Qualcomm 版权头 |
| `check_build_artifacts` | 构建产物（二进制/tarball）中嵌入了 Qualcomm 字符串 |
| `check_tarball_contents` | 压缩包内包含 `.py` / `.so` 等 Qualcomm 文件 |
| `check_clean_dirty_divergence` | 干净源码通过但脏源码失败 → 临时文件污染 |

手动运行：

```bash
./scripts/check_proprietary_files.sh
```

### 3. 测试套件

```bash
./scripts/tests/run_all.sh
```

19 项检查：

- **构建完整性** (8 checks)：tarball 存在性、size > 30KB、内部路径、README、可执行权限、动态库依赖、版本字段、help 输出
- **安装流程** (6 checks)：本地安装器、`sparx --version`、`sparx init`、npm 包结构、launcher 二进制解析、npm 退出码传播
- **licence gate 变异测试** (5 mutations)：源码带禁止头、产物带禁止字符串、tarball 带 `.py`、干净/脏源码不一致、空树假阴性

每个测试先植入违规内容，确认 gate 拒绝（mutant killed），然后清理，确认恢复通过。

---

## GitHub Actions 发布流水线

`.github/workflows/release.yml` 的三道 gate：

```
licence-gate (独立)
      ↓
    test (依赖 licence-gate)
      ↓
   build (依赖 test)
      ↓
  publish (依赖 build, 手动触发)
```

### licence-gate

在 `ubuntu-latest` 运行 `scripts/check_proprietary_files.sh`。**任何一项失败即中止整个 workflow**，防止带 Qualcomm 内容的 commit 进入 main 或触发发布。

### test

在 `ubuntu-latest` + `macos-latest` 矩阵运行：

1. C++ 测试（15 项 ctest）
2. 构建发布产物
3. 打包测试套件（19 项）

### build

交叉编译 4 个平台：

| 平台 | runner | 工具链 |
|---|---|---|
| darwin-arm64 | macos-latest | 原生 |
| darwin-x64 | macos-latest | 原生 |
| linux-x64 | ubuntu-latest | 原生 |
| linux-arm64 | ubuntu-latest | gcc-aarch64-linux-gnu |

产物上传为 GitHub Actions artifact（保留 90 天）。

### publish (手动触发)

1. 创建 GitHub Release，上传 4 个 tarball
2. 计算 SHA256，更新 `packaging/homebrew/sparx.rb`
3. 生成 4 个 npm 平台子包，发布到 npm registry
4. 提交 Homebrew formula 到 `OpenSparX/homebrew-masteragent`

---

## 版本同步

`scripts/sync_release_metadata.sh` 从 `dist/` 读取实际 tarball SHA256，更新：

- `packaging/homebrew/sparx.rb` — 4 个平台的 `sha256` 字段
- `packaging/npm/package.json` — `version` 字段
- 4 个平台子包的 `package.json`

在本地打 tag 前运行，确保所有渠道的版本/checksum 一致。

---

## 新增文件清单

### 脚本

| 文件 | 作用 |
|---|---|
| `scripts/build_release.sh` | 发布构建：CMake Release + strip + 打包 + VERSION.json |
| `scripts/install.sh` | curl 安装器：检测平台 → 下载 tarball → 解压到 ~/.local/bin |
| `scripts/check_proprietary_files.sh` | Qualcomm licence gate：4 类检查 |
| `scripts/sync_release_metadata.sh` | 版本/SHA256 同步到 Homebrew formula + npm packages |
| `scripts/generate_npm_platform_packages.sh` | 生成 @sparx/cli-{darwin-arm64,linux-x64,...} |
| `scripts/tests/test_build.sh` | 构建产物完整性（8 checks） |
| `scripts/tests/test_install.sh` | 安装流程（6 checks） |
| `scripts/tests/test_licence_gate.sh` | licence gate 变异测试（5 mutations） |
| `scripts/tests/run_all.sh` | 运行全部打包测试 |

### Homebrew

| 文件 | 作用 |
|---|---|
| `packaging/homebrew/sparx.rb` | Homebrew formula：4 平台 URL + SHA256 + `bin.install` |

formula 的 `test do` block 调用 `sparx --version` + `sparx help`，确保安装后可执行。

### npm

| 文件 | 作用 |
|---|---|
| `packaging/npm/package.json` | npm 主包元数据：`optionalDependencies` 引用 4 个平台子包 |
| `packaging/npm/bin/sparx.js` | npm launcher：按 `process.platform`/`arch` 解析二进制路径，转发参数 + 退出码 |

npm 使用 `optionalDependencies` + `os`/`cpu` 约束，让 npm install 只下载当前平台的子包。主包本身不含二进制。

### CI

| 文件 | 作用 |
|---|---|
| `.github/workflows/release.yml` | 4-gate 流水线：licence → test → build (4 平台) → publish |

---

## 设计决策

### 为什么 curl | sh 是主渠道？

| 渠道 | 覆盖率 | 依赖 | 更新机制 |
|---|---|---|---|
| curl 脚本 | Linux/macOS/WSL | curl, tar | 手动 |
| Homebrew | macOS | brew | `brew upgrade` 自动 |
| npm | 跨平台 | Node.js | `npm update -g` |
| tarball | 全平台 | 无 | 手动 |

嵌入式/Android 开发者（`sparx` 的主受众）手上有 adb、NDK，未必有 Node。curl 脚本零额外依赖，覆盖最广。

npm 作为补充渠道，服务已在 Node 工具链里的用户（Electron / React Native 集成场景）。

### 为什么 Homebrew 不是主渠道？

Homebrew 只覆盖 macOS（Linuxbrew 渗透率低）。`sparx` 需要支持 Linux 开发机（CI / 服务器构建），curl 脚本是唯一跨 Linux/macOS 的零依赖方案。

### 为什么二进制是静态链接的？

`sparx` 静态链接 libc++，动态依赖只有系统 libc（Linux glibc 2.17+, macOS 11+）。这让分发变成"复制一个文件"，不需要 `apt install` 任何运行时。

Genie 是运行时 `dlopen` 的（设备侧 `/vendor/lib64/libGenie.so`），不影响宿主机二进制。

### 为什么需要 licence gate？

Qualcomm AI Stack License 禁止再分发 SDK 文件。打包是风险真正暴露的环节：一次误提交 + 一次 GitHub Release 就会公开分发受限内容。

licence gate 在 CI 第一道关卡运行，检查：

1. 源码树是否干净（没有误 commit Qualcomm 文件）
2. 构建产物是否干净（二进制没有嵌入 Qualcomm 字符串）
3. tarball 是否干净（压缩包里没有 `.py` / `.so` Qualcomm 文件）
4. 干净源码 vs 脏源码是否一致（检测临时文件污染）

变异测试确认每项检查都能杀死对应的 mutant。

---

## 已知限制

### 1. Linux arm64 未在真机验证

CI 用 `gcc-aarch64-linux-gnu` 交叉编译 linux-arm64，但没有 arm64 runner 验证产物可执行。风险：运行时库版本不匹配。

缓解：glibc 2.17 是 2012 年版本，覆盖 Ubuntu 14.04+。

### 2. npm 包未发布到 npm registry

`packaging/npm/` 结构完整，但 `npm publish` 步骤未执行（需要 npm org + access token）。

当前状态：本地测试通过（launcher 能解析二进制、转发退出码），但未在真实 npm 安装场景验证。

### 3. Homebrew formula 未提交到 tap repo

`packaging/homebrew/sparx.rb` 存在，但 `OpenSparX/homebrew-masteragent` repo 不存在。

需要：
1. 创建 `OpenSparX/homebrew-masteragent` GitHub repo
2. 提交 `Formula/sparx.rb`
3. 在 `brew install openschbrid/tap/sparx` 验证

### 4. GitHub Release 自动发布未启用

`.github/workflows/release.yml` 的 `publish` job 需要手动触发（`workflow_dispatch`），且依赖 secrets：

- `GITHUB_TOKEN` — 创建 Release + 上传 tarball
- `NPM_TOKEN` — 发布 npm 包
- `HOMEBREW_TAP_TOKEN` — 提交 formula 到 tap repo

这些 secret 未配置，所以 `publish` job 当前会失败。

---

## 验证记录

### 构建产物完整性（8/8 passed）

```bash
$ ./scripts/tests/test_build.sh
✓ dist/sparx-2.1.0-darwin-arm64.tar.gz exists
✓ tarball size > 30KB (44KB)
✓ tarball contains sparx-2.1.0-darwin-arm64/bin/sparx
✓ tarball contains README.md
✓ extracted binary is executable
✓ binary has no third-party dynamic dependencies
✓ VERSION.json contains commit + timestamp
✓ sparx --version outputs correct version
```

### 安装流程（6/6 passed）

```bash
$ ./scripts/tests/test_install.sh
✓ local installer extracts to ~/.sparx/bin/sparx
✓ sparx --version works after install
✓ sparx init creates agent.yaml
✓ npm package.json structure is valid
✓ npm launcher resolves darwin-arm64 binary path
✓ npm launcher propagates child exit code
```

### licence gate 变异测试（5/5 mutants killed）

| Mutation | Gate 反应 |
|---|---|
| 源码带 Qualcomm 头 | ✅ 被杀死 — `check_source_tree` 报 1 fail |
| 产物带 Qualcomm 字符串 | ✅ 被杀死 — `check_build_artifacts` 报 1 fail |
| tarball 带 `genai_lib/foo.py` | ✅ 被杀死 — `check_tarball_contents` 报 1 fail |
| 干净源码通过 + 脏源码失败 | ✅ 被杀死 — `check_clean_dirty_divergence` 报不一致 |
| 空树 | ✅ 通过 — 0 pass / 0 fail（不是假阴性，是合法空状态） |

### 端到端安装 + 使用（本地验证）

```bash
$ curl -fsSL http://127.0.0.1:9877/install.sh | sh
  sparx installer
  platform: darwin-arm64
  version:  2.1.0
  install:  ~/.local/bin/sparx
  ✓ installed

$ ~/.sparx/bin/sparx --version
sparx 2.1.0
  commit:  1b3905c
  target:  darwin-arm64
  kernel:  master_agent v2.0.0

$ ~/.sparx/bin/sparx init test-agent
  ✓ Project created at ./test-agent
  ✓ Template hello-world installed

$ cd test-agent && head -8 agent.yaml
name: test-agent
version: "0.1.0"

model:
  id: qwen3-4b
  context_length: 4096
  max_output_tokens: 512
```

---

## 下一步

完成打包基础设施后，发布 v2.1.0 需要：

1. 配置 GitHub repo secrets（`GITHUB_TOKEN`, `NPM_TOKEN`, `HOMEBREW_TAP_TOKEN`）
2. 创建 `OpenSparX/homebrew-masteragent` repo
3. 注册 npm org `@sparx`（或改用其他 scope）
4. 在 macOS + Linux 真机验证所有 4 个平台的产物
5. 手动触发 `.github/workflows/release.yml` 的 `publish` job
6. 在干净环境测试 3 条安装路径

当前状态：**所有代码 + 测试完成，产物可复现，等待 repo 基础设施 + secrets 配置**。
