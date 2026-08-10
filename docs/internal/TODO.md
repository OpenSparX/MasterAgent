# Remaining Setup Tasks

## Repository Infrastructure (Required Before First Release)

### 1. Create GitHub Repository
- [ ] Create `OpenSparX/MasterAgent` (or your preferred org/name)
- [ ] Push this codebase to `main` branch
- [ ] Enable GitHub Actions in repo settings

### 2. Configure GitHub Secrets
Go to **Settings → Secrets and variables → Actions**, add:
- [ ] `NPM_TOKEN` — from npmjs.com/settings/tokens (Automation token, not read-only)
- [ ] `HOMEBREW_TAP_TOKEN` — GitHub PAT with `repo` scope
- (GITHUB_TOKEN is auto-provided, no action needed)

### 3. Create Homebrew Tap Repository
```bash
gh repo create OpenSparX/homebrew-masteragent --public
cd /tmp
git clone https://github.com/OpenSparX/homebrew-masteragent.git
cd homebrew-tap
mkdir -p Formula
echo "# Homebrew Tap for sparx" > README.md
git add . && git commit -m "Initial commit" && git push
```

### 4. Register npm Organization
Choose one:
- Option A: Use existing org → update `packaging/npm/package.json` name field
- Option B: Create new org:
  ```bash
  npm login
  npm org create sparx  # or your preferred name
  ```

### 5. Enable GitHub Actions Permissions
In **Settings → Actions → General → Workflow permissions**:
- [x] Read and write permissions
- [x] Allow GitHub Actions to create and approve pull requests

---

## Real-Hardware Verification (Post-Release)

### 1. Test All 4 Installation Channels

**macOS arm64** (your machine):
```bash
# curl
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version && sparx init test1

# Homebrew
brew install openschbrid/tap/sparx
sparx --version

# npm
npm i -g @sparx/cli
sparx --version
```

**Linux x64** (Ubuntu/Debian):
```bash
# curl
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version && sparx init test2

# npm
npm i -g @sparx/cli
sparx --version
```

**Linux arm64** (Raspberry Pi / ARM server):
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version && sparx init test3
```

**macOS x64** (Intel Mac):
```bash
curl -fsSL https://raw.githubusercontent.com/OpenSparX/MasterAgent/main/scripts/install.sh | sh
sparx --version && sparx init test4
```

### 2. Verify Full Developer Flow

On an Android device (e.g., 骁龙8G5 phone):
```bash
sparx init my-agent
sparx pull qwen3-4b
sparx run                        # local deterministic routing works
sparx devices                    # detects phone, shows SoC + NPU
sparx deploy --device 1 --start  # pushes + starts daemon
sparx shell --device <serial>    # connects, can send messages
```

### 3. Test NPU Inference Path

Requires a device with Qualcomm QNN + Genie installed:
```bash
# On device, verify runtime presence
adb shell ls -lh /vendor/lib64/libQnnHtp.so
adb shell ls -lh /vendor/lib64/libGenie.so

# Deploy and test
sparx deploy --device 1 --model ~/.sparx/models/qwen3-4b.gguf --start
sparx shell --device <serial>
# In shell: send a query that triggers inference
# Verify: logs show Genie invocation, no seal validation errors
```

This tests:
- GenieModelRuntime probe() finds libGenie.so
- Dialog handle creation succeeds
- Streaming callback receives tokens
- UTF-8 boundary protection works
- Framework verifies streamed output matches final raw_output

### 4. Test Crash Recovery Demo

```bash
sparx demo crash
# Should show:
# - WAL written with ISSUED payment.charge + torn tail
# - Status: UNKNOWN
# - Recommendation: manual intervention required

sparx demo stream
# Should show:
# - 9 chunks streamed
# - StreamIntegrity: VERIFIED
# - Output matches accumulated deltas
```

---

## Known Risks (Cannot Be Verified Without Hardware)

### 1. Genie Draft Token Semantics
**Risk:** If Genie's callback delivers unverified speculative-decoding draft tokens that are later retracted, the framework accumulator will see deltas that don't match the final `raw_output`, triggering false-positive `INFERENCE_STREAM_OUTPUT_DIVERGED`.

**Current mitigation:** `genieTokenCallback` only flushes on `SENTENCE_COMPLETE`/`END`/`CONTINUE`/`BEGIN`, and drops buffer on `SENTENCE_REWIND`.

**Verification needed:** Run inference on real device, check logs for any `DIVERGED` errors. If they occur, we need to:
- Read Qualcomm docs (docs.qualcomm.com, if accessible)
- OR instrument the callback to log every token + sentence code
- OR disable speculative decoding in Genie config

### 2. Qwen3-VL Multi-Modal Input
**Risk:** `InferenceRequest` has no slot for image/embedding input, so `GenieDialog_embeddingQuery` API cannot be used for vision models.

**Fix:** Add `std::vector<uint8_t> embedding_input` to `InferenceRequest` before freezing the ABI.

---

## Optional Improvements (Post-Launch)

- [ ] Add `sparx upgrade` command (checks GitHub releases, re-runs installer)
- [ ] Add `sparx doctor --fix` (auto-repair common device issues)
- [ ] Add `sparx logs --device <serial>` (tail agent.log without adb)
- [ ] Add telemetry opt-in (anonymized usage stats for prioritizing features)
- [ ] Add Windows native support (currently requires WSL)
- [ ] Add `sparx init --template <name>` (pre-built agent templates)
- [ ] Generate `CHANGELOG.md` from git log + PR titles
- [ ] Set up docs site (docs.openschbrid.dev or GitHub Pages)

---

## Documentation Status

- ✅ `.github/RELEASE_CHECKLIST.md` — operator runbook for releases
- ✅ `README_DISTRIBUTION.md` — packaging architecture + decisions
- ✅ `docs/PACKAGING.md` — packaging internals (Chinese, 950 lines)
- ✅ `docs/INSTALL.md` — user install guide (English)
- ✅ `docs/DEVLOG_CLI.md` — CLI development log (Chinese, 250 lines)
- ⏳ `README.md` — needs update with install instructions
- ⏳ `CONTRIBUTING.md` — needs contributor guidelines
- ⏳ `CHANGELOG.md` — needs initial entry for v2.1.0

---

## Current Status: ✅ Code Complete, ⏳ Awaiting Repo Setup

All 18 packaging files written, tested locally, and verified:
- 8/8 build integrity checks pass
- 6/6 install flow checks pass
- 5/5 licence gate mutation tests pass (every mutant killed)
- End-to-end: curl install → init → version check → project scaffold

**Next action:** Create GitHub repo + configure secrets, then trigger release workflow.
