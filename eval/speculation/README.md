# Speculative Execution Evaluation

## Overview

This evaluation measures the effectiveness of the SPARX Speculative Agent Execution system — a predictive pre-computation pipeline that uses NPU idle time to speculatively pre-compute likely user requests, delivering instant (sub-millisecond) responses when predictions are correct.

## Methodology

### Synthetic Trace Generation

We generate **realistic user interaction traces** across 50 simulated days (~1000+ interactions) using a pattern-aware trace generator with four behavioral profiles:

| Pattern | Schedule | Sequence | Regularity |
|---------|----------|----------|------------|
| **Morning routine** | 7-9am, 90% weekdays / 60% weekends | weather → calendar → commute (± news) | High |
| **Work session** | 9am-5pm, weekdays only | email → slack → jira → code_review → docs | Medium-high (Markov transitions) |
| **Evening routine** | 8-10pm, 85% of days | music/podcast → lights → alarm | High |
| **Random/exploratory** | Any time, fills remaining slots | 10 diverse intents (timer, translate, etc.) | None (uniform random) |

Traces include:
- Natural language variation (prefixes like "hey", "please", "can you")
- Occasional order swaps within routines (~10%)
- Time-of-day and day-of-week metadata for temporal modeling
- The random/exploratory group acts as a **control** — these interactions have no repeating pattern, so the predictor should achieve low hit rates on them (validating that high hit rates on patterned sessions are not an artifact).

### Simulation Engine

For each interaction in the trace:

1. **Check cache** — does the SpeculationCache contain a pre-computed result for this intent + input? If yes, count as a hit (latency = 0.5ms). If no, count as a miss (latency = 200ms mock inference).
2. **Observe** — feed the interaction to the IntentPredictor, updating bigram/trigram/temporal transition counts.
3. **Speculate** — if the predictor is warmed up, generate top-3 predictions and cache results for any that are not already present.
4. **Track metrics** — memory, entries, hit/miss, per-pattern breakdown.

### What is Measured

| Metric | Definition |
|--------|-----------|
| **Hit Rate** | % of turns where the speculation cache had the correct pre-computed result |
| **Latency Savings** | Actual ms saved per turn (200ms inference vs <1ms cached) |
| **Effective Speedup** | Ratio of total latency without vs with speculation |
| **Cold Start** | Turn index of the first successful cache hit (indicates warm-up period) |
| **False Positive Rate** | % of speculations that were computed but never consumed by a real request |
| **Memory Overhead** | Peak cache size in KB at steady state |
| **Per-Pattern Hit Rate** | Breakdown by morning/work/evening/random behavioral category |

### Ablation Study

We measure degradation when disabling each predictor component:

| Variant | What changes |
|---------|-------------|
| **Full model** | Bigram + trigram + temporal (temporal_weight=0.3) |
| **No temporal** | temporal_weight=0 — time-of-day signal removed |
| **Temporal-heavy** | temporal_weight=0.8 — over-reliance on time signal |
| **Conservative** | Full model but min_confidence=0.7 (fewer speculations triggered) |

### Baseline

The baseline is "no speculation" — every interaction pays the full 200ms inference latency. The evaluation compares total session latency and per-turn averages.

## Building and Running

From the project root:

```bash
g++ -std=c++17 -O2 -I cli/include \
    eval/speculation/eval_speculation.cpp cli/src/sparx_speculative.cpp \
    -o eval_speculation -pthread

./eval_speculation
```

Requirements:
- C++17 compiler (GCC 8+, Clang 7+, MSVC 2019+)
- Standard library only (no external dependencies)
- pthreads (for mutex/thread in the speculation classes)

## Interpreting Results

- **High hit rate on patterned sessions** (morning/evening/work) validates the n-gram prediction approach.
- **Low hit rate on random sessions** confirms the system is not over-fitting or cache-thrashing.
- **Cold start < 25 turns** shows the predictor learns user patterns within the first 1-2 days of typical use.
- **False positive rate < 10%** indicates efficient use of idle NPU compute.
- **Memory < 10 KB** confirms the cache fits easily within device constraints.

## Limitations

- This evaluation uses a **deterministic simulation** — it does not exercise the async `SpeculativeExecutor` thread or real NPU scheduling. Those require integration testing on target hardware.
- Input variation is limited to prefix/suffix perturbations. Real users exhibit more diverse paraphrasing.
- The embedding similarity matching (SimHash) is exercised via the cache's `get()` path but the traces do not specifically stress fuzzy matching scenarios with high paraphrase diversity.
- TTL expiry is not a major factor because the simulation is time-compressed (all interactions within a single process lifetime).

## File Structure

```
eval/speculation/
├── eval_speculation.cpp   # Complete evaluation harness
└── README.md              # This file
```
