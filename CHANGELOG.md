# 0.4.0-alpha.3 — execution contracts and HTTP business integration

- Add contextual tools with session/request identity, stable scoped idempotency
  keys, monotonic deadlines and shared cooperative cancellation signals.
- Introduce typed committed/failed/unknown outcomes; preserve the legacy callback API.
- Check cancellation before dispatch; preserve authoritative results returned after cancellation.
- Add an independent HTTP inventory service with atomic stock/idempotency receipts,
  lost-response injection, external reconciliation and service/runtime restart tests.
- Make public-runtime static analysis blocking; run ASan/UBSan and dependency-free
  builds in CI. Verify contextual consumers and the HTTP example from SDK archives.

# 0.4.0-alpha.2 — durable receipts and recovery

- Add an opt-in SQLite store with WAL/FULL synchronization, exclusive ownership,
  persistent request outcomes/history, conservative UNKNOWN recovery and audited reconciliation.
- Fail closed on storage errors; write validated dispatch information before tool callbacks.
- Add CLI state, JSONL, recovery, reconciliation, HTTP timeout and credential-env options.
- Test real process termination after an external fsync, storage failure injection,
  restart replay/history, locking, corrupted stores and schema versions.
- Package provenance metadata/checksums and validate persistent installed consumers.

# 0.4.0-alpha — open reference runtime

- Add an independently runnable reference CLI and installable C++ Core/Http SDK.
- Validate tool arguments; scope in-memory request replay and history by session.
- Keep test expectations active in Release and verify installed/relocated consumers.
- Fix eval executable paths, failure exit status, and Bash counter portability.
- Bound edge/cloud inference waits and preserve backend lifetime after timeouts.
- Initialize intent metadata and isolate speculation test persistence.
- Require verified CLI + SDK archives in CI/release; document experimental limits.

# Changelog

All notable changes to OAK (Open Agent Kernel) will be documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Edge-Cloud Pipeline Harness — pluggable dual-path inference architecture
  - `IPromptEngine`: prompt compression + intent distillation before cloud dispatch
  - `ICloudBackend`: async cloud LLM client (OpenAI-compatible)
  - `IArbiter`: local arbitration (cloud_prefer / latency_first / confidence)
  - `IConfidenceScorer`: two-phase confidence gating (pre-score + post-score)
  - `PipelineHarness`: top-level orchestrator with component registry
- Configuration: `config/harness.yaml` for edge-cloud pipeline settings
- Prompt templates: `templates/` directory with default, navigation, vehicle_control
- Test suite: `test_harness` with 15 unit tests covering all harness components
- Design documentation: `docs/edge_cloud_design.md`

## [0.3.0] - 2026-08-21

### Added
- Agent Scheduler with priority queues (RealTime/Interactive/Batch/Idle)
- llama.cpp model runtime — connects to llama-server for on-device inference
- Genie/QNN model runtime stub for Qualcomm NPU backends
- Speculative Execution engine (LSTM intent predictor + HNSW cache)
- Formal Plan Verification (CTL* model checking + CDCL SAT solver)
- Agent Mesh networking (mDNS discovery + OR-Set CRDT + Merkle anti-entropy)
- On-Device Learning with DP-SGD privacy guarantees
- Constrained Decoding (GBNF grammar enforcement)
- DAG Orchestrator with WAL crash recovery
- CI pipeline (Ubuntu, macOS Intel/ARM)
- Apache 2.0 LICENSE
- SECURITY.md policy

### Changed
- Versioning reset to 0.x to reflect alpha status honestly

## [Unreleased]
- npm CLI package distribution
- End-to-end integration test with real GGUF model
- Android demo APK in GitHub releases
