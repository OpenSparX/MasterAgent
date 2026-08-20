# GitHub Issues to Create

## Issue #1: Roadmap — Open-source the kernel runtime

**Title:** [Roadmap] Open-source kernel runtime (orchestrator + WAL + dispatch)

**Body:**

## Goal

Make OAK fully self-contained as an open-source project by releasing the kernel runtime source.

## Current State

The strategic feature modules (speculative execution, formal verification, agent mesh, on-device learning, constrained decoding) are fully open. The kernel runtime that wires them together is currently proprietary.

## What "kernel runtime" includes

- [ ] Task Orchestrator (DAG execution with dependency resolution)
- [ ] WAL (Write-Ahead Log) crash recovery
- [ ] Agent Dispatch (control plane, route selector, preemption)
- [ ] Atomic Service (MCP wire protocol, tool registry)
- [ ] Inference Framework runtime (session management, seal validation)
- [ ] IPC transport layer

## Milestones

- [ ] v0.4: Extract kernel interfaces into standalone compilation unit
- [ ] v0.5: Implement minimal open-source orchestrator (single-process, no IPC)
- [ ] v0.6: Add WAL recovery to OSS orchestrator
- [ ] v1.0: Full parity — CLI builds end-to-end from source

## How to Contribute Now

Even without the kernel runtime, you can:
1. Improve strategic feature modules (better LSTM architecture, faster SAT solving)
2. Add new model runtime adapters (MLX, ONNX Runtime, TensorRT)
3. Write agent skill definitions
4. Improve documentation and examples
5. Port tests to more platforms

---

## Issue #2: CI — Add end-to-end test with real GGUF model

**Title:** [CI] End-to-end integration test with small GGUF model

**Body:**

Add a CI job that:
1. Downloads a small GGUF model (e.g., qwen2.5-0.5b-instruct, ~350MB Q4)
2. Starts llama-server
3. Runs a basic inference through the sparx runtime adapter
4. Verifies token output is non-empty and properly streamed

This proves the llama.cpp integration actually works end-to-end.

**Labels:** enhancement, ci, good first issue

---

## Issue #3: Add MLX model runtime adapter

**Title:** [Feature] MLX model runtime adapter for Apple Silicon

**Body:**

Apple's MLX framework provides efficient inference on Apple Silicon. Add an `MlxModelRuntime` adapter implementing `IModelRuntime` that:
- Loads MLX-format or GGUF models via mlx-lm
- Streams tokens through the existing `InferenceChunk` interface
- Reports hardware utilization (ANE/GPU/CPU split)

This would give macOS developers a native alternative to llama.cpp.

**Labels:** enhancement, good first issue, help wanted
