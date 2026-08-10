# System Overview

MasterAgent-v2 is a C++17 reference implementation for intelligent in-vehicle interaction. Starting from text input, it covers request normalization, short-term memory, deterministic skills, prompt assembly, two-stage mock inference, task orchestration, MCP atomic services, agent dispatch, result delivery, data logging, and exception governance.

## 1. Design Goals

- **Deterministic first**: when rules can close, the model is never invoked.
- **Model-controlled**: the model may only output a closed JSON protocol; it cannot directly execute capabilities.
- **Reliable side-effects**: commit, query, terminal, unknown, and reconciliation are separated.
- **Verifiable priority**: P0, P1, P2 share unified types and comparison rules.
- **Preemption-safe**: cooperative preemption only at safe points — threads are never killed, success is never forged.
- **Fail-closed recovery**: when side-effect confirmation is impossible, the system enters `Unknown` state; blind retry is forbidden.
- **Auditable**: critical events, exceptions, causal identity, and durability levels are fully traceable.

## 2. Default Execution Chain

```text
TextInput
  -> InteractionLayer
  -> AgentService
  -> Preprocess + Memory
  -> Skill / Prompt / Intent
  -> direct ASK|REPLY|FAIL
     or Task DAG -> Orchestrator
          -> AtomicService (MCP Tool)
          -> AgentDispatch (external sub-Agent boundary)
          -> InferenceFramework (Mock model)
  -> TurnResult
  -> DataLog + Exception
```

## 3. Core Modules

| Module | Responsibility |
| --- | --- |
| Interaction | Validate input; generate stable request, trace, and session turn |
| AgentService | Coordinate one user turn without owning downstream business state |
| Preprocess | Request validation, UTF-8 sanitization, parameter/time normalization, independent runtime capability discovery and on-demand query |
| Memory | Interface with the delivered short-term memory store |
| Skill | Deterministic intent and parameter closure |
| Prompt | Assemble controlled model inputs and summaries |
| Intent | Manage deterministic paths and up to two-stage model protocol |
| Orchestrator | Validate DAG, schedule nodes, aggregate plan state |
| AtomicService | MCP Tool registration, execution, idempotency, and reconciliation |
| AgentDispatch | Manage external sub-Agent dispatch contracts |
| Inference | Manage model jobs, KV cache, priority, and safe preemption |
| DataLog | Events, audit, trace, durability, and privacy boundaries |
| Exception | Exception normalization, grouping, escalation, recovery, and emergency loop |
| Runtime | Create, inject, start, and shut down modules |

## 4. Default Deployment

The default deliverable is a single process with one core library and one application entry point. Optional IPC code lives in `transport/ipc` and is excluded from the default build. Business interfaces are transport-agnostic, so adding process isolation later requires no changes to task, priority, or recovery semantics.
