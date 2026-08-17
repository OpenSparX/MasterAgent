# Agent Mesh Protocol — Evaluation Methodology

## Overview

This evaluation suite measures the core properties of the OpenSparX Agent Mesh
Protocol under realistic multi-device conditions. It simulates 3-8 heterogeneous
devices (phones, tablets, laptops, smart speakers) performing concurrent state
mutations, experiencing network partitions, and coordinating split inference.

## Build

From the project root:

```bash
c++ -std=c++17 -O2 -I cli/include \
    eval/mesh/eval_mesh.cpp cli/src/sparx_mesh.cpp \
    -o eval_mesh -pthread
./eval_mesh
```

No external dependencies beyond the C++17 standard library and the project's own
`sparx_mesh.h` / `sparx_mesh.cpp`.

## Simulated Environment

### Device Profiles

| Device         | NPU  | GPU  | NPU TOPS | RAM (MB) | Battery | Idle  |
|----------------|------|------|----------|----------|---------|-------|
| Pixel 9 Pro    | Yes  | Yes  | 45       | 12,288   | 82%     | Yes   |
| Galaxy S25     | Yes  | Yes  | 75       | 12,288   | 65%     | No    |
| iPad Pro M4    | Yes  | Yes  | 38       | 16,384   | 91%     | Yes   |
| Laptop i9      | No   | Yes  | 0        | 32,768   | 70%     | No    |
| Nest Hub Max   | No   | No   | 0        | 4,096    | 100%    | Yes   |
| MacBook M3     | Yes  | Yes  | 18       | 24,576   | 55%     | No    |
| OnePlus 12     | Yes  | Yes  | 36       | 8,192    | 40%     | Yes   |
| ThinkPad X1    | No   | Yes  | 0        | 16,384   | 90%     | No    |

### Simulation Parameters

- Operations per test: 2,000-10,000 depending on section
- Keys in state space: 50-500
- Sync model: gossip-based with per-pair watermark tracking
- Partition model: random 1-3 nodes go offline, write concurrently, then rejoin
- Deterministic RNG seed (42) for reproducibility

## Metrics Measured

### 1. CRDT Convergence Time

Measures how many gossip rounds are needed after a batch of concurrent mutations
until all online nodes agree on all modified keys.

- **Method**: Issue 50 concurrent mutations per batch, then gossip until no new
  merges occur. Record rounds needed.
- **Reported**: avg, max, P99 sync rounds.
- **Expected**: 1-2 rounds for operation-based CRDTs with full mesh connectivity.

### 2. Merkle Anti-Entropy Efficiency

Measures the bandwidth savings of using Merkle tree digests to identify divergent
keys versus sending all state.

- **Method**: Populate 500 shared keys, then introduce 10% divergence. Compare
  Merkle digests between all node pairs, count bytes for digest exchange vs full
  state transfer.
- **Reported**: efficiency %, bandwidth ratio, divergent keys found.
- **Expected**: 80-95% savings when divergence is low (< 20% of keys).

### 3. Conflict Resolution Correctness

Verifies that CRDTs guarantee zero data loss under concurrent writes.

- **Tests**:
  - GCounter: multiple nodes increment concurrently; merged counter must reflect all.
  - ORSet: concurrent adds from all nodes; all items must survive in merged set.
  - LWWRegister: concurrent writes to same key; all nodes must agree on winner.
- **Reported**: total concurrent writes, data loss events, correctness %.
- **Target**: 100% correctness (zero data loss).

### 4. Partition Tolerance

Simulates network partitions where subsets of nodes cannot communicate, both
sides write concurrently, then the partition heals.

- **Method**: Take 1-3 nodes offline, issue writes on both sides, heal, gossip
  until convergence.
- **Reported**: partitions simulated, avg rounds to converge after heal.
- **Expected**: convergence in 1-2 rounds after heal.

### 5. Throughput

Measures raw ops/sec for state mutations with periodic synchronization.

- **Method**: Issue N mutations across the cluster with gossip every 100 ops.
  Measure wall-clock time including sync overhead.
- **Reported**: ops/sec for 3, 5, and 8 node clusters.

### 6. Bandwidth Overhead

Compares three synchronization strategies under the same workload:

| Strategy             | Description                                  |
|---------------------|----------------------------------------------|
| Naive full-sync     | Every round, every node sends full state     |
| CRDT op-based       | Only new operations since last sync          |
| Merkle-guided       | Digest exchange + divergent keys only        |

### 7. Split Inference Coordination

Tests the `SplitInferenceCoordinator::plan()` function with varying cluster
sizes and model configurations.

- **Method**: Attempt planning for 5 model/cluster combinations. Verify all
  layers are assigned and speedup ratios are computed.
- **Reported**: success rate, avg speedup ratio.

## Baselines

### A. Naive Full-State Sync
Every sync round transmits the entire key-value state from every node to every
other node. O(N^2 * K) bytes per round. Trivially correct (full copy) but
extremely wasteful.

### B. Last-Writer-Wins Without CRDT
Simulates a naive distributed system where concurrent writers to the same key
silently overwrite each other based on imprecise timestamps. Demonstrates the
data loss that CRDTs eliminate.

### C. Single-Leader Replication
One node is the designated leader; all writes must go through it. During network
partitions, followers cannot write (rejected). Demonstrates the availability
penalty that multi-leader CRDT systems avoid.

## Interpretation

The evaluation demonstrates that the Agent Mesh Protocol:

1. **Converges quickly** (1-2 rounds) because op-based CRDTs are commutative,
   associative, and idempotent — order of delivery doesn't matter.

2. **Saves bandwidth** via incremental op delivery (only new operations) and
   Merkle anti-entropy (identifies exactly which keys diverge without full scan).

3. **Never loses data** because CRDT merge functions are mathematically
   guaranteed to be monotonically increasing in information content.

4. **Tolerates partitions** because it's an AP (Available + Partition-tolerant)
   system — nodes can always write locally and merge later.

5. **Scales reasonably** to 8 heterogeneous devices at thousands of ops/sec,
   which exceeds the needs of typical agent collaboration workloads.
