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
./scripts/check_license.sh
```

All checks must pass before proceeding. The release build derives the version
from the tag (or accepts `SPARX_VERSION` explicitly); do not hand-edit a source
version constant.

### Step 2: Prepare the Release Commit

Update `CHANGELOG.md` with the release notes, then commit all intended changes:

```bash
git add CHANGELOG.md
git commit -m "release: vX.Y.Z"
```

Keep the worktree clean before tagging. The repository policy is to create a
new patch tag; never delete or recreate an existing tag.

### Step 3: Tag and Push

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin main --tags
```

Pushing a new `vX.Y.Z` tag triggers `.github/workflows/release.yml`.

### Step 4: Verify the GitHub Actions Release Workflow

The current workflow will:
- ✅ Run the Qualcomm licence gate (`scripts/check_license.sh`)
- ✅ Run the C++ suite and packaging suite on Linux and macOS
- ✅ Build 4 platform artifacts (darwin-arm64, darwin-x64, linux-x64, linux-arm64)
- ✅ Create a GitHub Release with artifacts and combined checksums

The current workflow does **not** publish npm packages or update a Homebrew
tap. Those channels require a separate, explicitly configured distribution
workflow. `scripts/update_packaging.sh` can prepare local formula/manifests
from a complete set of artifacts; review its diff before committing anything.

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

## Recovery from a Bad Release

If a published release has a critical issue, do **not** delete or recreate its
git tag. Publish a new patch version with the fix (for example, `v2.1.9`),
clearly mark the affected release as superseded, and update the changelog.
If the GitHub Release itself must be hidden, archive or edit its notes while
preserving the immutable tag history.

For npm or Homebrew, follow the registry/tap's documented deprecation or
reversion process. Do not unpublish or force-push as an automated rollback.

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

The formula must reference the exact artifact checksum. After all four
artifacts are available in `dist/`, regenerate packaging metadata with:

```bash
./scripts/update_packaging.sh X.Y.Z
git diff -- packaging/homebrew packaging/npm
```

Review the generated diff and make a new corrective commit if needed. Never
amend a published release commit or force-push a release branch.

---

## Post-release Tasks

- [ ] Update CHANGELOG.md
- [ ] Notify users (Twitter, Discord, Slack)
- [ ] Update docs.openschbrid.dev (if exists)
- [ ] Close milestone in GitHub Issues
