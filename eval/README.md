# OpenSparX Evaluation Suite

Comprehensive evaluation binaries that measure system-level metrics for each
major OpenSparX subsystem. These are not unit tests — they simulate realistic
workloads and produce quantitative reports suitable for technical papers,
design reviews, and regression tracking.

## Quick Start

```bash
# From the project root:
bash eval/run_all.sh

# Results appear in eval/results/
# Summary report: eval/results/SUMMARY.md
```

## Prerequisites

- CMake 3.18+
- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- The project must configure successfully (third_party/memory_short_term must be present)

## Running Individual Evaluations

Build everything first:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target eval_speculation eval_mesh eval_formal eval_learning eval_constrained
```

Then run individually:

```bash
./build/eval_speculation
./build/eval_mesh
./build/eval_formal
./build/eval_learning [--verbose] [--seed 42]
./build/eval_constrained [--verbose]
```

## What Each Evaluation Measures

### eval_speculation — Speculative Agent Execution

Simulates 50 days of user interactions with patterned routines (morning,
work, evening) and random exploratory queries. Measures:

- **Hit rate:** percentage of turns served from speculation cache
- **Latency savings:** ms saved per turn vs always running full inference
- **Cold start:** how many interactions before the predictor starts hitting
- **False positive rate:** speculations computed but never used
- **Memory overhead:** peak cache size in KB
- **Ablation study:** contribution of bigram, trigram, and temporal components

### eval_mesh — Agent Mesh Protocol

Simulates 3-8 heterogeneous devices (phones, tablets, laptops) exchanging
state via CRDTs with Merkle anti-entropy. Measures:

- **CRDT convergence time:** gossip rounds needed for all nodes to agree
- **Merkle anti-entropy efficiency:** bandwidth savings vs full-state sync
- **Conflict resolution correctness:** zero data loss with GCounter, ORSet, LWW
- **Partition tolerance:** convergence after network heals
- **Throughput:** ops/sec at various cluster sizes
- **Bandwidth overhead:** comparison against naive full-sync and single-leader
- **Split inference coordination:** feasibility of distributing model layers

### eval_formal — Formal Plan Verification

Runs 25 plan topologies (safe and unsafe) through the Kripke model checker.
Measures:

- **Detection rate:** percentage of unsafe plans correctly flagged
- **False positive rate:** safe plans incorrectly rejected
- **Verification scaling:** time vs plan size (log-log regression)
- **POR effectiveness:** speedup from Partial-Order Reduction
- **Runtime monitor overhead:** cost per event of live property checking
- **Safety property checking:** AG/AF temporal logic formula evaluation

### eval_learning — On-Device Continual Learning

Simulates 220+ user corrections over 30 days across multiple domains
(medical, legal, technical, style, format). Measures:

- **Perplexity improvement:** reduction from base model after adaptation
- **Personalization accuracy:** how many corrections the adapted model remembers
- **Privacy budget consumption:** epsilon spent under differential privacy
- **Catastrophic forgetting:** retention when training on new domains
- **Quality guard effectiveness:** catching degraded adapters before commit
- **Training time:** simulated on-device NPU training duration
- **Adapter size:** LoRA adapter growth over versions

### eval_constrained — Constrained Decoding (GBNF)

Tests the GBNF grammar generator against 16+ realistic MCP tool schemas of
varying complexity. Measures:

- **Grammar correctness:** percentage of schemas producing valid GBNF
- **Schema coverage:** JSON Schema features handled (enums, arrays, nesting)
- **Scaling behavior:** grammar size vs schema complexity
- **Decode validity rate:** structural guarantees of generated grammars
- **Generation overhead:** microseconds to produce grammar from schema
- **Constrained vs unconstrained comparison:** malformed outputs avoided
- **Stress tests:** 50-field schemas, 120-value enums, 15-tool unions

## Interpreting Results

Each evaluation prints a structured report to stdout. Key sections:

1. **Configuration** — parameters used for the run
2. **Metrics tables** — quantitative results with units
3. **Verdict / Pass-Fail** — threshold-based checks (where applicable)
4. **Comparison** — with/without the subsystem, or vs baselines

A run is considered successful if the binary exits with code 0. Non-zero
exit indicates a threshold check failed (eval_constrained) or an assertion
fired (eval_learning privacy accounting).

For regression tracking, compare the numeric metrics across runs. The
evaluations use deterministic random seeds (default 42) so results are
reproducible given the same code.

## Adding New Evaluations

1. Create a subdirectory: `eval/<subsystem>/`
2. Write `eval_<subsystem>.cpp` — include the relevant header from `cli/include/`
3. Add the target in `eval/CMakeLists.txt` using `add_eval_target()`
4. Add the binary name to the `EVALS` array in `eval/run_all.sh`
5. Document what it measures in this README

Template for a new eval CMake entry:

```cmake
add_eval_target(eval_<subsystem>
    "${CMAKE_CURRENT_SOURCE_DIR}/<subsystem>/eval_<subsystem>.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_<subsystem>.cpp"
)
```

If the evaluation needs additional CLI sources, pass them as a semicolon-separated
list in the third argument to `add_eval_target()`.

## Design Principles

- **Deterministic:** fixed random seeds for reproducibility
- **Self-contained:** no network, no hardware dependencies, no external data
- **Realistic workloads:** patterns modeled on actual usage scenarios
- **Quantitative:** every claim backed by measured numbers
- **Fast:** full suite completes in under 30 seconds on a modern laptop
