# Changelog

All notable changes to Sparx are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [2.1.9] — 2026-08-10

### Fixed
- Distribution channel URLs were inconsistent: npm platform packages pointed at
  `openschbrid/sparx` while install.sh, README, and the Homebrew formula used
  `OpenSparX/MasterAgent`. All channels now agree.
- Homebrew formula header named a non-existent `homebrew-sparx` tap and used the
  wrong-case install path; corrected to `OpenSparX/masteragent/sparx`.

### Changed
- RELEASE_CHECKLIST.md now matches what `release.yml` actually does (GitHub
  Release only — npm and Homebrew publishing are manual), references the scripts
  that exist, and its recovery section follows the never-delete-tags policy.

## [2.1.8] — 2026-08-11

### Added
- `sparx version` now shows real git-derived version in local builds (was `0.0.0-dev`)
- CHANGELOG.md

## [2.1.7] — 2026-08-11

### Added
- `sparx init` now generates a bilingual README with run commands, layout, and skill-adding guide
- English documentation: WAL_RECOVERY.md, MCP_SERVICES.md, QUALCOMM_NPU.md
- Chinese documentation: WAL_RECOVERY_zh-CN.md, MCP_SERVICES_zh-CN.md, QUALCOMM_NPU_zh-CN.md, CONTRIBUTING_zh-CN.md
- Enhanced IoT Edge and Smart Home example READMEs with architecture diagrams, file listings, and customization guides
- 5 new skill files across smart_home and iot_edge examples
- CLI functional tests (test_cli_commands.sh — 13 test cases)
- Production deployment guide (PRODUCTION_DEPLOYMENT.md)
- Performance tuning guide (PERFORMANCE_TUNING.md)

### Fixed
- test_cli_commands.sh looked for binary at `build/sparx` instead of `build/cli/sparx` — was silently skipping since v2.1.6
- CONTRIBUTING_zh-CN.md quoted Qualcomm proprietary header verbatim, triggering licence gate
- Leaked test fixture (sparx-9.9.9-test.tar.gz) persisting in dist/ after interrupted test runs

## [2.1.6] — 2026-08-10

### Fixed
- darwin-x64 CI build: use macos-14 runner with CMAKE_OSX_ARCHITECTURES
- npm e2e test: prefer dist/ binary over stale platforms/ binary
- Version derivation: read from git tag instead of hardcoding

## [2.1.5] — 2026-08-10

### Fixed
- Pass SPARX_VERSION through CI test job to avoid dirty-tree version mismatch
- Sync npm package versions to git tag before packaging tests run

## [2.1.4] — 2026-08-10

### Fixed
- Test scripts read version from git tag instead of hardcoding v2.1.0
- Triple contract test uses extracted function instead of process substitution (POSIX compat)

## [2.1.3] — 2026-08-10

### Added
- English translations of system overview and build guide

## [2.1.2] — 2026-08-10

### Added
- npm distribution: `@sparx/cli` with platform-specific optional dependencies
- Homebrew formula for macOS installation
- curl-based installer script

## [2.1.1] — 2026-08-10

### Added
- GitHub Actions CI workflow (licence-gate → test → build → publish)
- Packaging test suite (triple contract, licence gate, install e2e, npm e2e)

## [2.1.0] — 2026-08-10

### Added
- Complete product restructuring: bilingual README, examples, distribution
- sparx CLI with init, run, demo, doctor, deploy, shell commands
- Three example projects: automotive_assistant, smart_home, iot_edge
- Architecture diagrams (Mermaid + ASCII)
- Community files: CONTRIBUTING.md, CODE_OF_CONDUCT.md, SECURITY.md

### Core (carried from v2.0)
- C++17 agent framework (38,582 LOC)
- Qualcomm QNN NPU integration
- WAL recovery with Unknown terminal state
- MCP service orchestration with P0/P1/P2 priority scheduling
- Two-stage inference (deterministic-first routing)
- 15 core test suites covering crash recovery, concurrency, fault injection
