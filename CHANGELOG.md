# Changelog

All notable changes to Sparx are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [2.1.11] — 2026-08-10

### Changed
- `sparx run` now prints its actual compile-time version (`SPARX_VERSION`) in
  the banner instead of a hardcoded `v0.1.0`.
- When no GGUF model is resolved, `sparx run` prints `reality=SIMULATED ·
  runtime=none` with a clear note on how to configure a model. Previously it
  printed `reality=SIMULATED · runtime=mock` and returned a canned Chinese
  string that looked like real inference output.
- `--resume` no longer prints `✓ recovered from WAL (torn tail repaired)`. WAL
  recovery is implemented in the kernel orchestrator but not yet wired into the
  CLI — the old message was a false positive.
- When a model IS configured (`--model`, `model.path` in agent.yaml, or the
  `SPARX_MODEL` env var), `sparx run` constructs a `LlamaCppModelRuntime` and
  streams real tokens. Banner reads `reality=REAL · runtime=llama-cpp` and the
  REPL prints actual TTFT, total latency, token count, and stream-integrity
  status rather than placeholders.

### Added
- `AgentConfig` now parses `model.path` and `model.endpoint` from agent.yaml.
- Model path resolution precedence: `--model` flag > `model.path` > `$SPARX_MODEL`.
- **`sparx add skill <name>`** — scaffolds `skills/<name>.yaml` and registers the
  name in `agent.yaml`. The registration half is what developers forget by hand,
  and an unregistered skill is silently inert. Refuses to overwrite an existing
  skill, rejects path traversal and unsafe names, and preserves comments and
  ordering in `agent.yaml` (line-wise rewrite rather than YAML round-trip).
- **Skill YAML loading** (`sparx_skill_loader.h`). Deterministic matching
  previously hardcoded a check for the string `"hello"`, so every `skills/*.yaml`
  file on disk — including the ones shipped in `examples/` — was ignored. Editing
  a skill's patterns had no effect and produced no error. `sparx run` now loads
  each registered skill, matches patterns case-insensitively for ASCII, and
  reports `skills: N/M loaded` at startup, naming any registered skill with no
  YAML file.
- Skill handlers support `handler.response` and `handler.response_template`,
  including `|` literal block scalars. Because parameter extraction is not
  implemented, unfilled `{placeholder}` tokens are listed explicitly rather than
  printed at the user as if they were text.
- `docs/architecture.md` — bilingual architecture reference with Mermaid diagrams
  for system layers, request flow, the inference seal/digest trust model, WAL
  terminal states, runtime topology, and MCP invocation.
- **`DagBuilder`** (`cli/include/sparx_dag_builder.h`) — fluent builder for small
  multi-step plans. Hand-constructing a 2-node `IntentDAG` took ~25 lines of
  boilerplate, and `validateDAG` rejects an incomplete plan with a reject code but
  no indication of which field was missing. `build()` returns the `IntentDAG` and
  a matching `AdmissionContext` together, because the two must agree: every node
  action has to appear in `allowed_capabilities`, and a node with
  `max_attempts > 1` needs a retry policy keyed by its action.

      auto [dag, admission] = DagBuilder("turn-off-ac")
          .node("read_temp", "vehicle.climate.getTemperature")
          .node("set_ac", "vehicle.climate.setPower", {{"power", "off"}})
              .after("read_temp")
          .build();

  Nodes with no `.after()` have no incoming edge and start together — parallelism
  is the default, sequencing is what you opt into. `retries()` requires an explicit
  idempotency policy and a non-empty retryable-error set; there is no default,
  because choosing one would assert a safety property the builder cannot know.
  P0 priority additionally requires `p0Authorization()` with a real
  `trusted-safety:` reference — the builder will not mint its own, since a builder
  that could would make the orchestrator's P0 check unenforceable for every local
  caller. `priority(P0)` alone is therefore rejected, and a test asserts that.
- **Plan export** — `dagToJson()`, `dagToMermaid()`, and `dagToText()`. Empty
  `params` and `dependencies` are omitted from JSON rather than emitted as `{}`,
  so the output does not imply configuration that is not there. Mermaid output is
  pasteable into docs and annotates an edgeless multi-node plan as parallel
  instead of rendering unexplained orphan boxes. Text output marks roots (`●`) and
  dependents (`○`), and labels multi-dependency nodes as joins.

### Fixed
- `examples/automotive_assistant/agent.yaml` registered 5 skills but shipped only
  3 YAML files; `phone` and `vehicle_status` were silently inert. Both added.
  A test now fails if any shipped example registers a skill with no file.
- `test_cli_commands.sh`: 13 → 36 assertions, covering `add skill` scaffolding and
  registration, name validation and path traversal, the edited-skill-fires loop,
  banner/version agreement, the absence of the false WAL claim, and skill
  completeness across all shipped examples. Verified against four mutants
  (hardcoded banner, restored WAL claim, disabled loader, skipped registration) —
  each is caught.
- `tests/test_sparx_dag_builder.cpp` — 11 cases, 52 assertions, every plan shape
  validated through a real `Orchestrator` rather than a mock, so the builder's
  contract is defined by the validator and not by its own source. Verified against
  four mutants (dropped retry policy, self-granted P0 authorization, unpropagated
  node deadlines, placeholder capabilities) — each is caught. ctest 15 → 16 targets,
  all passing.
- `DagBuilder::after({"a", "b"})` — the documented braced-list form did not compile:
  a two-element list of `const char*` is ambiguous between the `vector` overload and
  `std::string`'s `(InputIt, InputIt)` constructor. Added an `initializer_list`
  overload, which both prefer.
- **CI now runs on pull requests.** `CONTRIBUTING.md` told contributors "Automated
  checks: CI must pass (build, tests, licence gate)", but the only workflow was
  `release.yml`, which triggers on tag push and `workflow_dispatch`. Nothing ran on
  a PR — including the licence gate, the one check whose failure is a legal problem
  rather than a bug. Added `.github/workflows/ci.yml`: licence gate, then build +
  ctest + CLI and licence integration tests on ubuntu-latest and macos-latest.
  Packaging tests are deliberately excluded, because 18 of their assertions require
  a published release artifact and would leave CI permanently red — a red CI that
  is normal to ignore is worse than none.
- `CONTRIBUTING.md` documented a Google Test dependency that does not exist. There
  is no gtest anywhere in the tree; tests are plain executables using `expect()`
  from `tests/test_support.h`, which throws. The example test, the registration
  step, and the named test target (`preprocessing_test`, which does not exist —
  it is `test_preprocess`) are all corrected.
- **The release has been blocked since v2.1.6 by one missing `env:` block.**
  `release.yml` ran `sync_npm_version.sh` without `SPARX_VERSION`, unlike the two
  steps after it. The script then fell back to `git describe --tags --always`, and
  `actions/checkout` does not fetch tags — so describe returned a bare commit SHA
  and every `package.json` was written with `"version": "7b3ee26"`. That surfaced
  far downstream as five confusing `manifest wrong` / `optionalDependencies drift`
  failures, which failed the packaging gate, which blocked publish. v2.1.7 through
  v2.1.10 were tagged but never released.

  Fixed in two places: the workflow now passes `SPARX_VERSION` to the sync step,
  and `sync_npm_version.sh` refuses any value that is not valid semver instead of
  silently writing a commit SHA into a version field. The guard names the likely
  cause and the two ways to fix it, because the original failure gave no
  indication of where the bad value came from.

  Packaging suite: **76 passed / 18 failed → 94 passed / 0 failed.** These were
  previously described in this project's own notes as "pre-existing version drift
  from the unrun release workflow" — that was circular and wrong. The release did
  not fail to run; it ran and failed, and this was why.

## [2.1.10] — 2026-08-10

### Fixed
- `update_packaging.sh`: BSD sed basic-regex has no `\|` alternation, so the
  pattern that rewrites tarball filenames in the Homebrew formula silently
  matched nothing on macOS — leaving old version strings in the filename while
  the download path said the new version. Every `brew install` would 404.
- `test_triple_contract.sh`: used `head -1` of all artifacts, so a leftover
  tarball from a previous release would sort first and fail the assertion even
  when the current build was correct. Now checks for this version's artifact
  specifically and fails only when dist/ holds *other* versions with none for
  the current.
- Compiled binary (`bin/sparx`) was tracked in git under
  `packaging/npm/platforms/darwin-arm64/`; untracked and gitignored (written by
  `update_packaging.sh` at publish time, not source code).

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
