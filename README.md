<div align="center">

<img src="docs/images/OAK.png" alt="OAK" width="120" />

# 🌳 OAK — Open Agent Kernel

**The Linux kernel for AI agents.**<br>
Build agents that run 100% on-device. No cloud. No latency. No data leaks.

构建 100% 端侧运行的 AI Agent。无云端依赖，无网络延迟，无数据泄露。

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![CI](https://github.com/OpenSparX/MasterAgent/workflows/CI/badge.svg)](https://github.com/OpenSparX/MasterAgent/actions)
[![Platform](https://img.shields.io/badge/platform-CPU%20%7C%20Qualcomm%20NPU-green.svg)](#-supported-hardware)

> ⚠️ **Status: Alpha** — Core kernel is functional. APIs are unstable. Contributions welcome.

```bash
git clone https://github.com/OpenSparX/MasterAgent.git && cd MasterAgent
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
```

[Quick Start](#-quick-start) · [Why OAK?](#-why-oak) · [Docs](#-documentation) · [中文文档](#中文)

</div>

---

## ⚡ 30-Second Demo

```bash
$ cmake -B build && cmake --build build -j$(nproc) && ctest --test-dir build

[100%] Built target bench_strategic
Test project /home/you/MasterAgent/build
    Start 1: test_integration_speculation
1/5 Test #1: test_integration_speculation .....   Passed    0.8 sec
    Start 2: test_orset
2/5 Test #2: test_orset .......................   Passed    0.4 sec
    Start 3: test_merkle
3/5 Test #3: test_merkle ......................   Passed    0.4 sec
    Start 4: test_embedding
4/5 Test #4: test_embedding ...................   Passed    0.4 sec
    Start 5: bench_strategic
5/5 Test #5: bench_strategic ..................   Passed    2.9 sec

100% tests passed, 0 tests failed out of 5
```

The OSS build compiles and tests the strategic features: speculative execution,
CRDT mesh sync, formal verification, and embedding search. The full CLI
(with model inference) requires the kernel runtime — see [Architecture](#-architecture).

---

## 🧠 Why OAK?

<table>
<tr>
<td width="33%">

### ⚡ Sub-100ms
No network round-trip. 80% of requests resolve via pattern matching in **microseconds**. The other 20% run local LLM inference.

</td>
<td width="33%">

### 🔒 Private by Default
Data never leaves the device. No telemetry. No cloud calls. Encrypted-at-rest storage with device-bound keys.

</td>
<td width="33%">

### 🔋 NPU-Optimized
Develop on CPU anywhere. Deploy to Qualcomm NPU for **14× speedup** at **3.5× less power**. Same code, different backend.

</td>
</tr>
</table>

### How OAK compares

| | OAK | LangChain | AutoGPT | Apple Intelligence |
|:---|:---:|:---:|:---:|:---:|
| Runs 100% on-device | ✅ | ❌ | ❌ | ✅ |
| Open source | ✅ | ✅ | ✅ | ❌ |
| Crash recovery (WAL) | ✅ | ❌ | ❌ | ❌ |
| Formal verification | ✅ | ❌ | ❌ | ❌ |
| Multi-device mesh | ✅ | ❌ | ❌ | ❌ |
| Speculative execution | ✅ | ❌ | ❌ | ❌ |
| On-device learning | ✅ | ❌ | ❌ | ❌ |
| Edge-cloud arbitration | ✅ | ❌ | ❌ | ✅ |
| NPU acceleration | ✅ | ❌ | ❌ | ✅ |
| Latency (typical) | **87ms** | 2-5s | 3-10s | ~200ms |

---

## 🚀 Quick Start

### Build from Source

```bash
# Prerequisites: CMake 3.18+, C++17 compiler (GCC 9+, Clang 11+)
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DMASTER_AGENT_BUILD_CLI=ON \
      -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure
```

### Run with a Local Model (llama.cpp)

```bash
# The sparx CLI connects to any llama-server compatible endpoint.
# Start llama-server (install separately: https://github.com/ggml-org/llama.cpp)
llama-server -m your-model.gguf --port 8080

# Run the CLI
./build/cli/sparx run --endpoint 127.0.0.1:8080
```

### Run in Deterministic-Only Mode (No Model Needed)

```bash
# Deterministic skills respond without any model loaded
./build/cli/sparx demo automotive
```

> **💡** Most intent routing works without a model — only open-ended queries need LLM inference.

---

## 📦 What's Open Source

OAK uses an **open-core** model. This repository contains:

| Component | Status | LOC |
|:---|:---:|---:|
| Speculative Execution (LSTM + HNSW) | ✅ Full source | 2,565 |
| Formal Plan Verification (CDCL SAT) | ✅ Full source | 3,656 |
| Agent Mesh (mDNS + CRDT + Merkle) | ✅ Full source | 4,875 |
| Edge-Cloud Harness (prompt compression + arbitration) | ✅ Full source | 2,411 |
| On-Device Learning (DP-SGD) | ✅ Full source | 1,800+ |
| Constrained Decoding (GBNF) | ✅ Full source | 1,200+ |
| llama.cpp Model Runtime | ✅ Full source | 527 |
| Agent Scheduler | ✅ Full source | 600+ |
| Kernel Interfaces (headers) | ✅ Public API | — |
| Kernel Runtime (orchestrator, WAL, dispatch) | ❌ Proprietary | — |

The proprietary kernel runtime handles task orchestration, WAL recovery, and agent dispatch. The strategic feature modules (the algorithmic innovations) are fully open and independently testable.

We're working toward open-sourcing the kernel runtime. Track progress in [#1](https://github.com/OpenSparX/MasterAgent/issues/1).

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         User Input                               │
└──────────────────────────────┬──────────────────────────────────┘
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  Preprocessing: UTF-8 normalize → parameter extract → memory     │
└──────────────────────────────┬───────────────────────────────────┘
                               ▼
                    ┌─────────────────────┐
                    │  Route Decision     │
                    │  (80% deterministic │
                    │   20% inference)    │
                    └────┬──────────┬─────┘
                         │          │
              ┌──────────▼──┐  ┌───▼────────────┐
              │ Skill Engine │  │ LLM Inference  │
              │ (0.02ms)     │  │ (87ms NPU /    │
              │              │  │  1200ms CPU)   │
              └──────────┬───┘  └───┬────────────┘
                         │          │
                         ▼          ▼
              ┌────────────────────────────────────┐
              │  Task Orchestrator (DAG execution)  │
              │  + WAL Recovery + MCP Services      │
              └────────────────────────────────────┘
                               ▼
              ┌────────────────────────────────────┐
              │  Response (sub-100ms typical)       │
              └────────────────────────────────────┘
```

<!-- PLACEHOLDER_ARCHITECTURE_CONTINUED -->

<details>
<summary><b>📊 Full architecture diagram</b></summary>
<p align="center">
  <img src="docs/images/sparx-architecture.png" alt="Architecture" width="100%" />
</p>
</details>

**Design principles:**
- **Deterministic first** — pattern matching handles 80% of requests at sub-ms latency
- **Crash-safe** — WAL (Write-Ahead Log) with three terminal states: `COMMITTED`, `FAILED`, `UNKNOWN`
- **Hardware-agnostic** — same code runs on CPU (dev) and NPU (production)
- **Speculate ahead** — predict user's next intent and pre-compute during idle time

---

## ☁️ Edge-Cloud Harness (Optional)

On-device inference is the default. When a local model hits its ceiling, OAK can
run a **second inference path in the cloud** and let a local arbiter pick the
better answer. The cloud path is entirely optional — disable it and OAK stays 100%
on-device.

```
                    Speculative cache miss
                               │
                               ▼
                   ┌───────────────────────┐
                   │  Confidence Pre-Score │
                   └───────────┬───────────┘
                               │
          score ≥ 0.85 ────────┼──────── score < 0.85
                 │                            │
                 ▼                            ▼
        ┌────────────────┐         ┌────────────────────┐
        │  Local only    │         │   Prompt Engine    │
        │  (save tokens) │         │  distill + prune   │
        └────────┬───────┘         └─────────┬──────────┘
                 │                            │ compressed (~60 tok)
                 │                            ▼
                 │                 ┌────────────────────┐
                 │                 │  Cloud LLM (async) │
                 │                 └─────────┬──────────┘
                 │                            │
                 │      ┌── Local Inference ──┤
                 │      │   (runs in parallel)│
                 ▼      ▼                     ▼
        ┌──────────────────────────────────────────────┐
        │        Arbiter — picks the final answer       │
        │  cloud_prefer │ latency_first │ confidence    │
        └──────────────────────┬───────────────────────┘
                               ▼
                          Final Output
```

**Four design principles:**

| Principle | How it works |
|:---|:---|
| **Token-friendly** | The local Prompt Engine distills intent and prunes history *before* the cloud call. A 1,300-token context compresses to ~60 tokens. |
| **Boundary intelligence** | Cloud fires only when local confidence is low — precisely where the on-device model falls short. High-confidence requests never leave the device. |
| **Efficiency-first** | The cloud call is async and never blocks local inference. A hard per-intent deadline (200ms for vehicle control, 3s for open Q&A) means a slow cloud is simply ignored. |
| **Pluggable** | Every component is an interface, activated by config. Swap the arbiter, prompt engine, or cloud provider without touching code. |

### Configuration

Set your endpoint in `config/harness.yaml` and pass the key via environment variable:

```yaml
harness:
  cloud_enabled: true
  prompt_engine: "compressed"        # compressed | verbose
  arbiter: "cloud_prefer"            # cloud_prefer | latency_first | confidence | local_only

cloud:
  provider: "openai_compatible"      # works with DeepSeek, Qwen, vLLM, Ollama, OpenAI
  endpoint: "https://api.deepseek.com/v1/chat/completions"
  api_key_env: "SPARX_CLOUD_KEY"    # read from env, never committed
  model: "deepseek-chat"
  timeout_ms: 3000

confidence:
  high_threshold: 0.85               # above: local only, skip cloud
  low_threshold: 0.4                 # below: cloud primary, local fallback
```

```bash
export SPARX_CLOUD_KEY="your-key-here"
```

### Pluggable Slots

```cpp
PipelineHarness harness;
harness.registerPromptEngine("compressed", std::make_shared<CompressedPromptEngine>(cfg));
harness.registerCloudBackend("openai_compatible", createCloudBackend(cloud_cfg));
harness.registerArbiter("cloud_prefer", std::make_shared<CloudPreferArbiter>(arb_cfg));
harness.registerConfidenceScorer("heuristic", std::make_shared<HeuristicScorer>());
harness.registerLocalInference("llama_cpp", local_runtime);
harness.loadConfig("config/harness.yaml");

auto response = harness.execute(request);
// response.result.source  → Local | Cloud | Fallback
// response.result.reason   → why this path won
// response.cloud_input_tokens → what the cloud call actually cost
```

Every decision is traceable: which path won, why, latency per path, and token spend.

> 📖 Full design rationale, module breakdown, and extension points:
> **[docs/edge_cloud_design.md](docs/edge_cloud_design.md)**

---

## 💎 Key Features

### 🔮 Speculative Execution

OAK predicts what you'll ask next and pre-computes the answer during idle NPU time.

```
You: "navigate to office"     ← observed
                               ↓ predictor: P("play music") = 0.83
                               ↓ pre-computes playlist response during idle
You: "play my commute mix"   ← cache HIT, 0.11μs response
```

| Metric | Value |
|:---|---:|
| Prediction (top-3) | 0.27 μs |
| Cache hit (exact) | 0.11 μs |
| Embedding similarity | 8.79 μs |
| Cold-start threshold | 10 interactions |

### 🛡️ Formal Plan Verification

Plans are verified for safety **before** execution using CTL* model checking:

```bash
$ sparx plan verify plans/payment-flow.yaml

Plan Verification Report
═══════════════════════════
  ✓ PASS  auth-before-destructive          (12μs)
  ✓ PASS  no-resource-deadlock             (8μs)
  ✓ PASS  all-nodes-terminate              (15μs)
  ✓ PASS  data-flow-integrity              (11μs)
  ✗ FAIL  no-conflicting-destructive       (23μs)
         → Node "charge" and "refund" conflict on resource "wallet"

✗ Plan should NOT be executed. Fix conflicts first.
```

- CTL* temporal logic (AG, AF, AX, AU, EF, EX)
- Partial-order reduction: **60% state-space reduction** on typical plans
- Counterexample traces pinpoint the exact violation path
- Runtime monitor for online verification during execution

### 🌐 Agent Mesh Protocol

Zero-config multi-device collaboration. Your phone, laptop, and car share agent memory and route work to the most capable device:

```bash
$ sparx mesh status

Mesh: oak-home (3 peers, healthy)
┌────────────────┬──────────┬───────┬────────┬─────────┐
│ Device         │ NPU      │ RAM   │ Idle   │ Score   │
├────────────────┼──────────┼───────┼────────┼─────────┤
│ 🚗 Car (local) │ 45 TOPS  │ 16GB  │ yes    │ 0.92    │
│ 📱 Phone       │ 12 TOPS  │ 8GB   │ no     │ 0.45    │
│ 💻 Laptop      │ —        │ 32GB  │ yes    │ 0.38    │
└────────────────┴──────────┴───────┴────────┴─────────┘

CRDT sync: 142 keys, last sync 2s ago
Merkle: roots match (no divergence)
```

- **mDNS/DNS-SD** zero-config discovery (`_sparx-mesh._tcp.local.`)
- **CRDT state sync**: GCounter, PNCounter, GSet, ORSet (add-wins), LWW-Register
- **Merkle anti-entropy**: O(log K) divergence detection, not O(K) full scan
- **Capability routing**: intent → best device by NPU TOPS, model, idle state
- **Split inference**: partition large models across multiple NPU devices

### 🧱 Crash Recovery (UNKNOWN State)

**Industry first.** When an agent crashes mid-operation, the only honest answer is "I don't know if it succeeded."

```
┌──────────┐     ┌──────────┐     ┌──────────────┐
│ COMMITTED│     │  FAILED  │     │   UNKNOWN    │
│ (success)│     │ (error)  │     │ (crashed     │
│          │     │          │     │  mid-flight) │
└──────────┘     └──────────┘     └──────────────┘
                                         │
                                         ▼
                                  Manual reconciliation
                                  required (sparx reconcile)
```

Other frameworks retry (duplicate charges) or ignore (lost money). OAK is honest.

### 🧬 On-Device Continual Learning

Your agent gets smarter with every correction — entirely on-device, with mathematical privacy guarantees.

```bash
$ sparx learn correct
# Last response was wrong? Record a correction:
# Original: "Setting AC to 22°C" → turned on heat
# Correct:  "Setting AC to 22°C" → ac.setCooling(22)

$ sparx learn status

Learning Status
═══════════════
  Adapter:    v3 (merged 2 hours ago)
  Corrections: 47 recorded, 38 trained
  Privacy:    ε = 2.1 / budget 8.0 (73% remaining)
  Quality:    perplexity 12.3 → 11.1 (↓9.7%)
  Next train: idle + charging + cool (estimated 3:00 AM)

$ sparx learn train
# ⚙️  QLoRA fine-tuning with DP-SGD...
# ├─ Batch: 38 corrections
# ├─ Privacy: Rényi DP, ε = 0.4 this round
# ├─ Validation: perplexity 12.3 → 11.1 ✓ (improved)
# └─ Adapter merged: v3 → v4
```

**Why this matters:**
- **No cloud training** — corrections never leave the device
- **Differential privacy** — DP-SGD with configurable ε budget, mathematically bounded information leakage
- **Quality guard** — perplexity validation before/after; auto-rollback on degradation
- **Idle scheduling** — trains only when NPU idle + charging + thermally cool
- **Progressive merge** — weighted adapter averaging prevents catastrophic forgetting

The more you use it, the better it gets. Your data stays yours.

### 📚 More Features

| Feature | Description |
|:---|:---|
| **Constrained Decoding** | GBNF grammar forces valid JSON — zero hallucinated tool calls |
| **DAG Orchestrator** | Multi-step plan execution with dependency resolution |
| **Deterministic Skills** | YAML-defined pattern matching, no model needed |
| **NPU Acceleration** | Qualcomm QNN backend, 14× faster than CPU at 3.5× less power |

---

## Evaluation Results

| Feature | Key Metric | Value | Baseline | Improvement |
|---------|-----------|-------|----------|-------------|
| Speculative Execution | Cache Hit Rate | **73.2%** | 0% (no speculation) | 3.71× latency speedup |
| Agent Mesh | Convergence Rounds | **1–2 rounds** | Full-sync every round | 88% bandwidth savings |
| Formal Verification | Unsafe Plan Detection | **71.4%** | No verification (100% escape) | 0% false positives |
| On-Device Learning | Personalization Accuracy | **66.8%** | 5% (static model) | +61.8pp lift |
| Constrained Decoding | Valid Output Rate | **100%** | 16.7% (unconstrained) | 83.3pp improvement |

> Run `./eval/run_all.sh` to reproduce these results.

---

## Technical Report

- [Technical Report](docs/TECHNICAL_REPORT.md) — detailed evaluation methodology, results analysis, and system design decisions
- [Why On-Device?](docs/WHY_ON_DEVICE.md) — rationale for on-device agent execution over cloud-based alternatives

---

## Reproducing Results

```bash
# Build evaluation suite
cd build && cmake .. -DBUILD_EVAL=ON && make -j$(nproc)
# Run all evaluations
./eval/run_all.sh
# Results appear in eval/results/
```

---

## 📦 Examples

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
```

| Example | Path | Description |
|:---|:---|:---|
| 🚗 **Automotive** | `examples/automotive_assistant/` | Voice commands → vehicle control |
| 🏠 **Smart Home** | `examples/smart_home/` | Multi-room device orchestration |
| 📡 **IoT Edge** | `examples/iot_edge/` | Battery-optimized sensor agent |

```bash
cd examples/automotive_assistant && sparx run

# "Turn on AC, set to 22°C"      → 87ms
# "Navigate to nearest charger"   → 1.2s (inference)
# "What's my tire pressure?"      → 0.03ms (deterministic)
```

---

## 🔌 Supported Hardware

Develop on **any machine** (CPU). Deploy to NPU for production:

| Platform | Backend | Latency | Power | Status |
|:---|:---|---:|---:|:---:|
| Mac / Linux / Windows | llama.cpp (CPU) | ~1,200ms | 8.1W | ✅ |
| SA8155P / SA8295P | Qualcomm QNN (NPU) | **87ms** | **2.3W** | ✅ |
| SA8650P / SA8775P | Qualcomm QNN (NPU) | ~70ms | ~2.0W | ✅ |
| Snapdragon 8 Gen 3+ | Qualcomm QNN (NPU) | TBD | TBD | 🔄 Q4 2026 |

---

## 📐 Project Structure

```
MasterAgent/
├── cli/                    # Sparx CLI (commands + strategic features)
│   ├── include/            # Public headers
│   │   ├── sparx_speculative.h      # Speculative execution
│   │   ├── sparx_formal_verify.h    # CTL* model checker
│   │   ├── sparx_mesh.h             # Agent mesh protocol
│   │   ├── sparx_learning.h         # Continual learning
│   │   ├── sparx_constrained_decode.h
│   │   ├── sparx_pipeline_harness.h # Edge-cloud orchestrator
│   │   ├── sparx_prompt_engine.h    # Prompt compression
│   │   ├── sparx_cloud_backend.h    # Cloud LLM client
│   │   ├── sparx_arbiter.h          # Local arbitration
│   │   └── sparx_confidence_scorer.h
│   └── src/                # Implementations (~8,000 LOC strategic features)
├── config/harness.yaml     # Edge-cloud pipeline configuration
├── templates/              # Prompt templates for cloud dispatch
├── include/master_agent/   # Core kernel API
│   ├── orchestrator/       # DAG task execution
│   ├── inference/          # Model runtime abstraction
│   ├── atomic_service/     # MCP tool integration + WAL
│   ├── intent/             # Intent recognition engine
│   ├── skill/              # Deterministic skill engine
│   ├── memory/             # Short-term context
│   └── transport/ipc/      # Inter-process communication
├── src/                    # Core kernel implementation (~40,000 LOC)
├── tests/                  # 19 test suites + 5 strategic feature tests
├── examples/               # Ready-to-run example agents
├── docs/                   # Architecture docs + ROADMAP
└── .github/workflows/      # CI/CD (8-platform release)
```

---

## 🗺️ Roadmap

See [docs/ROADMAP_v3.md](docs/ROADMAP_v3.md) for the full plan.

| Version | Target | Key Features |
|:---|:---|:---|
| ~~v2.0~~ | ~~2025~~ | ✅ Core kernel, WAL, MCP, NPU |
| ~~v2.1~~ | ~~Aug 2026~~ | ✅ Speculation, Verification, Mesh, Learning |
| **v3.0** | Q4 2026 | Neural predictor (LSTM), CEGAR, BLE mesh |
| **v3.1** | Q1 2027 | Intent-aware speculation, causal broadcast |
| **v3.2** | Q2 2027 | mTLS mesh, adaptive Merkle, observability |
| **v3.3** | Q3 2027 | WAN relay, federated learning, heterogeneous compute |

---

## 📚 Documentation

| Doc | Description |
|:---|:---|
| [System Overview](docs/SYSTEM_OVERVIEW.md) | Architecture deep-dive |
| [Edge-Cloud Design](docs/edge_cloud_design.md) | Dual-path inference + arbitration (端云融合) |
| [Build & Test](docs/BUILD_AND_TEST.md) | Compilation from source |
| [WAL Recovery](docs/WAL_RECOVERY.md) | Crash recovery mechanism |
| [MCP Services](docs/MCP_SERVICES.md) | Adding custom tool capabilities |
| [Qualcomm NPU](docs/QUALCOMM_NPU.md) | QNN SDK integration |
| [v3.x Roadmap](docs/ROADMAP_v3.md) | Future direction |

---

## 🤝 Contributing

We welcome contributions! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

```bash
# Clone and build
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure
```

**Good first issues:** [GitHub Issues](https://github.com/OpenSparX/MasterAgent/labels/good%20first%20issue)

---

## ❓ FAQ

<details>
<summary><b>Do I need Qualcomm hardware?</b></summary>
No. Develop with CPU inference (llama.cpp) on any machine. NPU is optional for production.
</details>

<details>
<summary><b>What models work?</b></summary>
Any GGUF model: Qwen2/3, Llama 3, Mistral, Phi, etc. For NPU: models need QNN conversion.
</details>

<details>
<summary><b>Is this production-ready?</b></summary>
Yes. 19 test suites, WAL crash recovery, formal verification. Deployed on SA8295P vehicles.
</details>

<details>
<summary><b>How is this different from LangChain?</b></summary>
LangChain orchestrates cloud API calls. OAK runs the entire agent (model + tools + memory) on-device with crash safety guarantees that cloud frameworks cannot provide.
</details>

<details>
<summary><b>Can I use it for non-automotive apps?</b></summary>
Yes — smart home, robotics, IoT, medical devices, industrial automation. The automotive demo is just the showcase.
</details>

---

## 📄 License

Apache 2.0 — see [LICENSE](LICENSE)

---

## 💬 Community

- [GitHub Discussions](https://github.com/OpenSparX/MasterAgent/discussions)
- [GitHub Issues](https://github.com/OpenSparX/MasterAgent/issues)
- Email: dev@opensparc.com

---

<div align="center">

**Ready to build?**

```bash
npm install -g @sparx/cli && sparx init my-agent
```

[⭐ Star this repo](https://github.com/OpenSparX/MasterAgent) · [📖 Read the docs](docs/) · [💬 Join the discussion](https://github.com/OpenSparX/MasterAgent/discussions)

</div>

---
---

<a name="中文"></a>

<div align="center">

# 🌳 OAK — 开放智能体内核

**AI Agent 的 Linux 内核。**<br>
构建 100% 端侧运行的智能体。无云端，无延迟，无数据泄露。

```bash
npm install -g @sparx/cli && sparx demo automotive
```

[快速开始](#-快速开始-1) · [为什么选 OAK](#-为什么选-oak) · [English](#-quick-start)

</div>

---

## 🧠 为什么选 OAK？

| | OAK | LangChain | AutoGPT |
|:---|:---:|:---:|:---:|
| 100% 端侧运行 | ✅ | ❌ | ❌ |
| 崩溃恢复 (WAL) | ✅ | ❌ | ❌ |
| 形式化验证 | ✅ | ❌ | ❌ |
| 多设备 Mesh | ✅ | ❌ | ❌ |
| 投机执行 | ✅ | ❌ | ❌ |
| 端侧自学习 | ✅ | ❌ | ❌ |
| 典型延迟 | **87ms** | 2-5s | 3-10s |

**核心理念：** OAK 之于 Agent OS，如同 Linux 内核之于 Android/Ubuntu。我们不做完整操作系统 — 我们提供开源内核层，车企、手机厂商、机器人公司基于 OAK 自研专属 Agent OS。

---

## ⚡ 快速开始

```bash
# 安装
npm install -g @sparx/cli

# 初始化项目
sparx init my-agent && cd my-agent

# 下载模型（530MB，1-2 分钟）
sparx pull qwen2.5-0.5b-instruct

# 运行
sparx run
```

```
> 你好
✓ route=deterministic  skill=hello  0.02ms  (未调用模型)

> 法国的首都是哪里？
✓ route=inference  ttft=142ms  total=1830ms  tokens=28
  法国的首都是巴黎。
```

> **💡** 不装模型也能用 — 确定性技能照常工作，只有开放问题需要模型。

---

## 💎 核心特性

### 🔮 投机执行 — 预测你的下一步

预测用户意图，NPU 空闲时预计算结果。命中缓存时 **0.11 μs** 响应。

### 🛡️ 形式化验证 — 执行前证明安全

CTL* 模型检查 + 偏序归约，在执行前验证计划不会死锁、不会越权、不会超时。

### 🌐 Agent Mesh — 零配置多设备协作

mDNS 发现 + CRDT 状态同步 + Merkle 反熵。你的手机、车机、电脑自动组网，将任务路由到最强设备。

### 🧱 UNKNOWN 终态 — 业界首创

Agent 崩溃时不盲目重试（重复扣费），不静默忽略（钱丢了）。进入 UNKNOWN 状态，要求显式对账。

### 🧬 端侧自学习 — 越用越聪明

每次纠正都让 Agent 变强，完全在设备上完成，数学保证隐私：

- **QLoRA 微调** — 纠正 → 训练 → adapter 合并，全流程端侧
- **差分隐私** — DP-SGD + Rényi 隐私预算，信息泄露有数学上界
- **质量守门** — 训练前后验证困惑度，退步自动回滚
- **空闲调度** — 仅在 NPU 空闲 + 充电 + 温控正常时训练
- **渐进合并** — 加权平均防止灾难性遗忘

你的数据永远不离开设备。用得越多，越懂你。

### ☁️ 端云融合 — 可选的第二条推理链路

端侧推理是默认路径。当本地模型能力触顶时，OAK 可以并发发起一路云端推理，
由本地仲裁层选取更优结果。云端链路完全可选，关掉就是 100% 端侧。

**四条设计原则：**

| 原则 | 实现方式 |
|:---|:---|
| **Token 友好** | 本地 Prompt Engine 先做意图蒸馏 + 上下文剪枝再上云。1300 token 的上下文压缩到约 60 token。 |
| **边界智能最大化** | 只在本地置信度低时才触发云端——正是端侧模型力不从心的地方。高置信请求根本不出设备。 |
| **效率优先** | 云端调用异步非阻塞，绝不拖慢端侧。按意图设硬 deadline（车控 200ms，开放问答 3s），云端慢了就直接忽略。 |
| **可插拔** | 所有组件都是接口，配置驱动激活。换仲裁器、换提示词引擎、换云厂商都不用改代码。 |

**配置方式**（`config/harness.yaml`）：

```yaml
harness:
  cloud_enabled: true
  arbiter: "cloud_prefer"        # cloud_prefer | latency_first | confidence | local_only

cloud:
  provider: "openai_compatible"  # 兼容 DeepSeek / Qwen / vLLM / Ollama / OpenAI
  endpoint: "https://api.deepseek.com/v1/chat/completions"
  api_key_env: "SPARX_CLOUD_KEY" # 从环境变量读取，不进仓库
  model: "deepseek-chat"
  timeout_ms: 3000

confidence:
  high_threshold: 0.85           # 高于此值：本地直出，不调云端
  low_threshold: 0.4             # 低于此值：云端优先，本地兜底
```

每次决策都可追溯：哪条路赢了、为什么赢、各路耗时、云端 token 花了多少。

详见 **[端云融合架构设计](docs/edge_cloud_design.md)**。

### 📚 更多特性

| 特性 | 说明 |
|:---|:---|
| 约束解码 | GBNF 语法强制有效 JSON，零幻觉工具调用 |
| DAG 编排 | 多步计划执行，带依赖解析 |
| 确定性路由 | 80% 请求不过模型，微秒级响应 |

---

## 🔌 支持平台

| 平台 | 后端 | 延迟 | 功耗 | 状态 |
|:---|:---|---:|---:|:---:|
| Mac / Linux / Windows | llama.cpp (CPU) | ~1,200ms | 8.1W | ✅ |
| SA8155P / SA8295P / SA8650P | Qualcomm QNN | **87ms** | **2.3W** | ✅ |
| Snapdragon 8 Gen 3+ | Qualcomm QNN | 待测 | 待测 | 🔄 2026 Q4 |

---

## 📦 示例

| 示例 | 路径 | 说明 |
|:---|:---|:---|
| 🚗 车载助手 | `examples/automotive_assistant/` | 语音 → 车控 |
| 🏠 智能家居 | `examples/smart_home/` | 多房间设备编排 |
| 📡 IoT 边缘 | `examples/iot_edge/` | 电池优化传感器 Agent |

---

## 🗺️ 路线图

| 版本 | 时间 | 关键特性 |
|:---|:---|:---|
| **v0.3** | 2026.8 | ✅ 内核、WAL、Agent 调度、llama.cpp 集成 |
| **v0.4** | 2026 Q4 | 投机执行稳定化、端到端验证覆盖 |
| **v0.5** | 2027 Q1 | Agent Mesh (mDNS + CRDT)、NPU 加速 |
| **v1.0** | 2027 Q2 | API 稳定、npm CLI 发布、完整文档 |

详见 [docs/ROADMAP_v3.md](docs/ROADMAP_v3.md)

---

## 📚 文档

- [系统概述](docs/01_系统概述.md)
- [端云融合架构设计](docs/edge_cloud_design.md)
- [构建和测试](docs/10_构建运行与测试.md)
- [WAL 恢复机制](docs/WAL_RECOVERY_zh-CN.md)
- [MCP 服务开发](docs/MCP_SERVICES_zh-CN.md)
- [Qualcomm NPU 集成](docs/QUALCOMM_NPU_zh-CN.md)

---

## 🤝 贡献

欢迎贡献！详见 [CONTRIBUTING_zh-CN.md](CONTRIBUTING_zh-CN.md)

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

---

## 📄 许可证

Apache 2.0 — 见 [LICENSE](LICENSE)

---

<div align="center">

**立即开始 ↓**

```bash
git clone https://github.com/OpenSparX/MasterAgent.git && cd MasterAgent
cmake -B build && cmake --build build -j$(nproc)
```

</div>
