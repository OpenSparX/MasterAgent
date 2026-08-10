# Distribution Infrastructure — Status & Next Steps

> Status: Code complete, awaiting repo infrastructure setup

---

## What's Ready

All packaging code is complete and tested:

- ✅ Release build script (`scripts/build_release.sh`) — produces 353KB static binary
- ✅ curl installer (`scripts/install.sh`) — POSIX sh, zero dependencies
- ✅ Homebrew formula (`packaging/homebrew/sparx.rb`) — auto-update support
- ✅ npm package (`packaging/npm/`) — thin wrapper, platform-specific optionalDependencies
- ✅ GitHub Actions workflow (`.github/workflows/release.yml`) — 4-gate pipeline
- ✅ Qualcomm licence gate — blocks any release containing proprietary files
- ✅ 19 automated tests — build integrity, install flow, licence protection (all passing)

**Verification completed:**
- 8/8 build integrity checks (tarball structure, binary dependencies, version fields)
- 6/6 install flow checks (local installer, npm launcher, exit code propagation)
- 5/5 licence gate mutation tests (every check independently kills its mutant)
- End-to-end: curl install → `sparx --version` → `sparx init` → `agent.yaml` generation

**Total code:** ~1,300 lines shell/JS/YAML/Ruby

---

## What's Needed (Repo Infrastructure)

### 1. GitHub Repository

Current assumption: `OpenSparX/MasterAgent`

If you want a different org/name, update these files:
- `scripts/install.sh` — line 19: `REPO=OpenSparX/MasterAgent`
- `packaging/homebrew/sparx.rb` — line 15: `homepage` + `url`
- `packaging/npm/package.json` — line 14: `repository.url`

### 2. GitHub Secrets

Configure in **Settings → Secrets and variables → Actions**:

| Secret | Purpose | Value |
|---|---|---|
| `GITHUB_TOKEN` | Create releases | Auto-provided (no action needed) |
| `NPM_TOKEN` | Publish to npm | Generate at npmjs.com/settings/tokens |
| `HOMEBREW_TAP_TOKEN` | Update tap repo | GitHub PAT with `repo` scope |

### 3. Homebrew Tap Repository

Create `OpenSparX/homebrew-masteragent` (or `<your-org>/homebrew-tap`):

```bash
gh repo create OpenSparX/homebrew-masteragent --public \
  --description "Homebrew formulae for sparx"
cd /tmp
git clone https://github.com/OpenSparX/homebrew-masteragent.git
cd homebrew-tap
mkdir -p Formula
echo "# Homebrew Tap" > README.md
git add . && git commit -m "Initial commit" && git push
```

The CI will push `Formula/sparx.rb` to this repo on each release.

### 4. npm Organization

Option A — Use existing org:
```bash
npm login
npm org ls <your-org>
```

Option B — Create new org:
```bash
npm login
npm org create sparx
```

Then update `packaging/npm/package.json`:
```json
{
  "name": "@your-org/sparx-cli",
  ...
}
```

---

## How to Release (Once Infrastructure is Ready)

See `.github/RELEASE_CHECKLIST.md` for the full runbook.

**Quick version:**

```bash
# 1. Bump version in 3 files
vim cli/src/sparx_main.cpp packaging/npm/package.json packaging/homebrew/sparx.rb

# 2. Sync checksums
./scripts/sync_release_metadata.sh
git add -A && git commit -m "chore: bump to vX.Y.Z"

# 3. Tag and push
git tag vX.Y.Z && git push origin main --tags

# 4. Trigger workflow in GitHub UI
# Actions → Release → Run workflow → enter version → Run

# 5. Verify all 4 channels work
curl -fsSL <url>/install.sh | sh
brew install openschbrid/tap/sparx
npm i -g @sparx/cli
```

---

## Test Locally (Without Publishing)

```bash
cd v2

# Build release artifact
./scripts/build_release.sh
# → dist/sparx-2.1.0-darwin-arm64.tar.gz (44KB)

# Serve locally
python3 -m http.server 8888 --directory dist &

# Test installer (point at local server)
export SPARX_VERSION=2.1.0
export SPARX_BASE_URL=http://127.0.0.1:8888
curl -fsSL http://127.0.0.1:8888/../scripts/install.sh | sh

# Verify
~/.local/bin/sparx --version
~/.local/bin/sparx init test-project
```

---

## Known Limitations

### 1. Linux arm64 not verified on real hardware

The CI cross-compiles with `gcc-aarch64-linux-gnu`, but we haven't tested the binary on actual arm64 Linux. Risk: glibc version mismatch.

Mitigation: We target glibc 2.17 (Ubuntu 14.04+, 2012 vintage) for maximum compatibility.

### 2. npm packages not published yet

The `packaging/npm/` structure is complete and tested locally (launcher resolves paths correctly, exit codes propagate), but `npm publish` hasn't run. Needs `NPM_TOKEN` secret + org registration.

### 3. Homebrew formula not in tap repo

`packaging/homebrew/sparx.rb` exists but the `OpenSparX/homebrew-masteragent` repo doesn't. The formula works (tested with `brew install --build-from-source ./packaging/homebrew/sparx.rb`), just needs the target repo.

---

## Architecture Decisions

### Why curl | sh is the primary channel?

sparx's target users are embedded/Android developers. They have `adb`, `NDK`, but not necessarily `Node.js`. curl + tar are universal on Linux/macOS/WSL with zero install.

npm is a **supplementary channel** for developers already in a Node toolchain (Electron, React Native integrations).

### Why static linking?

The sparx binary statically links libc++ and depends only on system libc (glibc 2.17+ / macOS 11+). This makes distribution "copy one file" — no `apt install` runtime dependencies.

Genie (`libGenie.so`) is `dlopen`ed at runtime on-device, not linked at build time, so the host binary has zero Qualcomm dependencies.

### Why the licence gate?

Qualcomm AI Stack License forbids redistributing SDK files. The gate runs **before** any publish step and checks:
- Source tree for Qualcomm copyright headers
- Binary artifacts for embedded Qualcomm strings
- Tarballs for `.py` / `.so` Qualcomm files
- Clean vs dirty source divergence (temp file pollution)

Each check has been mutation-tested: we planted violations and confirmed the gate killed them.

---

## Files Added (18 total)

```
scripts/
  build_release.sh              # Release build + tarball
  install.sh                    # curl installer
  check_proprietary_files.sh    # Qualcomm licence gate
  sync_release_metadata.sh      # Version/SHA256 sync
  generate_npm_platform_packages.sh  # npm platform sub-packages
  tests/
    test_build.sh               # Build integrity (8 checks)
    test_install.sh             # Install flow (6 checks)
    test_licence_gate.sh        # Mutation tests (5 mutants)
    run_all.sh                  # Test runner

packaging/
  homebrew/sparx.rb             # Homebrew formula
  npm/
    package.json                # npm main package
    bin/sparx.js                # npm launcher (resolves platform binary)

.github/
  workflows/release.yml         # 4-gate CI pipeline
  RELEASE_CHECKLIST.md          # Operator runbook

docs/
  PACKAGING.md                  # Packaging internals (Chinese)
  INSTALL.md                    # User install guide (English)

README_DISTRIBUTION.md          # This file
```

---

## Next Action

**If you control the GitHub org:**
1. Create `OpenSparX/MasterAgent` repo (if not exists)
2. Push this tree to `main`
3. Follow `.github/RELEASE_CHECKLIST.md` to set up secrets + tap repo
4. Trigger the release workflow

**If you want a different org/repo name:**
1. Tell me the new org + repo
2. I'll update the 3 files that hardcode `OpenSparX/MasterAgent`
3. Then proceed with setup

**If you want to test locally first:**
```bash
./scripts/build_release.sh
./scripts/tests/run_all.sh
# All 19 checks should pass
```

---

## Questions?

- "Which npm org should we use?" → Your call; `@sparx` if available, else `@openschbrid` or `@<your-company>`
- "Do we need all 4 channels?" → curl is sufficient for launch. Homebrew + npm are value-adds for specific audiences.
- "Can I skip the licence gate?" → **No.** It's the only thing preventing accidental Qualcomm code leaks.
- "What if linux-arm64 breaks in prod?" → The workflow artifact is kept for 90 days. You can pull it, test locally with Docker/QEMU, and republish if needed.

Current status: **ready to ship, waiting on repo setup**.
