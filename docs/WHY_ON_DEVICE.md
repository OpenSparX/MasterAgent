# Why On-Device Agents Need These 5 Capabilities

Your phone has a neural processor sitting idle 90% of the time. OpenSparX puts it to work building a personal agent that is fast, private, reliable, adaptable, and safe — without ever phoning home to the cloud.

Here is why each capability exists and why removing any one of them breaks the system.

## 1. Speculative Execution — Because Waiting is Unacceptable

Cloud APIs add 100–500ms per round-trip. Chain five tool calls together, and the user is staring at a spinner for seconds. On-device inference is faster, but still not instant.

Speculative execution solves this by predicting what you will ask next and pre-computing the answer while the NPU is idle. It tracks your interaction patterns — "every morning you check the calendar, then the weather, then transit times" — and has those answers ready before you ask. When the prediction hits, response time is zero.

When it misses, nothing bad happens. You get the normal inference path. Speculation is pure upside.

The prediction engine combines frequency-based transition models (bigrams, trigrams weighted by time-of-day) with a lightweight SimHash embedding for fuzzy matching. No cloud-scale model needed — this runs in microseconds.

## 2. Agent Mesh Protocol — Because You Own More Than One Device

Your phone, laptop, and tablet all have compute capability. Today they operate in isolation. The mesh protocol lets them collaborate:

- **Discovery:** Devices find each other automatically via mDNS (zero config, works on any LAN).
- **Routing:** A task goes to the device best suited for it — the one with the right model loaded, the most free RAM, or the one that is idle.
- **State sync:** Corrections you make on your phone appear on your laptop. No cloud sync service required. Conflict-free replicated data types (CRDTs) ensure merges are always consistent, even if devices were offline.
- **Split inference:** If a model is too large for one device, the mesh can partition layers across two NPUs.

The mesh operates without any central server. When devices are isolated, each one works independently. When they reconnect, state converges automatically.

## 3. Formal Plan Verification — Because Autonomous Actions Have Consequences

An agent that can read your email, call APIs, and manage files is powerful. It is also dangerous. A hallucinated tool call or a plan that executes steps in the wrong order can delete data, send messages you did not intend, or corrupt state.

Before any multi-step plan executes, the formal verifier checks it against safety properties expressed in temporal logic:

- No destructive operation runs without prior authorization
- No two conflicting operations execute concurrently
- Every operation completes within its deadline
- No resource deadlock

The verifier builds a mathematical model of the plan and exhaustively checks these properties. It runs in under 10ms for typical plans. If a property is violated, the verifier produces a concrete counterexample trace showing exactly how things would go wrong.

This is not a heuristic or a prompt-based guardrail. It is a formal proof that the plan is safe.

## 4. On-Device Continual Learning — Because One Size Does Not Fit All

A base model trained on internet-scale data does not know your preferences, your writing style, your workflow, or your terminology. Every correction you give the agent is a learning opportunity.

OpenSparX captures corrections as training pairs and fine-tunes a small LoRA adapter during device idle time. The adapter is a few megabytes and loads in <100ms, personalizing the model to you.

Privacy is non-negotiable:
- Training data is encrypted at rest with a device-bound key
- Training uses Differential Privacy (DP-SGD) so individual corrections cannot be extracted from the adapter
- A hard privacy budget (epsilon) limits total information leakage per week
- Data never leaves the device — not even the adapter weights

A quality guard rejects any adapter that makes the model worse (perplexity check), and progressive merging prevents newer learning from erasing older knowledge.

## 5. Constrained Decoding — Because "Almost Valid" JSON Breaks Everything

When an agent calls a tool, it must produce JSON that exactly matches the tool's schema. A missing quote, a hallucinated parameter name, or an invalid type crashes the pipeline. Retry loops are slow and unreliable.

Constrained decoding eliminates this failure mode entirely. It converts tool schemas into a grammar that physically prevents the model from generating invalid tokens. At each sampling step, tokens that would violate the grammar are masked to zero probability.

The result: every tool call is structurally valid by construction. Not "usually valid" — always valid. This is the foundation that makes speculation and mesh routing trustworthy. A speculated tool call that is malformed is worse than no speculation at all.

## How They Work Together

These are not five independent features bolted onto a framework. They form a feedback loop:

- **Learning improves speculation.** As the model adapts to your patterns, intent predictions get more accurate, which means more cache hits, which means faster responses.
- **Verification guards speculation.** A speculated plan is not served until verified safe. Context changes since pre-computation can invalidate safety properties.
- **The mesh distributes speculation.** Idle devices pre-compute speculations for busy ones.
- **Constrained decoding makes distribution safe.** Every tool call — whether speculated locally, routed to another device, or generated in real-time — is structurally valid.
- **Corrections sync across the mesh.** Learn once on any device, benefit everywhere.

Remove any one piece and the system degrades:
- Without speculation: latency returns.
- Without mesh: wasted compute on idle devices, no redundancy.
- Without verification: speculation becomes dangerous.
- Without learning: speculation never improves, responses stay generic.
- Without constrained decoding: tool calls from speculation/mesh are unreliable.

## The Bet

OpenSparX bets that the next generation of personal agents will run primarily on-device, not because hardware is faster than the cloud (it is not), but because privacy, reliability, and personalization matter more than raw throughput for personal computing. The NPU in your pocket is enough — if the software is designed as a coherent system rather than five independent hacks.
