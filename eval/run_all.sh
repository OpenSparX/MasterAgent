#!/usr/bin/env bash
# eval/run_all.sh — Build and run the full OpenSparX evaluation suite.
#
# Usage:
#   cd /path/to/sparx-work
#   bash eval/run_all.sh [--verbose]
#
# Outputs:
#   eval/results/<eval_name>.txt  — per-evaluation raw output
#   eval/results/SUMMARY.md       — aggregated report

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULTS_DIR="${SCRIPT_DIR}/results"
VERBOSE_FLAG=""

for arg in "$@"; do
    case "$arg" in
        --verbose|-v) VERBOSE_FLAG="--verbose" ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# --- Build ---
echo "=== Building evaluation binaries ==="
mkdir -p "${BUILD_DIR}"
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
cmake --build "${BUILD_DIR}" --target eval_speculation eval_mesh eval_formal eval_learning eval_constrained -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
echo ""

# --- Prepare results directory ---
rm -rf "${RESULTS_DIR}"
mkdir -p "${RESULTS_DIR}"

EVALS=(
    eval_speculation
    eval_mesh
    eval_formal
    eval_learning
    eval_constrained
)

PASS_COUNT=0
FAIL_COUNT=0
declare -a STATUSES=()

# --- Run each evaluation ---
for eval_name in "${EVALS[@]}"; do
    BINARY="${BUILD_DIR}/${eval_name}"
    OUTPUT_FILE="${RESULTS_DIR}/${eval_name}.txt"

    echo "--- Running ${eval_name} ---"
    if [[ -x "${BINARY}" ]]; then
        if "${BINARY}" ${VERBOSE_FLAG} > "${OUTPUT_FILE}" 2>&1; then
            echo "  PASSED (output: ${OUTPUT_FILE})"
            STATUSES+=("PASS")
            ((PASS_COUNT++))
        else
            echo "  FAILED (exit code $?, output: ${OUTPUT_FILE})"
            STATUSES+=("FAIL")
            ((FAIL_COUNT++))
        fi
    else
        echo "  SKIPPED (binary not found: ${BINARY})"
        STATUSES+=("SKIP")
        ((FAIL_COUNT++))
    fi
done

echo ""
echo "=== Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed ==="

# --- Generate SUMMARY.md ---
SUMMARY="${RESULTS_DIR}/SUMMARY.md"
cat > "${SUMMARY}" << EOF
# OpenSparX Evaluation Summary

**Date:** $(date -u '+%Y-%m-%d %H:%M:%S UTC')
**Build type:** Release
**Results:** ${PASS_COUNT} passed, ${FAIL_COUNT} failed

## Per-Evaluation Results

| Evaluation | Status | Output |
|-----------|--------|--------|
EOF

for i in "${!EVALS[@]}"; do
    eval_name="${EVALS[$i]}"
    status="${STATUSES[$i]}"
    echo "| ${eval_name} | ${status} | [${eval_name}.txt](${eval_name}.txt) |" >> "${SUMMARY}"
done

cat >> "${SUMMARY}" << 'EOF'

## Evaluation Descriptions

| Evaluation | Measures |
|-----------|----------|
| eval_speculation | Speculative agent execution: hit rate, latency savings, cold start, false positive rate, memory overhead |
| eval_mesh | Agent mesh protocol: CRDT convergence, Merkle anti-entropy, conflict resolution, partition tolerance, throughput |
| eval_formal | Formal plan verification: detection rate, false positive rate, scaling, POR effectiveness, runtime monitor overhead |
| eval_learning | On-device continual learning: perplexity improvement, personalization accuracy, privacy budget, catastrophic forgetting |
| eval_constrained | Constrained decoding (GBNF): grammar correctness, schema coverage, scaling, generation overhead |

## Raw Output Excerpts

EOF

for eval_name in "${EVALS[@]}"; do
    OUTPUT_FILE="${RESULTS_DIR}/${eval_name}.txt"
    if [[ -f "${OUTPUT_FILE}" ]]; then
        echo "### ${eval_name}" >> "${SUMMARY}"
        echo '```' >> "${SUMMARY}"
        # Include last 30 lines (typically the summary section)
        tail -30 "${OUTPUT_FILE}" >> "${SUMMARY}"
        echo '```' >> "${SUMMARY}"
        echo "" >> "${SUMMARY}"
    fi
done

echo "Summary written to: ${SUMMARY}"
