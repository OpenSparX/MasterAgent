# Release Checklist

This document outlines the steps to publish a new sparx release.

## Prerequisites (One-time Setup)

### 1. GitHub Repository Secrets

Configure the following secrets in **Settings → Secrets and variables → Actions**:

| Secret | Purpose | How to get it |
|---|---|---|
| `GITHUB_TOKEN` | Create releases, upload artifacts | Auto-provided by GitHub Actions (no setup needed) |
| `NPM_TOKEN` | Publish to npm registry | `npm login` → generate token at npmjs.com/settings/tokens |
| `HOMEBREW_TAP_TOKEN` | Push formula to tap repo | Create GitHub PAT with `repo` scope |

### 2. Create Homebrew Tap Repository

```bash
# Create a new GitHub repo: OpenSparX/homebrew-masteragent
gh repo create OpenSparX/homebrew-masteragent --public --description "Homebrew tap for sparx"

# Initial commit
cd /tmp
git clone https://github.com/OpenSparX/homebrew-masteragent.git
cd homebrew-tap
mkdir -p Formula
echo "# Homebrew Tap for sparx" > README.md
git add README.md Formula/
git commit -m "Initial commit"
git push origin main
```

### 3. Register npm Organization

Option A: Use existing org
```bash
npm login
# Verify: npm org ls <your-org>
```

Option B: Create new org
```bash
npm login
npm org create sparx
```

Then update `packaging/npm/package.json` to use the correct scope:
```json
{
  "name": "@your-org/sparx-cli",
  ...
}
```

### 4. Verify GitHub Actions Permissions

In **Settings → Actions → General → Workflow permissions**, ensure:
- ✅ Read and write permissions
- ✅ Allow GitHub Actions to create and approve pull requests

---

## Release Process

### Step 1: Pre-release Checks

```bash
cd v2

# Clean build + full test suite
rm -rf /tmp/ma_build
cmake -S . -B /tmp/ma_build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/ma_build
ctest --test-dir /tmp/ma_build --output-on-failure

# Build release artifacts
./scripts/build_release.sh

# Run packaging tests
./scripts/tests/run_all.sh

# Verify licence gate passes
./scripts/check_proprietary_files.sh
```

All checks must pass before proceeding.

### Step 2: Version Bump

Edit the following files to update version to `X.Y.Z`:

- `cli/src/sparx_main.cpp` — `printVersion()` function
- `packaging/npm/package.json` — `version` field
- `packaging/homebrew/sparx.rb` — `version` line

Then sync checksums:

```bash
./scripts/sync_release_metadata.sh
git add -A
git commit -m "chore: bump version to X.Y.Z"
```

### Step 3: Tag and Push

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin main --tags
```

### Step 4: Trigger GitHub Actions Release Workflow

1. Go to **Actions → Release** in the GitHub UI
2. Click **Run workflow**
3. Select branch: `main`
4. Enter version: `X.Y.Z`
5. Click **Run workflow**

The workflow will:
- ✅ Run licence gate (fails if any Qualcomm files detected)
- ✅ Run C++ tests (15 tests) + packaging tests (19 tests)
- ✅ Build 4 platform artifacts (darwin-arm64, darwin-x64, linux-x64, linux-arm64)
- ✅ Create GitHub Release with artifacts
- ✅ Update Homebrew formula with checksums
- ✅ Publish npm packages

### Step 5: Verify Installation Channels

Test all 4 channels on different machines:

#### curl script (macOS + Linux)
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version
sparx init test-project
```

#### Homebrew (macOS)
```bash
brew install openschbrid/tap/sparx
sparx --version
```

#### npm (cross-platform)
```bash
npm install -g @sparx/cli
sparx --version
```

#### Manual tarball (any platform)
```bash
# Download from https://github.com/OpenSparX/MasterAgent/releases/latest
tar -xzf sparx-X.Y.Z-<platform>.tar.gz
./sparx-X.Y.Z-<platform>/bin/sparx --version
```

### Step 6: Announce Release

Post release notes with:
- What's new (features, bugfixes)
- Breaking changes (if any)
- Installation instructions
- Known issues

---

## Rollback

If a release has critical issues:

1. Delete the GitHub release + tag
   ```bash
   gh release delete vX.Y.Z --yes
   git tag -d vX.Y.Z
   git push origin :refs/tags/vX.Y.Z
   ```

2. Unpublish npm package (within 72h of publish)
   ```bash
   npm unpublish @sparx/cli@X.Y.Z
   ```

3. Revert Homebrew formula commit
   ```bash
   cd homebrew-tap
   git revert <commit-sha>
   git push origin main
   ```

---

## Troubleshooting

### licence-gate fails with "Qualcomm header detected"

A Qualcomm-licensed file was accidentally committed. Find and remove it:

```bash
grep -r "Qualcomm Technologies, Inc." --include="*.py" --include="*.cpp" --include="*.h" .
git rm <offending-file>
```

### Cross-compile fails for linux-arm64

The workflow uses `gcc-aarch64-linux-gnu`. If it fails:
- Check CMake toolchain file is correct
- Verify glibc version compatibility (we target 2.17+)
- Test locally with Docker:
  ```bash
  docker run --rm -v $(pwd):/work -w /work ubuntu:20.04 \
    bash -c "apt update && apt install -y cmake ninja-build gcc-aarch64-linux-gnu && \
             ./scripts/build_release.sh"
  ```

### npm publish fails with 403

Token expired or wrong scope. Regenerate:
```bash
npm login
npm token create --read-only=false
# Update GITHUB_TOKEN secret
```

### Homebrew formula SHA256 mismatch

The tarball changed after `sync_release_metadata.sh` ran. Re-run:
```bash
./scripts/sync_release_metadata.sh
git add packaging/homebrew/sparx.rb
git commit --amend --no-edit
git push --force-with-lease
```

---

## Post-release Tasks

- [ ] Update CHANGELOG.md
- [ ] Notify users (Twitter, Discord, Slack)
- [ ] Update docs.openschbrid.dev (if exists)
- [ ] Close milestone in GitHub Issues
