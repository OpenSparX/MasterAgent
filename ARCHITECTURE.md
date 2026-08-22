# Architecture & Code Map

## Directory Structure

```
OAK/
├── include/master_agent/       # Public kernel API (stable interfaces)
│   ├── common/types.h          #   Shared types: Result<T>, Status, CallContext
│   ├── orchestrator/           #   DAG task orchestration + WAL recovery
│   ├── inference/              #   Model runtime interface (IModelRuntime)
│   ├── agent_dispatch/         #   Agent lifecycle management
│   ├── atomic_service/         #   MCP tool execution
│   └── kv_cache/               #   KV cache management
│
├── cli/                        # Developer CLI + Agent OS modules
│   ├── src/
│   │   ├── sparx_main.cpp             # CLI entry point
│   │   ├── cmd_*.cpp                  # CLI commands (init, run, demo, etc.)
│   │   │
│   │   ├── # ─── Agent OS Modules ───
│   │   ├── sparx_agent_scheduler.cpp  # OS-level process management (PCB, queues)
│   │   ├── sparx_context_manager.cpp  # Conversation context tracking
│   │   ├── sparx_memory_manager.cpp   # Short/long-term memory
│   │   ├── sparx_access_control.cpp   # Permission enforcement
│   │   ├── sparx_tool_registry.cpp    # MCP tool discovery + dispatch
│   │   ├── sparx_model_registry.cpp   # Model lifecycle management
│   │   │
│   │   ├── # ─── Strategic Features ───
│   │   ├── sparx_speculative.cpp      # LSTM intent predictor + HNSW cache
│   │   ├── sparx_transformer_predictor.cpp  # Transformer-based predictor
│   │   ├── sparx_formal_verify.cpp    # CDCL SAT solver + CTL* checker
│   │   ├── sparx_smt_backend.cpp      # SMT theory backend
│   │   ├── sparx_mesh.cpp             # mDNS + CRDT + Merkle anti-entropy
│   │   ├── sparx_delta_crdt.cpp       # Delta-state CRDT (OR-Set, LWW, GCounter)
│   │   ├── sparx_learning.cpp         # DP-SGD on-device fine-tuning
│   │   ├── sparx_constrained_decode.cpp  # GBNF grammar enforcement
│   │   ├── sparx_cloud_fusion.cpp     # Cloud/edge inference routing (legacy)
│   │   ├── sparx_trace.cpp            # Distributed tracing
│   │   │
│   │   ├── # ─── Edge-Cloud Harness (端云融合) ───
│   │   ├── sparx_pipeline_harness.cpp # Pluggable pipeline orchestrator
│   │   ├── sparx_prompt_engine.cpp    # Prompt compression + intent distillation
│   │   ├── sparx_cloud_backend.cpp    # Cloud LLM HTTP client (OpenAI compat)
│   │   ├── sparx_arbiter.cpp          # Local arbitration (cloud_prefer/latency/confidence)
│   │   ├── sparx_confidence_scorer.cpp # Confidence-gated routing
│   │   │
│   │   ├── # ─── Model Runtime Adapters ───
│   │   ├── llama_cpp_model_runtime.cpp   # llama-server HTTP adapter
│   │   └── genie_model_runtime.cpp       # Qualcomm QNN/GenieX adapter
│   │
│   └── include/                # Internal headers for above modules
│
├── android/                    # Android demo APK (Kotlin)
│   └── app/src/main/
│       ├── java/               # Inference service, model download
│       └── res/                # UI layouts
│
├── tests/                      # Unit + integration tests
│   ├── test_integration_speculation.cpp
│   ├── test_orset.cpp
│   ├── test_merkle.cpp
│   ├── test_embedding.cpp
│   └── bench_strategic.cpp
│
├── eval/                       # Benchmarks per module
│   ├── speculation/
│   ├── formal/
│   ├── mesh/
│   ├── learning/
│   └── constrained/
│
├── examples/                   # Example agent configurations
│   ├── automotive_assistant/
│   ├── smart_home/
│   └── iot_edge/
│
├── docs/                       # Documentation
├── scripts/                    # Build/release scripts
├── third_party/                # Vendored dependencies
│   ├── nlohmann/               # JSON library
│   └── memory_short_term/      # L1 memory module
│
├── CMakeLists.txt              # Root build (C++17, CMake 3.18+)
├── VERSION.json                # Project metadata
├── LICENSE                     # Apache 2.0
├── CHANGELOG.md
├── CONTRIBUTING.md
├── CODE_OF_CONDUCT.md
├── SECURITY.md
└── .github/
    ├── workflows/ci.yml        # Multi-platform CI (Ubuntu, macOS Intel/ARM)
    └── ISSUE_TEMPLATE/
```

## Module Dependency Graph

```
                    ┌─────────────────────┐
                    │   CLI Commands      │
                    │   (cmd_*.cpp)       │
                    └──────────┬──────────┘
                               │ uses
              ┌────────────────┼────────────────┐
              │                │                │
              ▼                ▼                ▼
     ┌─────────────┐  ┌──────────────┐  ┌───────────────┐
     │ Agent OS    │  │ Strategic    │  │ Model Runtime │
     │ Modules     │  │ Features     │  │ Adapters      │
     │             │  │              │  │               │
     │ • Scheduler │  │ • Speculative│  │ • llama.cpp   │
     │ • Context   │  │ • Formal     │  │ • Genie/QNN   │
     │ • Memory    │  │ • Mesh/CRDT  │  │               │
     │ • Access    │  │ • Learning   │  └───────┬───────┘
     │ • Tools     │  │ • Constrained│          │
     └──────┬──────┘  └──────┬───────┘          │
            │                │                   │
            ▼                ▼                   ▼
     ┌──────────────────────────────────────────────────┐
     │         Kernel API (include/master_agent/)        │
     │         IOrchestrator, IModelRuntime, types       │
     └──────────────────────────────────────────────────┘

     ┌──────────────────────────────────────────────────┐
     │     Edge-Cloud Pipeline Harness (harness/)        │
     │                                                   │
     │  IPromptEngine ─→ ICloudBackend                  │
     │       │                  │                        │
     │       ▼                  ▼                        │
     │  IConfidenceScorer ──→ IArbiter ──→ Output       │
     │       ▲                                          │
     │       │                                          │
     │  ILocalInference (wraps Model Runtime)           │
     └──────────────────────────────────────────────────┘
```

## Build Targets

| Target | What it builds | OSS? |
|:---|:---|:---:|
| `bench_strategic` | Performance benchmarks | ✅ |
| `test_*` | Unit/integration tests | ✅ |
| `eval_*` | Module evaluation harnesses | ✅ |
| `sparx` | Developer CLI (full Agent OS) | ❌ Needs kernel |
| `master_agent` | Embedded runtime library | ❌ Needs kernel |

## Namespaces

- `master_agent::` — Kernel API types and interfaces
- `master_agent::inference::` — Model runtime interfaces
- `sparx::` — CLI features and strategic modules
- `sparx::os::` — Agent scheduler, process management
- `sparx::mesh::` — Agent Mesh networking
- `sparx::verify::` — Formal verification
