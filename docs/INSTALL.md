# 安装 sparx

## 快速安装

```bash
curl -fsSL https://openschbrid.dev/install.sh | sh
```

装完之后：

```bash
sparx init my-agent && cd my-agent
sparx doctor
```

安装脚本会自动识别平台、校验 SHA256、装到一个**不需要 sudo** 的目录（优先 `~/.local/bin`），然后实际执行一次 `sparx version` 确认二进制能在这台机器上跑起来。如果目标目录不在 `PATH` 上，它会打印那一行需要你自己加的配置——不会偷偷改你的 shell 配置文件。

## 四个渠道

| 渠道 | 命令 | 适合谁 |
|---|---|---|
| 安装脚本 | `curl -fsSL https://openschbrid.dev/install.sh \| sh` | 默认路径，Linux / macOS / WSL 通吃 |
| Homebrew | `brew install OpenSparX/MasterAgent/sparx` | macOS 开发者，`brew upgrade` 自动升级 |
| npm | `npm i -g @sparx/cli` | 已经在 Node 工具链里的人 |
| 直接下载 | [Releases](https://github.com/OpenSparX/MasterAgent/releases) | CI、离线环境、企业内网 |

四个渠道的产物是**同一个二进制**，来自同一次 CI 构建，SHA256 一致。选哪个只影响升级方式。

### 指定版本

```bash
SPARX_VERSION=2.1.0 curl -fsSL https://openschbrid.dev/install.sh | sh
```

### 指定安装目录

```bash
SPARX_INSTALL_DIR=/opt/sparx/bin curl -fsSL https://openschbrid.dev/install.sh | sh
```

### 离线 / 内网安装

```bash
# 在有网的机器上
curl -fsSLO https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-linux-arm64.tar.gz
curl -fsSLO https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-linux-arm64.tar.gz.sha256

# 拷到目标机器后
shasum -a 256 -c sparx-2.1.0-linux-arm64.tar.gz.sha256
tar -xzf sparx-2.1.0-linux-arm64.tar.gz
sudo install -m 755 sparx-2.1.0-linux-arm64/bin/sparx /usr/local/bin/
```

## 支持的平台

| 平台 | 产物 |
|---|---|
| macOS Apple Silicon | `darwin-arm64` |
| macOS Intel | `darwin-x64` |
| Linux x86_64 | `linux-x64` |
| Linux ARM64 | `linux-arm64` |

Windows 请用 WSL。二进制约 350 KB（打包后 44 KB），只依赖系统 libc/libc++，没有第三方动态库。

## 关于 NPU 加速

`sparx` 本身**不包含也不链接**任何 Qualcomm 代码——它在运行时用 `dlopen` 找 `libGenie.so`。这意味着同一个二进制在没有 NPU 的开发机和在 SA8797P 上都能跑，前者自动走 CPU 后端。

NPU 推理需要你自己从 Qualcomm 获取 AI Engine Direct (QNN) runtime，这部分受 Qualcomm AI Stack License 约束，**不可再分发**，所以任何渠道都不会附带它。装好之后用 `sparx doctor` 检查：

```
$ sparx doctor
  ✓ device connected        SM8797 (soc_id 72)
  ✓ libGenie.so             /vendor/lib64/libGenie.so
  ✓ ADSP_LIBRARY_PATH       /vendor/lib/rfsa/adsp
  ✗ context binary          not found — run `sparx pull qwen3-4b`
```

没有 NPU 环境时 `doctor` 会明确告诉你缺什么，而不是静默降级。

## 从源码构建

只在需要改内核代码时才有必要：

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd sparx/v2
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

需要 CMake ≥ 3.16 和支持 C++17 的编译器。源码构建的二进制版本号是 `0.0.0-dev`，故意不是合法的发布版本号——这样 bug 报告里一眼能看出是自编译的还是官方发布的。

## 卸载

```bash
rm -f "$(command -v sparx)"
rm -rf ~/.sparx          # 模型缓存和配置

# Homebrew
brew uninstall sparx
# npm
npm uninstall -g @sparx/cli
```

## 排查

**`sparx: command not found`** — 装好了但不在 `PATH` 上。安装脚本最后打印的那一行就是解法；或者直接跑 `~/.local/bin/sparx`。

**`checksum MISMATCH`** — 不要用这个下载。可能是网络中间层篡改或镜像不同步。换网络重试，或从 Releases 页面手动下载核对。

**npm 装完报 `could not find the sparx binary`** — 平台子包没装上，通常是 `--no-optional` 或者 npm 代理没同步。用 `npm install --force @sparx/cli`，或者直接换安装脚本。

**Apple Silicon 上装成了 x64** — 在 Rosetta 终端里跑的。检查 `uname -m` 应该是 `arm64`；如果是 `x86_64`，退出 Rosetta 终端重装。
