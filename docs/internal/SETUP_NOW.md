# 立即配置 GitHub 仓库

✅ 仓库已创建：
- 主仓库：https://github.com/OpenSparX/MasterAgent.git
- Homebrew tap：https://github.com/OpenSparX/homebrew-masteragent.git

---

## 第一步：初始化 Homebrew Tap 仓库

```bash
cd /tmp
git clone https://github.com/OpenSparX/homebrew-masteragent.git
cd homebrew-masteragent

mkdir -p Formula

cat > README.md << 'TAPREADME'
# Homebrew Tap for sparx

Install sparx via Homebrew:

```bash
brew install OpenSparX/masteragent/sparx
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

访问：https://github.com/OpenSparX/MasterAgent/settings/secrets/actions

### 添加 NPM_TOKEN

1. 打开 https://www.npmjs.com/settings/YOUR_USERNAME/tokens
2. 点击 "Generate New Token" → "Automation"
3. Token 名称：`github-actions-sparx`
4. 复制生成的 token（`npm_xxxxxxxxxxxxxxxxxxxxxx`）
5. 在 GitHub 添加 secret：
   - Name: `NPM_TOKEN`
   - Secret: 粘贴刚才复制的 token

### 添加 HOMEBREW_TAP_TOKEN

1. 打开 https://github.com/settings/tokens/new
2. Note: `sparx-homebrew-masteragent-ci`
3. Expiration: No expiration（或 1 年）
4. Select scopes: ☑ **repo** (全选所有子项)
5. 点击 "Generate token"
6. 复制生成的 token（`ghp_xxxxxxxxxxxxxxxxxxxx`）
7. 在 GitHub 添加 secret：
   - Name: `HOMEBREW_TAP_TOKEN`
   - Secret: 粘贴刚才复制的 PAT

---

## 第三步：配置 Actions 权限

访问：https://github.com/OpenSparX/MasterAgent/settings/actions

找到 **Workflow permissions**，选择：
- ☑ **Read and write permissions**
- ☑ **Allow GitHub Actions to create and approve pull requests**

点击 **Save**

---

## 第四步：推送代码

```bash
cd /Users/hzp/Downloads/MasterAgentV2/MasterAgent-SA8397/v2

# 检查是否已经初始化 git
if [ -d .git ]; then
    echo "Git repository exists, updating remote..."
    git remote remove origin 2>/dev/null || true
else
    echo "Initializing git repository..."
    git init
fi

# 添加 remote
git remote add origin https://github.com/OpenSparX/MasterAgent.git

# 添加所有文件
git add .

# 提交
git commit -m "Initial commit: sparx v2.1.0

- Complete CLI implementation (init, pull, run, deploy, shell, devices, demo)
- Multi-runtime support (llama.cpp CPU + Genie NPU)
- 4-channel distribution (curl, Homebrew, npm, manual)
- Cross-platform build (darwin-arm64/x64, linux-x64/arm64)
- Qualcomm licence gate with mutation testing
- 19 automated tests (build integrity, install flow, licence gate)
- Full documentation (English + Chinese)
"

# 推送到 main
git branch -M main
git push -u origin main
```

---

## 第五步：发布 v2.1.0

### 方式 A：通过 Git Tag 触发（推荐）

```bash
cd /Users/hzp/Downloads/MasterAgentV2/MasterAgent-SA8397/v2

git tag -a v2.1.0 -m "Release v2.1.0

First public release of sparx.

Features:
- On-device Agent framework for Qualcomm and ARM platforms
- Multi-runtime support (llama.cpp CPU + Genie NPU)
- 4 installation channels (curl, Homebrew, npm, manual)
- Cross-platform (macOS, Linux, Android devices)
- Full CLI: init, pull, run, deploy, shell, devices, demo
"

git push origin v2.1.0
```

GitHub Actions 会自动触发 release workflow。

### 方式 B：GitHub UI 手动触发

1. 打开 https://github.com/OpenSparX/MasterAgent/actions/workflows/release.yml
2. 点击 **Run workflow**
3. Branch: `main`
4. Version: `2.1.0`
5. 点击 **Run workflow**

---

## 监控 Release

### 查看 Workflow 执行

```bash
gh run list --repo OpenSparX/MasterAgent --workflow=release.yml
```

或访问：https://github.com/OpenSparX/MasterAgent/actions

### Workflow 流程（约 15-20 分钟）

```
[1/4] licence-gate (~1 min)
  检查源码树、产物、tarballs 无 Qualcomm 文件
  
[2/4] test (~5 min, ubuntu + macos 并行)
  C++ 测试 + 打包测试 (19 checks)
  
[3/4] build (~8 min, 4 平台并行)
  darwin-arm64, darwin-x64, linux-x64, linux-arm64
  
[4/4] publish (~3 min)
  创建 GitHub Release
  更新 Homebrew formula
  发布 npm 包
```

---

## 验证 Release

### GitHub Release
```bash
gh release view v2.1.0 --repo OpenSparX/MasterAgent
```

或访问：https://github.com/OpenSparX/MasterAgent/releases/tag/v2.1.0

### Homebrew tap
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/homebrew-masteragent/main/Formula/sparx.rb | head -20
```

### npm
```bash
npm view @sparx/cli
```

---

## 测试安装

### curl 脚本
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version
```

### Homebrew (macOS)
```bash
brew install OpenSparX/masteragent/sparx
sparx --version
```

### npm
```bash
npm install -g @sparx/cli
sparx --version
```

---

## npm 组织说明

如果 `@sparx` 已被占用，需要修改 `packaging/npm/package.json`：

```json
{
  "name": "@opensparx/cli",  // 或 @masteragent/cli
  ...
}
```

然后重新提交并打 tag。

---

## 故障排查

### licence-gate 失败

检查是否有 Qualcomm 文件：
```bash
grep -r "Qualcomm Technologies" --include="*.py" --include="*.cpp" .
```

### npm publish 失败

检查 NPM_TOKEN 是否正确配置：
```bash
gh secret list --repo OpenSparX/MasterAgent
```

### 查看详细日志

```bash
gh run view --repo OpenSparX/MasterAgent --log
```

---

## 当前状态

✅ 主仓库已创建：https://github.com/OpenSparX/MasterAgent.git
✅ Homebrew tap 已创建：https://github.com/OpenSparX/homebrew-masteragent.git
✅ 所有代码就绪，可以立即推送和发布

**下一步：**
1. 初始化 Homebrew tap 仓库（见第一步）
2. 配置 GitHub Secrets（见第二步）
3. 启用 Actions 权限（见第三步）
4. 推送代码（见第四步）
5. 打 tag 触发发布（见第五步）
