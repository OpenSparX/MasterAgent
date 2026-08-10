# Distribution Infrastructure Manifest

## Summary

Complete packaging and distribution system for sparx — 4 installation channels, cross-platform build matrix, Qualcomm licence protection, and automated testing.

**Status:** Code complete, awaiting repository infrastructure setup

**Total deliverables:** 18 files, ~1,300 lines (shell/JS/YAML/Ruby/Markdown)

---

## Core Scripts

### Build & Release
| File | Lines | Purpose |
|------|-------|---------|
| `scripts/build_release.sh` | 208 | Release build orchestrator — CMake Release + strip + tarball + SHA256 |
| `scripts/install.sh` | 151 | POSIX sh curl installer — platform detection, download, extract to ~/.local/bin |
| `scripts/sync_release_metadata.sh` | 67 | Sync version/checksums across sparx_main.cpp, package.json, sparx.rb |
| `scripts/generate_npm_platform_packages.sh` | 89 | Generate 4 npm platform sub-packages with os/cpu constraints |

### Security & Compliance
| File | Lines | Purpose |
|------|-------|---------|
| `scripts/check_license.sh` | 133 | Qualcomm licence gate — 4-layer check (source tree, binaries, tarballs, artifacts) |

### Testing
| File | Lines | Purpose |
|------|-------|---------|
| `scripts/tests/run_all.sh` | 28 | Test runner — orchestrates all 3 test suites |
| `scripts/tests/test_triple_contract.sh` | 82 | Platform triple contract — verifies build_release.sh ↔ install.sh consistency |
| `scripts/tests/test_install_e2e.sh` | 95 | Install flow E2E — local installer, version check, init command |
| `scripts/tests/test_npm_e2e.sh` | 87 | npm flow E2E — launcher resolution, exit code propagation |
| `scripts/tests/test_licence_gate.sh` | 156 | Mutation testing — plants 5 violations, confirms gate catches each |

**Total test coverage:** 19 automated checks
- 8 checks: build integrity (tarball structure, binary deps, version strings)
- 6 checks: install flow (local installer, sparx commands, npm launcher)
- 5 checks: licence gate mutations (source headers, binary strings, tarball contents, clean/dirty divergence, empty tree)

---

## Package Metadata

### Homebrew
| File | Lines | Purpose |
|------|-------|---------|
| `packaging/homebrew/sparx.rb` | 66 | Homebrew formula — 4 platform URLs + SHA256 + installation logic |

**Key features:**
- Platform-specific URLs via `on_macos` + `on_arm`/`on_intel`, `on_linux` + `on_arm`/`on_intel`
- SHA256 checksums synced by `sync_release_metadata.sh`
- Test block runs `sparx --version` post-install

### npm
| File | Lines | Purpose |
|------|-------|---------|
| `packaging/npm/package.json` | 34 | npm main package — optionalDependencies for 4 platform sub-packages |
| `packaging/npm/bin/sparx.js` | 42 | npm launcher — resolves platform binary, forwards args + exit code |
| `packaging/npm/install.js` | 38 | npm postinstall — verifies platform binary exists after optionalDeps resolve |

**Key features:**
- `optionalDependencies` pattern: npm auto-installs only matching platform
- Launcher uses `require.resolve()` to locate binary in node_modules
- Exit code propagation via `spawnSync(..., {stdio: 'inherit'})`

---

## CI/CD

### GitHub Actions
| File | Lines | Purpose |
|------|-------|---------|
| `.github/workflows/release.yml` | 154 | 4-gate pipeline: licence → test → build (4 platforms) → publish |

**Pipeline structure:**
```
licence-gate (independent)
      ↓
    test (ubuntu + macos matrix)
      ↓
   build (4 platform matrix: darwin-arm64/x64, linux-x64/arm64)
      ↓
  publish (creates GitHub Release, updates Homebrew tap, publishes npm)
```

**Build matrix:**
- darwin-arm64: macos-14 (native Apple Silicon)
- darwin-x64: macos-13 (native Intel)
- linux-x64: ubuntu-22.04 (native)
- linux-arm64: ubuntu-22.04 (cross-compile with gcc-aarch64-linux-gnu)

**Publish steps:**
1. Download all 4 platform artifacts
2. Generate combined SHA256SUMS
3. Create GitHub Release with softprops/action-gh-release@v2
4. Update Homebrew formula checksums, push to tap repo
5. Publish npm packages (main + 4 platform sub-packages)

---

## Documentation

### User-Facing
| File | Lines | Purpose |
|------|-------|---------|
| `docs/INSTALL.md` | 180 | Installation guide (English) — all 4 channels, platform notes, troubleshooting |

**Covers:**
- curl script installation (primary channel)
- Homebrew tap installation (macOS)
- npm installation (Node.js users)
- Manual tarball installation (offline/airgap)
- Platform-specific notes (WSL, glibc 2.17+, macOS 11+)
- Troubleshooting common issues

### Operator-Facing
| File | Lines | Purpose |
|------|-------|---------|
| `.github/RELEASE_CHECKLIST.md` | 243 | Release runbook — prerequisites, version bump, workflow trigger, verification, rollback |
| `docs/PACKAGING.md` | 383 | Packaging internals (Chinese) — architecture decisions, CI pipeline, testing strategy |
| `README_DISTRIBUTION.md` | 251 | Status document — what's ready, what's needed, how to release, known limitations |

**Release checklist covers:**
- One-time setup (GitHub secrets, Homebrew tap, npm org)
- Pre-release checks (tests, licence gate)
- Version bump process (3 files: sparx_main.cpp, package.json, sparx.rb)
- Workflow trigger (manual dispatch with version input)
- Post-release verification (test all 4 channels on real hardware)
- Rollback procedures (delete release, unpublish npm, revert formula)
- Troubleshooting (licence gate failures, cross-compile issues, token expiry)

---

## Architecture Decisions

### Why curl | sh is the primary channel?

Target audience: embedded/Android developers who have `adb` + NDK but not necessarily Node.js.

| Channel | Coverage | Dependencies | Update Mechanism |
|---------|----------|--------------|------------------|
| curl script | Linux/macOS/WSL | curl, tar (universal) | Manual re-run |
| Homebrew | macOS only | brew | `brew upgrade` |
| npm | Cross-platform | Node.js | `npm update -g` |
| Manual tarball | All platforms | None | Manual download |

**Decision:** curl is zero-dependency and covers the widest platform set.

### Why static linking?

sparx binary statically links libc++ and depends only on system libc:
- Linux: glibc 2.17+ (Ubuntu 14.04+, RHEL 7+, Debian 8+)
- macOS: 11+ (Big Sur, 2020)

This makes distribution "copy one file" with no runtime dependencies.

Genie is `dlopen`ed at runtime on-device (`/vendor/lib64/libGenie.so`), so the host binary has zero Qualcomm dependencies.

### Why the licence gate?

**Legal constraint:** Qualcomm AI Stack License permits SDK use but forbids standalone redistribution.

**Risk:** A single `cp` of a Qualcomm file + a GitHub Release = public licence violation.

**Solution:** 4-layer gate runs **before** any publish step:
1. Check source tree for Qualcomm copyright headers
2. Check binary artifacts for embedded Qualcomm strings
3. Check tarballs for `.py` / `.so` Qualcomm files
4. Verify clean vs dirty source consistency

Each check has been mutation-tested: we planted violations and confirmed the gate killed them.

### Why npm uses optionalDependencies?

npm `optionalDependencies` + `os`/`cpu` constraints make npm automatically install only the matching platform sub-package.

Example:
```json
{
  "name": "@sparx/cli-darwin-arm64",
  "os": ["darwin"],
  "cpu": ["arm64"]
}
```

On `npm install -g @sparx/cli`, npm resolves:
- macOS arm64 → installs `@sparx/cli-darwin-arm64` only
- Linux x64 → installs `@sparx/cli-linux-x64` only
- Windows → fails (no matching platform)

The main package's `bin/sparx.js` launcher then uses `require.resolve()` to locate the platform binary in node_modules.

---

## Known Limitations

### 1. Linux arm64 not verified on real hardware

CI cross-compiles with `gcc-aarch64-linux-gnu` but hasn't run the binary on actual arm64 Linux.

**Risk:** glibc version mismatch at runtime.

**Mitigation:** We target glibc 2.17 (Ubuntu 14.04+, 2012 vintage) for maximum compatibility.

**Verification needed:** Test on Raspberry Pi / ARM server after first release.

### 2. npm packages not published yet

`packaging/npm/` structure is complete and tested locally (launcher resolves paths correctly, exit codes propagate), but `npm publish` hasn't run.

**Blockers:**
- NPM_TOKEN secret not configured
- npm org (@sparx or @openschbrid) not registered

### 3. Homebrew formula not in tap repo

`packaging/homebrew/sparx.rb` exists but `OpenSparX/homebrew-masteragent` repo doesn't.

**Blockers:**
- Tap repo not created
- HOMEBREW_TAP_TOKEN secret not configured

### 4. Windows native support

Currently requires WSL. Native Windows builds would need:
- MSVC / MinGW cross-compile
- .zip instead of .tar.gz
- PowerShell installer script
- npm platform sub-package for win32-x64

---

## Testing Summary

All 19 automated checks passing:

### Build Integrity (8 checks)
```
✓ tarball exists at dist/sparx-2.1.0-darwin-arm64.tar.gz
✓ tarball size > 30KB (actual: 353KB)
✓ tarball contains sparx-2.1.0-darwin-arm64/bin/sparx
✓ tarball contains README.md
✓ extracted binary is executable
✓ binary has no third-party dynamic dependencies
✓ VERSION.json contains commit hash + timestamp
✓ sparx --version outputs correct version
```

### Install Flow (6 checks)
```
✓ local installer extracts to ~/.local/bin/sparx
✓ sparx --version works after install
✓ sparx init creates agent.yaml
✓ npm package.json structure is valid
✓ npm launcher resolves darwin-arm64 binary path
✓ npm launcher propagates child exit code
```

### Licence Gate Mutations (5 checks)
```
✓ Clean tree passes (baseline)
✓ Detects Qualcomm header in source tree (mutant killed)
✓ Detects Qualcomm string in binary (mutant killed)
✓ Detects Qualcomm file in tarball (mutant killed)
✓ Detects clean/dirty divergence (mutant killed)
```

### End-to-End Verification
```bash
$ curl -fsSL http://127.0.0.1:8888/install.sh | sh
  sparx installer
  platform: darwin-arm64
  version:  2.1.0
  install:  ~/.local/bin/sparx
  ✓ installed

$ ~/.local/bin/sparx --version
sparx 2.1.0
  commit:  1b3905c
  target:  darwin-arm64
  kernel:  master_agent v2.0.0

$ ~/.local/bin/sparx init test-agent
  ✓ Project created at ./test-agent
  ✓ Template hello-world installed
```

---

## Next Steps

### Immediate (Repo Infrastructure Setup)

1. **Create GitHub repository** `OpenSparX/MasterAgent` (or your preferred org/name)
2. **Configure GitHub secrets:**
   - `NPM_TOKEN` from npmjs.com/settings/tokens
   - `HOMEBREW_TAP_TOKEN` as GitHub PAT with `repo` scope
3. **Create Homebrew tap repository** `OpenSparX/homebrew-masteragent`
4. **Register npm organization** (@sparx preferred, or @openschbrid)
5. **Push code to main branch**
6. **Trigger release workflow** in GitHub UI (Actions → Release → Run workflow → enter version)

### Post-Release Verification

1. **Test all 4 installation channels:**
   - curl script (macOS + Linux)
   - Homebrew (macOS)
   - npm (cross-platform)
   - Manual tarball (any platform)

2. **Verify on real hardware:**
   - macOS arm64 ✓ (your machine)
   - macOS x64 (Intel Mac)
   - Linux x64 (Ubuntu/Debian)
   - Linux arm64 (Raspberry Pi / ARM server)

3. **End-to-end developer flow:**
   - `sparx init my-agent`
   - `sparx pull qwen3-4b`
   - `sparx run` (local routing)
   - `sparx devices` (detect Android device)
   - `sparx deploy --device 1 --start`
   - `sparx shell --device <serial>`

4. **NPU inference path:**
   - Deploy to Qualcomm device with QNN + Genie
   - Send inference request
   - Verify streaming callback works
   - Check logs for Genie invocation + no seal errors

---

## File Tree

```
v2/
├── scripts/
│   ├── build_release.sh              # Release build + tarball
│   ├── install.sh                    # curl installer (POSIX sh)
│   ├── check_license.sh              # Qualcomm licence gate
│   ├── sync_release_metadata.sh      # Version/SHA256 sync
│   ├── generate_npm_platform_packages.sh  # npm platform sub-packages
│   └── tests/
│       ├── run_all.sh                # Test orchestrator
│       ├── test_triple_contract.sh   # Platform triple consistency
│       ├── test_install_e2e.sh       # Install flow E2E
│       ├── test_npm_e2e.sh           # npm launcher E2E
│       └── test_licence_gate.sh      # Licence gate mutations
│
├── packaging/
│   ├── homebrew/
│   │   └── sparx.rb                  # Homebrew formula
│   └── npm/
│       ├── package.json              # npm main package
│       ├── bin/sparx.js              # npm launcher
│       └── install.js                # npm postinstall check
│
├── .github/
│   ├── workflows/
│   │   └── release.yml               # 4-gate CI pipeline
│   └── RELEASE_CHECKLIST.md          # Operator runbook
│
├── docs/
│   ├── INSTALL.md                    # User install guide (English)
│   └── PACKAGING.md                  # Packaging internals (Chinese)
│
├── README_DISTRIBUTION.md            # Status document
├── DISTRIBUTION_MANIFEST.md          # This file
└── TODO.md                           # Remaining setup tasks
```

---

## Contact

For questions about packaging/distribution:
- Read `README_DISTRIBUTION.md` first (high-level overview)
- Check `.github/RELEASE_CHECKLIST.md` for release procedures
- Read `docs/PACKAGING.md` for implementation details (Chinese)
- Review `TODO.md` for remaining setup tasks

**Current status:** Code complete, awaiting repo infrastructure setup
