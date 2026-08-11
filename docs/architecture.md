# Sparx Architecture

> 中文版见下方 / Chinese version below

This document describes the internal architecture of Sparx at the level a
contributor or integrator needs. The README has a simplified overview; this is
the real map.

---

## System Layers

```mermaid
graph TD
    subgraph CLI["CLI Layer (cli/)"]
        sparx_main["sparx main"]
        cmd_run["cmd_run"]
        cmd_deploy["cmd_deploy"]
        cmd_doctor["cmd_doctor"]
    end

    subgraph Runtimes["Runtime Adapters (cli/src/)"]
        llama["LlamaCppModelRuntime"]
        genie["GenieModelRuntime"]
    end

    subgraph Kernel["Kernel (include/master_agent/, src/)"]
        interaction["InteractionLayer"]
        preprocess["PreprocessEngine"]
        memory["MemoryService"]
        skill["SkillEngine"]
        intent["IntentEngine"]
        inference["InferenceFramework"]
        orchestrator["Orchestrator"]
        atomic["AtomicService / MCP"]
        dispatch["AgentDispatch"]
        wal["WAL + Recovery"]
    end

    subgraph Hardware["Hardware"]
        cpu["CPU (any host)"]
        npu["Qualcomm NPU (SA8155P/8295P/8650P/8775P)"]
    end

    sparx_main --> cmd_run
    sparx_main --> cmd_deploy
    sparx_main --> cmd_doctor
    cmd_run --> llama
    cmd_deploy --> genie
    llama --> inference
    genie --> inference
    inference --> orchestrator
    orchestrator --> atomic
    orchestrator --> wal
    interaction --> preprocess
    preprocess --> memory
    memory --> skill
    skill --> intent
    intent --> inference
    dispatch --> orchestrator
    llama --> cpu
    genie --> npu
```

---

## Request Flow (single turn)

```mermaid
sequenceDiagram
    participant U as User
    participant R as REPL (cmd_run)
    participant S as SkillEngine
    participant I as IntentEngine
    participant F as InferenceFramework
    participant RT as IModelRuntime
    participant O as Orchestrator

    U->>R: input text
    R->>S: matchesDeterministicSkill(input)
    alt Deterministic match
        S-->>R: skill result (model not invoked)
        R-->>U: ✓ route=deterministic
    else No match
        R->>I: classifyIntent(input)
        I->>F: InferenceRequest + RuntimeInvocationSeal
        F->>RT: inferStream(request, seal, sink)
        RT-->>F: InferenceChunk stream
        F-->>R: InferenceOutput (verified)
        R-->>U: streamed tokens + metrics
    end
    opt Tool calls in output
        R->>O: executeDAG(plan)
        O->>O: WAL write-ahead
        O-->>R: result or UNKNOWN
    end
```

---

## Inference Trust Model

The framework never trusts a runtime's self-reported output. Every inference
passes through a **seal + digest** contract:

```mermaid
flowchart LR
    subgraph Caller["Caller (cmd_run / Executor)"]
        A1[build InferenceRequest]
        A2[compute prompt_digest]
        A3[build RuntimeInvocationSeal]
        A4[compute invocation_id = runtimeInvocationDigest]
    end

    subgraph Runtime["IModelRuntime"]
        B1[validateSeal: prompt_digest, fencing_token, epoch]
        B2[inferStream → emit chunks with invocation_id]
        B3[echoSeal → copy seal fields into output]
    end

    subgraph Framework["Framework Verifier"]
        C1[accumulate chunk deltas independently]
        C2[compare concatenation vs raw_output]
        C3["StreamIntegrity: Verified | Diverged | Aborted"]
    end

    A1 --> A2 --> A3 --> A4
    A4 --> B1
    B1 --> B2 --> B3
    B2 --> C1
    B3 --> C2
    C2 --> C3
```

**Key invariants:**
- A chunk whose `invocation_id` does not match the seal is rejected (stale/misrouted).
- `prompt_digest` must match between request and seal — prevents a replay from a different prompt.
- `fencing_token` + `replica_epoch` prevent split-brain when multiple replicas exist.
- The framework's `StreamIntegrity::Verified` means it independently confirmed the concatenated stream matches `raw_output`. A runtime cannot lie about what it streamed.

---

## WAL and Terminal States

```mermaid
stateDiagram-v2
    [*] --> Pending: WAL entry created
    Pending --> Running: executor picks up
    Running --> Committed: all side effects confirmed
    Running --> Failed: deterministic failure (4xx, validation)
    Running --> Unknown: ambiguous side effect (timeout, partial write)
    Committed --> [*]
    Failed --> [*]
    Unknown --> [*]: operator decision required

    note right of Unknown
        Sparx refuses to guess.
        UNKNOWN is a first-class terminal state.
        No auto-retry. No silent drop.
    end note
```

---

## Runtime Topology

```mermaid
graph LR
    subgraph DevLaptop["Developer Laptop (Experience A)"]
        sparx_cli["sparx run"]
        llama_server["llama-server (child process)"]
        gguf["weights.gguf"]
        sparx_cli -->|HTTP /v1/chat/completions| llama_server
        llama_server --> gguf
    end

    subgraph Vehicle["Automotive ECU (Experience B)"]
        sparx_deploy["sparx deploy"]
        genie_so["libGenie.so (vendor)"]
        qnn["QNN HTP runtime"]
        ctx_bin[".serialized.bin (compiled contexts)"]
        sparx_deploy -->|dlopen| genie_so
        genie_so --> qnn
        qnn --> ctx_bin
    end

    subgraph Both["Shared Kernel"]
        kernel["master_agent_core"]
    end

    sparx_cli --> kernel
    sparx_deploy --> kernel
```

**Experience A** needs only a GGUF file and (optionally) a pre-built `llama-server`.
The runtime spawns it as a child or attaches to one already running.

**Experience B** requires Qualcomm-licensed artifacts that cannot be redistributed.
`sparx doctor` validates device readiness before deploy.

---

## MCP Service Invocation

```mermaid
sequenceDiagram
    participant O as Orchestrator
    participant A as AtomicService
    participant P as IAtomicProvider (MCP endpoint)

    O->>O: WAL: record intent + idempotency_key
    O->>A: invoke(tool_definition, params, seal)
    A->>A: validate CompletionPolicy
    A->>P: MCP JSON-RPC call
    alt Success
        P-->>A: result
        A->>O: ReconcileStatus::Committed
        O->>O: WAL: mark COMMITTED
    else Timeout / partial
        P-->>A: no response or error
        A->>O: ReconcileStatus::Unknown
        O->>O: WAL: mark UNKNOWN (no retry)
    end
```

---

## Build Targets

| Target | Type | Contents |
|--------|------|----------|
| `master_agent_core` | STATIC | Kernel: orchestrator, inference framework, MCP, WAL, dispatch |
| `sparx_runtimes` | STATIC | `LlamaCppModelRuntime` + `GenieModelRuntime` |
| `sparx` | EXECUTABLE | CLI commands, links both libraries above |
| `test_*` (×15) | EXECUTABLE | Unit + integration tests per subsystem |

---

## 中文版：Sparx 架构设计

### 分层结构

| 层 | 目录 | 职责 |
|----|------|------|
| CLI 层 | `cli/` | 开发者交互入口：`run`, `deploy`, `doctor`, `init`, `demo` |
| 运行时适配层 | `cli/src/*_runtime.cpp` | 将硬件差异封装为统一 `IModelRuntime` 接口 |
| 内核层 | `include/master_agent/`, `src/` | 编排、推理框架、WAL、MCP、意图引擎 |
| 硬件层 | — | CPU (llama.cpp) 或 Qualcomm NPU (Genie/QNN) |

### 核心设计决策

1. **确定性优先路由** — 80%+ 请求无需调用模型。SkillEngine 基于关键词/规则匹配，
   命中即返回，延迟 < 1ms。只有未命中的请求才进入推理路径。

2. **Seal 信任模型** — 运行时不可自证输出正确性。框架独立累积流式 chunk 并与
   `raw_output` 比对，`StreamIntegrity::Verified` 表示框架确认了一致性。
   运行时无法谎报它流出了什么。

3. **Unknown 终止态** — 当侧效应不确定时（超时、部分写入），WAL 标记为
   `UNKNOWN` 而非自动重试。这是业界首个将"不知道"作为正式终止态的 Agent 框架。
   这比虚假的 SUCCESS 更安全——不会重复扣款、不会重复下单。

4. **单二进制、双体验** — 同一个 `sparx` 二进制在笔记本上用 llama-server
   提供 CPU 推理（Experience A），部署到车机时通过 dlopen 加载 libGenie.so
   使用 NPU（Experience B）。内核代码完全共享。

5. **Fencing Token + Epoch** — 防止脑裂。当多个 replica 存在时，只有持有
   最新 fencing_token 且 epoch 匹配的调用才能提交结果。过期的 seal 被
   fail-closed 拒绝。

### 与竞品的架构差异

| 维度 | Sparx | LangChain/AutoGen | 云端 Agent |
|------|-------|-------------------|-----------|
| 部署位置 | 端侧（车机/手机/IoT） | 服务器 | 云端 |
| 推理延迟 | < 100ms (NPU) | 200-2000ms | 500-5000ms |
| 网络依赖 | 无 | 需要 API | 强依赖 |
| 崩溃恢复 | WAL + Unknown 态 | 无 | 依赖云服务 |
| 流式验证 | 框架独立比对 | 无 | 无 |
| 隐私 | 数据不离设备 | 发送至 LLM 提供商 | 发送至云端 |
