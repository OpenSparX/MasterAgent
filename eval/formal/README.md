# Formal Plan Verification -- Evaluation

## Overview

This evaluation comprehensively benchmarks the Sparx Formal Plan Verification system
(`sparx_formal_verify.h/.cpp`) across 25 plan topologies, measuring detection accuracy,
scaling behavior, Partial-Order Reduction effectiveness, and runtime monitoring overhead.

## Build & Run

```bash
cd /tmp/sparx-work/eval/formal
c++ -std=c++17 -O2 -I../../cli/include eval_formal.cpp ../../cli/src/sparx_formal_verify.cpp -o eval_formal
./eval_formal
```

## Methodology

### Plan Topologies (25 scenarios)

| # | Topology | Nodes | Purpose |
|---|----------|-------|---------|
| 1 | Sequential chain | 3 | Baseline; should verify instantly |
| 2 | Diamond (fork+join) | 4 | Tests parallel path handling |
| 3 | Unsafe payment | 3 | `send_payment` before `confirm_user` |
| 4 | Complex layered DAG | 25 | Multi-critical-path scalability |
| 5 | Liveness plan | 3 | Tests AF property |
| 6 | Circular dependency | 3 | Must detect cycle and reject |
| 7 | Fan-out (1:10) | 11 | Wide parallel dispatch |
| 8 | Fan-in (10:1) | 11 | Wide convergence |
| 9 | Auth + destructive | 2 | Safe destructive with auth |
| 10 | Destructive no auth | 2 | Unsafe: no authorization |
| 11 | Resource contention | 3 | Two destructive on same resource |
| 12 | Retry-safe (idempotent) | 2 | Verifies retry correctness |
| 13 | Long chain | 15 | Tests depth scaling |
| 14 | Binary tree | 15 | Tests breadth scaling |
| 15 | Error recovery | 4 | Error -> recovery path |
| 16 | Multi-service | 6 | 6 services, auth + deploy |
| 17 | Parallel safe destructive | 3 | Different services = safe |
| 18 | Deadline-critical | 3 | Short timeouts |
| 19 | Large DAG | 50 | Scalability stress test |
| 20 | Unsafe email | 3 | `send_email` before `confirm` |
| 21 | Self-loop | 1 | Malformed node depends on itself |
| 22 | Empty plan | 0 | Edge case |
| 23 | Single node | 1 | Minimal plan |
| 24 | W-shape (double diamond) | 7 | Complex join pattern |
| 25 | Resource deadlock | 3 | Two nodes hold cross-resources |

### Safety Properties Tested

1. **AG(not(send_payment AND not(user_confirmed)))** -- Never pay without confirmation
2. **AF(task_complete)** -- All plans eventually terminate
3. **AG(error -> AF(recovery))** -- Errors always lead to recovery
4. **AG(not(send_email AND not(confirm)))** -- Never email without confirmation
5. **AG(not(dataflow.backward))** -- Data flows forward only
6. **AG(not(destructive AND not(authorized)))** -- Auth precedes destruction

### Metrics Collected

| Metric | Meaning |
|--------|---------|
| Detection Rate | % of unsafe plans correctly flagged |
| False Positive Rate | % of safe plans incorrectly rejected |
| Verification Time vs Size | Polynomial scaling characterization |
| POR Effectiveness | State-space reduction from Partial-Order Reduction |
| Monitor Overhead | Per-event cost of RuntimeMonitor |

## Key Findings

### Detection Accuracy

- **Detection Rate: 71.4%** (5/7 unsafe plans caught)
- **False Positive Rate: 0%** (all 18 safe plans pass)
- Missed cases are concurrent-state properties (`conflict.destructive`, `resource.held`)
  that require joint-state tracking not present in the per-node Kripke construction.

### Scaling

- Verification time scales as **O(n^3)** (empirical exponent ~2.9-3.0)
- 50-node plans verify in ~10ms, 100-node plans in ~155ms
- State space grows linearly with node count (3 states per node + 2 terminal)

### Partial-Order Reduction

POR currently adds overhead (~30-70% slower) without reducing state space for
these topologies. The fan-out plans assign different services to each worker,
making the independence heuristic unable to reduce. POR would benefit from:
- Shared-resource workloads (same service, same tool)
- Workloads with true data-independent branches

### RuntimeMonitor

- Overhead per event: **~0.2 us** (amortized, for 3 monitored properties)
- Detects safety violations in real-time (demonstrated: payment-before-confirm)
- Linear scaling with event count

### Known Limitations

1. **AF (liveness) in BMC**: The Kripke model includes failure self-loops,
   causing AF to conservatively report "not all paths terminate." This is
   technically correct (failure IS a non-completing path) but operationally
   noisy. Workaround: use `AF(completed OR failed)` or fair-path semantics.

2. **Concurrent state labels**: The model labels individual node states but
   doesn't track joint concurrent state (e.g., "two destructive nodes executing
   simultaneously"). This limits detection of resource-contention patterns.

3. **POR overhead**: The `computeAmpleSet()` dependency-graph construction adds
   cost that exceeds its benefit on plans where all transitions are dependent.
   Consider disabling POR for plans with < 10 parallel branches.

## Architecture

```
eval_formal.cpp
  |
  +-- 25 Plan Topology Builders (buildSequential3, buildDiamond4, ...)
  +-- Custom Property Constructors (propNeverPayWithoutConfirm, ...)
  +-- Cycle Detection (Kahn's algorithm)
  +-- Scaling Measurement (2..100 nodes, log-log regression)
  +-- POR Comparison (with/without, fan-out topologies)
  +-- Monitor Overhead Measurement (10..1000 events)
  +-- Live Simulation (RuntimeMonitor on unsafe payment stream)
```

## Interpreting Results

- **SAFE / UNSAFE**: Whether the verifier found any property violations
- **CYCLE**: Pre-verification cycle detection rejected the plan
- **States**: Total Kripke states in the model (3 per node + terminals)
- **Speedup < 1.0x**: POR is slower than full exploration for this topology
- **Overhead%**: Relative cost vs bare event iteration (high % is expected
  since the baseline is nearly zero-cost iteration)
