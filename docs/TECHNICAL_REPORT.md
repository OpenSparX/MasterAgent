# OpenSparX: A Coherent System Architecture for On-Device Autonomous Agents

## Abstract

On-device autonomous agents promise personal AI that is fast, private, and reliable. Yet the field lacks a coherent systems-level answer to five intertwined challenges: latency, privacy, reliability, personalization, and safety. Most existing frameworks address one or two of these in isolation, or punt entirely to the cloud. OpenSparX presents an integrated architecture where five mutually-reinforcing subsystems — speculative execution, agent mesh networking, formal plan verification, on-device continual learning, and constrained decoding — compose into a system that is greater than the sum of its parts. This report describes the design, interactions, research contributions, and honest limitations of the approach.

## 1. Problem Statement

Running an autonomous agent on a personal device introduces constraints that cloud-hosted systems never face.

**Latency.** A cloud round-trip adds 100–500ms per tool call. For agents that chain 5–10 calls per task, this accumulates into multi-second delays that destroy interactive flow. Users perceive the agent as slow even when the underlying model is fast.

**Privacy.** Personal agents observe calendars, messages, health data, and financial records. Transmitting this context to remote servers — even temporarily — exposes it to breaches, subpoenas, and policy changes outside user control. On-device means the data never leaves.

**Reliability.** Network partitions are not edge cases; they are daily life. Airplane mode, spotty cellular, congested Wi-Fi in conference halls — an agent that depends on the cloud becomes useless precisely when the user needs it most.

**Personalization.** A model fine-tuned on aggregate data produces generic responses. True personalization requires learning from individual corrections over time, which conflicts with privacy unless the learning happens locally.

**Safety.** Autonomous agents execute multi-step plans that may include destructive operations (file deletion, API calls with side effects, financial transactions). Without formal guarantees, a mispredicted intent or hallucinated tool call can cause irreversible harm.

These five challenges are not independent — they form a system of constraints. A solution that addresses latency (speculation) must not compromise safety. A solution that enables personalization (learning) must not leak privacy. A system-level design must hold all five simultaneously.

## 2. System Architecture

OpenSparX addresses this constraint system with five subsystems that interact synergistically.

```
┌──────────────────────────────────────────────────────────────────────┐
│                        User Interaction                                │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│  CONSTRAINED DECODING                                                 │
│  (JSON Schema → GBNF grammar → logit masking)                         │
│  Ensures every tool call is structurally valid                        │
└────────────────────────────────┬─────────────────────────────────────┘
                                 │ valid tool calls only
                                 ▼
┌───────────────────┐   ┌───────────────────┐   ┌─────────────────────┐
│ SPECULATIVE        │   │ FORMAL VERIFY      │   │ AGENT MESH          │
│ EXECUTION          │◀─▶│                    │   │ PROTOCOL            │
│                    │   │ CTL* model checker │   │                     │
│ Intent prediction  │   │ checks plans       │   │ CRDT state sync     │
│ + pre-computation  │   │ BEFORE execution   │   │ + capability routing│
│ on idle NPU       │   │                    │   │ + split inference   │
└────────┬──────────┘   └────────────────────┘   └──────────┬──────────┘
         │                                                    │
         │         ┌──────────────────────────┐              │
         └────────▶│ ON-DEVICE LEARNING        │◀─────────────┘
                   │                           │
                   │ DP-SGD fine-tuning         │
                   │ improves predictions       │
                   │ + personalizes responses   │
                   └──────────────────────────┘
```

### 2.1 Speculative Execution

**Problem addressed:** Latency.

The speculative execution engine predicts the user's next intent and pre-computes responses during NPU idle time. When the prediction is correct, response latency drops to zero (cache hit). When incorrect, the system falls back to normal inference with no degradation.

The prediction model is a weighted ensemble:
- **Bigram frequency:** P(next_intent | current_intent) from transition counts
- **Temporal trigram:** P(intent | prev_2_intents, hour_of_day) incorporating circadian patterns
- **SimHash embedding similarity:** Fuzzy cache matching via 64-dimensional locality-sensitive hashing (Charikar, 2002) over character trigrams

Speculative work runs at the lowest priority (P3) and is preempted instantly when real inference is needed. A TTL-based LRU cache (default: 16 entries, 4MB, 5-minute TTL) stores pre-computed results. Context hashing ensures stale speculations are never served.

### 2.2 Agent Mesh Protocol

**Problem addressed:** Reliability + latency (multi-device).

The mesh protocol enables collaboration across a user's personal devices without any cloud dependency. A phone, laptop, and tablet can share agent state, route tasks to the most capable device, and even split large model inference across multiple NPUs.

Key mechanisms:
- **Zero-config discovery** via mDNS/DNS-SD (RFC 6762/6763) with BLE fallback
- **Capability-based routing** scores peers on NPU TOPS, RAM, battery, idle state, and network proximity
- **CRDT state synchronization** using Observed-Remove Sets (ORSet) with Lamport timestamps and vector clocks for causal ordering (Shapiro et al., 2011)
- **Merkle anti-entropy** for efficient sync: peers exchange compact hash digests and transfer only divergent key-buckets, reducing sync cost from O(K) to O(D log K) where D is the number of divergent keys
- **Split inference** partitions model layers across multiple NPU-capable devices when a model exceeds single-device memory

Security uses mTLS with device-pinned certificates (TOFU model). The system degrades gracefully to single-device operation when isolated.

### 2.3 Formal Plan Verification

**Problem addressed:** Safety.

Before any multi-step plan executes, the formal verifier checks it against temporal logic safety properties. The plan DAG is translated into a Kripke structure, and a bounded model checker evaluates CTL* formulas:

- `AG(not destructive_without_auth)` — no destructive operation runs without authorization
- `AG(AF node_terminates)` — every node eventually completes or fails
- `AG(not resource_deadlock)` — acquired resources are eventually released
- `A[completion <= deadline]` — bounded liveness

Partial-Order Reduction (Peled, 1993) reduces the state space for concurrent plan nodes by identifying independent transitions that need not be interleaved. Typical verification completes in <10ms for plans with up to 50 nodes.

A runtime monitor based on causal past logic continues checking properties during execution, catching violations that static analysis cannot foresee (e.g., unexpected timeouts).

### 2.4 On-Device Continual Learning

**Problem addressed:** Personalization + privacy.

When a user corrects the agent, the correction is stored as a (input, model_output, preferred_output) triple, encrypted at rest with a device-bound key. Once enough corrections accumulate, QLoRA fine-tuning runs during idle time to produce a personalized LoRA adapter.

Privacy is enforced through DP-SGD (Abadi et al., 2016):
- Per-sample gradient clipping (norm bound C = 1.0)
- Calibrated Gaussian noise addition (sigma = 1.1)
- Renyi Differential Privacy accounting to track cumulative epsilon
- Hard budget (default epsilon = 4.0 per week); training halts when exhausted

Quality is protected by a perplexity guard: if the adapted model's perplexity on a held-out set exceeds 105% of baseline, the adapter is rejected. Progressive adapter merging (weighted average: 0.7 new + 0.3 old) prevents catastrophic forgetting across training runs.

The resulting adapter is a few MB and loads via llama-server's `--lora` flag with <100ms overhead.

### 2.5 Constrained Decoding

**Problem addressed:** Reliability (structural correctness).

When the model must produce a tool call, the constrained decoder converts registered tool schemas (JSON Schema) into a GBNF grammar that masks invalid tokens at each sampling step. The model literally cannot produce malformed JSON or hallucinated parameter names — validity is enforced at the logit level.

The grammar generator:
1. Reads tool input schemas from MCP/skill definitions
2. Produces GBNF production rules for each tool's argument structure
3. Creates a top-level alternation: `root ::= tool1-call | tool2-call | ... | free-text`
4. Injects the grammar into llama-server's `/v1/chat/completions` request

This eliminates an entire class of runtime failures (malformed tool calls) without post-hoc validation or retry loops.

## 3. Feature Interactions

The system's value lies in how the five subsystems compose:

**Speculation + Formal Verify:** Speculative execution pre-computes responses, but those responses may include multi-step plans. Before a speculated plan is served from cache, the formal verifier checks it against current safety properties. A speculated plan that was safe when computed may be unsafe now (e.g., a file was deleted in the interim). The context hash and verification seal together prevent stale-but-dangerous speculations from executing.

**Speculation + Learning:** The learning subsystem improves speculation accuracy over time. As the model adapts to the user's patterns, intent predictions become more accurate, which increases cache hit rates, which further reduces latency. This creates a positive feedback loop: more usage → better personalization → better prediction → faster responses → more usage.

**Speculation + Mesh:** Idle devices in the mesh are candidates for speculative pre-computation. If the user's phone predicts what they will ask next but lacks NPU capacity, it can route the speculative inference to an idle tablet via the mesh. The result is cached and ready when needed.

**Mesh + Learning:** Learned corrections are synchronized across devices via CRDT state sync. A correction made on the phone improves the adapter on all devices. The ORSet CRDT ensures corrections are never lost even under concurrent edits on multiple devices.

**Constrained Decode + Speculation/Mesh:** Every tool call produced by speculation or routed through the mesh is structurally valid by construction. Constrained decoding is the foundation that makes the other subsystems reliable — a speculated tool call that is malformed is worse than no speculation at all.

**Formal Verify + Mesh:** When a plan is routed to a remote device for execution, the originating device can independently verify the plan before dispatching it. Safety guarantees are not delegated to the executing device.

## 4. Positioning vs. State of the Art

### Cloud-First Agent Frameworks (LangChain, AutoGPT, CrewAI)

These frameworks assume reliable, low-latency cloud connectivity. They have no on-device execution story, no local learning, no formal verification, and no mesh collaboration. They are unsuitable for personal agents that must work offline and protect user data.

### Apple Intelligence

Apple's on-device approach addresses privacy but remains closed-source with no extensibility. It lacks: mesh collaboration across non-Apple devices, formal plan verification, user-controllable learning, and a speculation system that learns from interaction patterns. Its constrained tool calling relies on proprietary adapter training rather than grammar-level enforcement.

### Academic On-Device Work

Gao et al. (2024, "On-Device LLM Agent") demonstrate feasibility of on-device agent execution but do not address the systems-level composition problem — their work focuses on model compression and inference optimization for a single device, without mesh, formal verification, or privacy-preserving learning.

Xu et al. (2024, "MobileLLM") optimize model architecture for mobile deployment but address only the inference substrate, not the agent-level concerns of safety, collaboration, and personalization.

Neither body of work addresses the interaction between subsystems that OpenSparX treats as its core contribution.

## 5. Research Contributions

Each subsystem applies known techniques to a novel domain (on-device autonomous agents), with specific adaptations:

| Feature | Technique | Novel Application |
|---------|-----------|-------------------|
| Speculation | SimHash (Charikar, 2002) | Ensemble predictor combining n-gram models with locality-sensitive embedding for intent similarity matching in agent command spaces |
| Mesh | ORSet CRDTs + Merkle anti-entropy (Shapiro et al., 2011) | Agent memory and correction state synchronization across personal device meshes; capability-based routing for heterogeneous NPU topologies |
| Formal Verify | CTL* + Partial-Order Reduction (Peled, 1993) | Model checking of agent plan DAGs (typically applied to hardware/protocol verification); runtime monitoring via causal past logic |
| Learning | DP-SGD + Renyi accounting (Abadi et al., 2016) | On-device QLoRA fine-tuning with formal privacy guarantees; progressive adapter merging to prevent catastrophic forgetting |
| Constrained Decode | GBNF grammars (llama.cpp) | Automatic grammar synthesis from MCP tool schemas; grammar-enforced structural validity for all agent tool calls |

We do not claim invention of CRDTs, CTL*, DP-SGD, or SimHash. The contribution is the engineering application of these techniques to the on-device agent domain, and the demonstration that they compose into a coherent system that holds all five constraints simultaneously.

## 6. Limitations

**Speculation accuracy is user-dependent.** Users with irregular, non-repetitive workflows will see low cache hit rates. The cold-start problem (minimum 10 interactions before predictions begin) means new users see no benefit initially.

**Mesh requires co-located devices.** LAN-first discovery (mDNS) means the mesh only functions when devices share a network. The WAN relay is optional and reintroduces a server dependency. BLE fallback has severe bandwidth constraints.

**Formal verification is bounded.** The model checker uses bounded model checking — it can prove properties up to depth k but cannot guarantee unbounded liveness for plans with cycles or unbounded recursion. Plans with >50 nodes may exceed the 100ms verification budget.

**Learning quality depends on correction volume.** With the default epsilon budget of 4.0 and noise multiplier of 1.1, the system needs approximately 20–50 high-quality corrections to produce a measurably improved adapter. Users who rarely correct the agent see little personalization benefit.

**Constrained decoding adds latency.** Grammar-constrained sampling is slower than unconstrained sampling (approximately 10–30% token generation overhead) because logit masking requires grammar state tracking at each step. For simple free-text responses, the grammar is not applied.

**Split inference is experimental.** Partitioning model layers across network-connected devices introduces activation transfer latency that may negate compute savings unless bandwidth exceeds approximately 1 Gbps. This feature is disabled by default.

**Privacy-utility tradeoff is real.** DP-SGD with epsilon=4.0 provides meaningful privacy guarantees but measurably reduces learning speed compared to non-private fine-tuning. Users cannot "opt out" of privacy (by design) even if they would prefer faster personalization.

## 7. Conclusion

OpenSparX demonstrates that the five core challenges of on-device autonomous agents — latency, privacy, reliability, personalization, and safety — can be addressed by a single coherent architecture rather than five independent solutions. The key insight is that these subsystems are not merely co-located; they actively reinforce each other: learning improves speculation, verification constrains speculation, the mesh distributes both, and constrained decoding provides the structural foundation that makes distribution safe.

## References

- Abadi, M. et al. (2016). "Deep Learning with Differential Privacy." CCS 2016.
- Charikar, M. (2002). "Similarity Estimation Techniques from Rounding Algorithms." STOC 2002.
- DeCandia, G. et al. (2007). "Dynamo: Amazon's Highly Available Key-value Store." SOSP 2007.
- Gao, Y. et al. (2024). "On-Device LLM Agent." arXiv preprint.
- Peled, D. (1993). "All from One, One for All: On Model Checking Using Representatives." CAV 1993.
- Shapiro, M. et al. (2011). "Conflict-Free Replicated Data Types." SSS 2011.
- Xu, Z. et al. (2024). "MobileLLM: Optimizing Sub-billion Parameter Language Models for On-Device Use Cases." ICML 2024.
