# Changelog

All notable changes to OAK (Open Agent Kernel) will be documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

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
