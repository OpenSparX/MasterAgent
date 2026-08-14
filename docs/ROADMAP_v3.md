# OpenSparX v3.x Roadmap

## Vision
Evolve from LAN-first agent mesh to cross-network, fully autonomous agent swarms
with learned behavior, verified safety, and heterogeneous compute orchestration.

---

## v3.0 — Neural Intelligence Layer

### 3.0.1 Neural Intent Predictor
Replace n-gram ensemble with lightweight LSTM/Transformer on intent embeddings.
- Train on-device from user history (federated, no data leaves device)
- Target: >80% top-1 prediction accuracy (vs ~60% for trigram)
- Architecture: 2-layer GRU, 64-dim hidden, quantized INT8 for NPU

### 3.0.2 CEGAR for Plan Verification
Counter-Example Guided Abstraction Refinement:
- Start with coarse abstraction (merge similar states)
- If spurious counterexample found → refine abstraction
- Enables verification of plans with 100+ nodes (currently ~20 practical)
- Reference: Clarke et al. "Counterexample-Guided Abstraction Refinement" (CAV 2000)

### 3.0.3 BLE Fallback Discovery
For scenarios without WiFi (automotive, industrial, wearables):
- BLE 5.0 advertising with custom service UUID
- GATT characteristics for capability exchange
- Seamless fallback: mDNS → BLE → USB-OTG
- Bandwidth-aware: only sync Merkle digests over BLE (not full ops)

---

## v3.1 — Autonomous Agent Swarms

### 3.1.1 Intent-Aware Speculation
- Speculation decisions informed by neural predictor confidence
- Adaptive threshold: raise when battery low, lower when charging+idle
- Multi-step lookahead: speculate chains (A→B→C) not just single next

### 3.1.2 Causal Broadcast Ordering
- Vector clock causality enforcement for CRDT operations
- Ensures "read-your-writes" consistency across mesh
- Buffering of out-of-order operations until causal dependencies met

### 3.1.3 Symmetry Reduction for BMC
- Detect isomorphic agents (same capabilities, same role)
- Collapse symmetric states → exponential reduction
- Combine with POR for multiplicative savings

---

## v3.2 — Production Hardening

### 3.2.1 mTLS Mesh Security
- Device-pinned X.509 certificates (TOFU on first contact)
- Certificate rotation via mesh consensus
- Encrypted CRDT operations in transit

### 3.2.2 Adaptive Merkle Granularity
- Auto-tune branching factor based on state size
- Hot-key detection: frequently-changing keys get finer buckets
- Tombstone compaction with crdt-safe garbage collection

### 3.2.3 Speculation Observability
- OpenTelemetry traces for speculation lifecycle
- Grafana dashboard: hit rate, latency saved, NPU utilization
- Anomaly detection: alert on hit rate drop

---

## v3.3 — Cross-Network Federation

### 3.3.1 Relay-Assisted WAN Mesh
- TURN-style relay for NAT traversal
- Selective sync: only high-priority CRDT keys cross WAN
- Bandwidth budgeting per peer link

### 3.3.2 Federated Learning for Predictors
- Share prediction model gradients (not data) across mesh
- Differential privacy guarantees (ε-DP with ε<1)
- Convergence without central coordinator

### 3.3.3 Heterogeneous Compute Scheduling
- Unified scheduler across NPU/GPU/CPU/TPU
- Operator-level splitting (not just layer-level)
- Pipeline parallelism with speculative prefetch

---

## Success Metrics

| Version | Key Metric | Target |
|---------|-----------|--------|
| v3.0 | Prediction accuracy | >80% top-1 |
| v3.0 | Verification scale | 100+ nodes |
| v3.1 | Mesh consistency | Causal+ |
| v3.2 | Security | mTLS, zero-trust |
| v3.3 | Network reach | WAN-capable |

---

## Timeline (Tentative)

- v3.0: Q4 2026
- v3.1: Q1 2027
- v3.2: Q2 2027
- v3.3: Q3 2027
