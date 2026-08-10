# GitHub 仓库配置指南

## 第一步：创建仓库

### 1.1 创建主仓库

```bash
# 使用 gh CLI（推荐）
gh repo create OpenSparX/MasterAgent --public \
  --description "On-device Agent framework for Qualcomm and ARM platforms" \
  --homepage "https://github.com/OpenSparX/MasterAgent"

# 或者在 GitHub 网页上手动创建
# https://github.com/new
# Repository name: sparx
# Description: On-device Agent framework for Qualcomm and ARM platforms
# Public
# 不要勾选 "Add a README file"（我们已经有了）
```

### 1.2 创建 Homebrew tap 仓库

```bash
# 使用 gh CLI
gh repo create OpenSparX/homebrew-masteragent --public \
  --description "Homebrew formulae for sparx"

# 初始化仓库结构
cd /tmp
git clone https://github.com/OpenSparX/homebrew-masteragent.git
cd homebrew-tap
mkdir -p Formula
cat > README.md << 'TAPREADME'
# Homebrew Tap for sparx

Install sparx via Homebrew:

```bash
brew install openschbrid/tap/sparx
```

## Available Formulae

- [sparx](Formula/sparx.rb) — On-device Agent framework for Qualcomm and ARM platforms
TAPREADME

git add .
git commit -m "Initial commit"
git push origin main
```

---

## 第二步：配置 GitHub Secrets

在主仓库（OpenSparX/MasterAgent）配置以下 secrets：

### 2.1 进入 Secrets 配置页面

1. 打开 https://github.com/OpenSparX/MasterAgent/settings/secrets/actions
2. 或者：仓库页面 → Settings → Secrets and variables → Actions → Secrets

### 2.2 添加 NPM_TOKEN

**作用：** 允许 CI 发布 npm 包到 npmjs.com

**获取步骤：**

```bash
# 1. 登录 npm
npm login

# 2. 生成 Automation token
# 打开 https://www.npmjs.com/settings/<your-username>/tokens
# 点击 "Generate New Token" → "Automation"
# Token 名称：github-actions-sparx
# 复制生成的 token（格式：npm_xxxxxxxxxxxxxxxxxxxxxx）
```

**添加到 GitHub：**
- Name: `NPM_TOKEN`
- Secret: `npm_xxxxxxxxxxxxxxxxxxxxxx`（你刚才复制的 token）

### 2.3 添加 HOMEBREW_TAP_TOKEN

**作用：** 允许 CI 推送 formula 到 homebrew-tap 仓库

**获取步骤：**

```bash
# 1. 生成 GitHub Personal Access Token (PAT)
# 打开 https://github.com/settings/tokens/new
# Note: sparx-homebrew-tap-ci
# Expiration: No expiration（或选择 1 年，到期前记得续期）
# Select scopes:
#   ☑ repo (所有子项)
# 点击 "Generate token"
# 复制生成的 token（格式：ghp_xxxxxxxxxxxxxxxxxxxx）
```

**添加到 GitHub：**
- Name: `HOMEBREW_TAP_TOKEN`
- Secret: `ghp_xxxxxxxxxxxxxxxxxxxx`（你刚才复制的 PAT）

### 2.4 GITHUB_TOKEN（无需手动配置）

这个 token 由 GitHub Actions 自动提供，用于创建 Release 和上传 artifacts。

**验证：** 确保仓库的 Actions 权限正确（下一步）。

---

## 第三步：配置 GitHub Actions 权限

### 3.1 进入 Actions 设置页面

1. 打开 https://github.com/OpenSparX/MasterAgent/settings/actions
2. 或者：仓库页面 → Settings → Actions → General

### 3.2 配置 Workflow permissions

找到 **Workflow permissions** 部分，选择：

```
☑ Read and write permissions
☑ Allow GitHub Actions to create and approve pull requests
```

点击 **Save** 保存。

**为什么需要这个？**
- Read and write：允许 workflow 创建 Release、上传 artifacts、推送到 tap 仓库
- Create PRs：（可选）未来如果需要自动化 PR 流程

---

## 第四步：推送代码到 GitHub

### 4.1 初始化 Git 仓库（如果还没有）

```bash
cd /Users/hzp/Downloads/MasterAgentV2/MasterAgent-SA8397/v2

# 检查是否已经是 git 仓库
if [ -d .git ]; then
    echo "Already a git repository"
else
    git init
    echo "Initialized git repository"
fi
```

### 4.2 配置 .gitignore

```bash
cat > .gitignore << 'GITIGNORE'
# Build outputs
build/
build-*/
dist/
*.tar.gz
*.sha256

# IDE
.vscode/
.idea/
*.swp
*.swo
*~

# macOS
.DS_Store

# Temporary files
*.log
.cache/

# npm
node_modules/
package-lock.json

# Qualcomm proprietary (should never be committed)
genai_lib/
utilities/
qnn_model_prepare_*.py
libGenie*.so
libQnn*.so
*.serialized.bin
GITIGNORE
```

### 4.3 提交所有代码

```bash
git add .
git status  # 检查要提交的文件

# 确保没有 Qualcomm 相关文件被 add（licence gate 会在 CI 再次检查）
git status | grep -E "genai_lib|utilities|qnn_model_prepare"
# 如果有输出，立即 git reset 这些文件

git commit -m "Initial commit: sparx v2.1.0

- Complete CLI implementation (init, pull, run, deploy, shell, devices, demo)
- Multi-runtime support (llama.cpp, Genie NPU)
- 4-channel distribution (curl, Homebrew, npm, manual)
- Cross-platform build (darwin-arm64/x64, linux-x64/arm64)
- Qualcomm licence gate with mutation testing
- 19 automated tests (build integrity, install flow, licence gate)
- Full documentation (English + Chinese)
"
```

### 4.4 推送到 GitHub

```bash
# 添加 remote
git remote add origin https://github.com/OpenSparX/MasterAgent.git

# 推送到 main
git branch -M main
git push -u origin main
```

**验证：** 打开 https://github.com/OpenSparX/MasterAgent 确认代码已上传。

---

## 第五步：触发第一次 Release

### 方式 A：通过 Git Tag 触发（推荐）

```bash
# 1. 创建 tag
git tag -a v2.1.0 -m "Release v2.1.0

First public release of sparx.

Features:
- On-device Agent framework for Qualcomm and ARM platforms
- Multi-runtime support (llama.cpp CPU + Genie NPU)
- 4 installation channels (curl, Homebrew, npm, manual)
- Cross-platform (macOS, Linux, Android devices)
- Full CLI: init, pull, run, deploy, shell, devices, demo
"

# 2. 推送 tag
git push origin v2.1.0

# 3. GitHub Actions 会自动触发 release workflow
```

### 方式 B：通过 GitHub UI 手动触发

1. 打开 https://github.com/OpenSparX/MasterAgent/actions/workflows/release.yml
2. 点击 **Run workflow** 按钮（右上角）
3. 选择 branch: `main`
4. 输入 version: `2.1.0`（不带 `v` 前缀）
5. 点击 **Run workflow**

### 方式 C：通过 gh CLI 触发

```bash
gh workflow run release.yml \
  --repo OpenSparX/MasterAgent \
  --ref main \
  --field version=2.1.0
```

---

## 第六步：监控 Release 流程

### 6.1 查看 Workflow 执行

```bash
# 命令行查看
gh run list --repo OpenSparX/MasterAgent --workflow=release.yml

# 或者打开网页
open https://github.com/OpenSparX/MasterAgent/actions
```

### 6.2 Workflow 流程（预计 15-20 分钟）

```
[1/4] licence-gate (1 分钟)
  ✓ 检查源码树无 Qualcomm 文件
  ✓ 检查已构建产物无 Qualcomm 字符串
  ↓ 如果失败，整个 workflow 中止

[2/4] test (3-5 分钟，ubuntu + macos 并行)
  ✓ 运行 C++ 测试（15 项）
  ✓ 构建 release artifacts
  ✓ 运行打包测试（19 项）
  ↓ 如果失败，不会进入 build

[3/4] build (5-8 分钟，4 平台并行)
  ✓ darwin-arm64 (macos-14)
  ✓ darwin-x64 (macos-13)
  ✓ linux-x64 (ubuntu-22.04)
  ✓ linux-arm64 (ubuntu-22.04, 交叉编译)
  ↓ 上传 artifacts

[4/4] publish (2-3 分钟)
  ✓ 下载所有 artifacts
  ✓ 生成 SHA256SUMS
  ✓ 创建 GitHub Release
  ✓ 上传 4 个 .tar.gz + SHA256SUMS
  ✓ 更新 Homebrew formula
  ✓ 发布 npm 包
```

### 6.3 验证 Release 成功

**GitHub Release：**
```bash
# 查看 Release
gh release view v2.1.0 --repo OpenSparX/MasterAgent

# 或者打开网页
open https://github.com/OpenSparX/MasterAgent/releases/tag/v2.1.0
```

**Homebrew tap：**
```bash
# 检查 formula 是否更新
curl -fsSL https://raw.githubusercontent.com/OpenSparX/homebrew-masteragent/main/Formula/sparx.rb | head -20
```

**npm：**
```bash
# 检查包是否发布
npm view @sparx/cli
```

---

## 第七步：验证安装流程

### 7.1 测试 curl 安装

```bash
# 在干净环境测试
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh

# 验证
sparx --version
sparx init test-project
cd test-project && ls
```

### 7.2 测试 Homebrew 安装

```bash
# macOS
brew install openschbrid/tap/sparx
sparx --version

# 测试升级机制
brew upgrade sparx
```

### 7.3 测试 npm 安装

```bash
# 任何平台（需要 Node.js）
npm install -g @sparx/cli
sparx --version

# 测试升级
npm update -g @sparx/cli
```

### 7.4 测试手动 tarball 安装

```bash
# 下载 tarball
VERSION=2.1.0
PLATFORM=darwin-arm64  # 或 darwin-x64, linux-x64, linux-arm64
curl -LO https://github.com/OpenSparX/MasterAgent/releases/download/v${VERSION}/sparx-${VERSION}-${PLATFORM}.tar.gz

# 验证 SHA256
curl -LO https://github.com/OpenSparX/MasterAgent/releases/download/v${VERSION}/SHA256SUMS
shasum -a 256 -c SHA256SUMS 2>&1 | grep sparx-${VERSION}-${PLATFORM}

# 解压并安装
tar -xzf sparx-${VERSION}-${PLATFORM}.tar.gz
sudo mv sparx-${VERSION}-${PLATFORM}/bin/sparx /usr/local/bin/
sparx --version
```

---

## 常见问题

### Q1: licence-gate 失败，提示 "Qualcomm header detected"

**原因：** 源码树中有 Qualcomm 版权文件。

**解决：**
```bash
# 查找所有 Qualcomm 文件
grep -r "Qualcomm Technologies, Inc." --include="*.py" --include="*.cpp" --include="*.h" .

# 删除或 .gitignore 它们
git rm <offending-file>
git commit -m "Remove Qualcomm proprietary files"
git push
```

### Q2: 交叉编译 linux-arm64 失败

**原因：** 缺少交叉编译工具链。

**解决：** 已在 workflow 中安装 `gcc-aarch64-linux-gnu`，如果仍失败检查 CMake 配置。

### Q3: npm publish 失败，提示 403 Forbidden

**原因：** NPM_TOKEN 过期或权限不足。

**解决：**
```bash
# 重新生成 token
# https://www.npmjs.com/settings/<your-username>/tokens
# 选择 "Automation" token
# 更新 GitHub secret NPM_TOKEN
```

### Q4: Homebrew formula SHA256 不匹配

**原因：** tarball 在构建后被修改，或 `sync_release_metadata.sh` 未运行。

**解决：**
```bash
# 本地重新生成 checksums
./scripts/build_release.sh
./scripts/sync_release_metadata.sh

# 提交更新
git add packaging/homebrew/sparx.rb
git commit -m "Update Homebrew formula checksums"
git push
```

### Q5: 如何回滚一个有问题的 release？

```bash
# 1. 删除 GitHub Release
gh release delete v2.1.0 --yes --repo OpenSparX/MasterAgent

# 2. 删除 tag
git tag -d v2.1.0
git push origin :refs/tags/v2.1.0

# 3. 取消发布 npm 包（72 小时内有效）
npm unpublish @sparx/cli@2.1.0

# 4. 回滚 Homebrew formula
cd /path/to/homebrew-tap
git revert <commit-sha>
git push origin main
```

详见 `.github/RELEASE_CHECKLIST.md` 的 Rollback 章节。

---

## 下一步

### 发布后的工作

- [ ] 在 README.md 添加 Installation 章节
- [ ] 在 GitHub repo 添加 Topics: `agent`, `qualcomm`, `npu`, `android`, `embedded`
- [ ] 设置 GitHub repo description 和 website
- [ ] 创建 GitHub Discussions（Q&A 和公告）
- [ ] 在 Twitter / Reddit / HN 宣布发布
- [ ] 监控 GitHub Issues（用户反馈）
- [ ] 准备 v2.2.0 roadmap

### 可选改进

- [ ] 添加 `sparx upgrade` 命令（自动检测新版本）
- [ ] 添加 Windows 原生支持（当前需要 WSL）
- [ ] 设置 docs 网站（GitHub Pages 或 docs.openschbrid.dev）
- [ ] 添加 telemetry（匿名使用统计，opt-in）
- [ ] 生成 CHANGELOG.md（从 git log）

---

## 参考文档

- `.github/RELEASE_CHECKLIST.md` — 完整 release 流程
- `README_DISTRIBUTION.md` — 打包架构决策
- `DISTRIBUTION_MANIFEST.md` — 文件清单
- `docs/PACKAGING.md` — 打包内部实现（中文）
- `docs/INSTALL.md` — 用户安装指南（英文）
- `TODO.md` — 待办事项清单
