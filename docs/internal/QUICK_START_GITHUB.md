# GitHub 配置快速参考

## 🚀 5 分钟快速配置

### 1️⃣ 创建仓库
```bash
gh repo create OpenSparX/MasterAgent --public
gh repo create OpenSparX/homebrew-masteragent --public
```

### 2️⃣ 配置 Secrets
访问：https://github.com/OpenSparX/MasterAgent/settings/secrets/actions

添加两个 secrets：
- `NPM_TOKEN` — 从 https://www.npmjs.com/settings/tokens 生成 Automation token
- `HOMEBREW_TAP_TOKEN` — 从 https://github.com/settings/tokens/new 生成 PAT (scope: repo)

### 3️⃣ 配置 Actions 权限
访问：https://github.com/OpenSparX/MasterAgent/settings/actions

选择：
- ☑ Read and write permissions
- ☑ Allow GitHub Actions to create and approve pull requests

### 4️⃣ 推送代码
```bash
cd /Users/hzp/Downloads/MasterAgentV2/MasterAgent-SA8397/v2
git init
git add .
git commit -m "Initial commit: sparx v2.1.0"
git remote add origin https://github.com/OpenSparX/MasterAgent.git
git push -u origin main
```

### 5️⃣ 发布
```bash
git tag -a v2.1.0 -m "Release v2.1.0"
git push origin v2.1.0
```

GitHub Actions 会自动构建并发布到：
- ✅ GitHub Releases
- ✅ Homebrew tap
- ✅ npm registry

---

## 📋 完整文档

详细步骤见：`docs/GITHUB_SETUP.md`

关键文档：
- `README_DISTRIBUTION.md` — 打包概览
- `.github/RELEASE_CHECKLIST.md` — Release 流程
- `DISTRIBUTION_MANIFEST.md` — 文件清单
- `TODO.md` — 待办事项

---

## ⚠️ 注意事项

### Qualcomm 文件检查
推送前确保没有这些文件：
- `genai_lib/`
- `utilities/`
- `qnn_model_prepare_*.py`
- `libGenie*.so`, `libQnn*.so`

检查命令：
```bash
git status | grep -E "genai_lib|utilities|qnn_model_prepare"
```

### npm 组织
如果 `@sparx` 不可用，更新 `packaging/npm/package.json`：
```json
{
  "name": "@your-org/sparx-cli"
}
```

---

## 🔍 验证 Release

### GitHub Release
```bash
gh release view v2.1.0 --repo OpenSparX/MasterAgent
```

### Homebrew
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/homebrew-masteragent/main/Formula/sparx.rb | head -20
```

### npm
```bash
npm view @sparx/cli
```

### 安装测试
```bash
# curl
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version

# Homebrew (macOS)
brew install openschbrid/tap/sparx

# npm
npm install -g @sparx/cli
```

---

## 🆘 故障排查

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| licence-gate 失败 | 源码中有 Qualcomm 文件 | `grep -r "Qualcomm Technologies" .` 找到并删除 |
| npm publish 失败 | NPM_TOKEN 过期 | 重新生成 token 并更新 secret |
| SHA256 不匹配 | tarball 被修改 | 运行 `./scripts/sync_release_metadata.sh` |
| 交叉编译失败 | 缺少工具链 | Workflow 会自动安装，检查日志 |

详细排查：`docs/GITHUB_SETUP.md` 常见问题章节

---

当前状态：✅ **所有代码就绪，可以立即配置并发布**
