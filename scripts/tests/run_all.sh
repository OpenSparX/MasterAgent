#!/usr/bin/env bash
#
# Runs the packaging/distribution test suites.
#
# These are separate from ctest because they test shell scripts, the release
# artifact, and the npm launcher — none of which the C++ test suite can reach.
# CI runs both.
#
#   ./scripts/tests/run_all.sh
#
# test_install_e2e.sh and test_npm_e2e.sh need a built artifact in dist/. Run
# scripts/build_release.sh first, or they will skip with a clear message.
#
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

SUITES=(
    test_triple_contract.sh
    test_licence_gate.sh
    test_install_e2e.sh
    test_npm_e2e.sh
)

TOTAL_PASS=0; TOTAL_FAIL=0; SKIPPED=()
RESULTS=()

for suite in "${SUITES[@]}"; do
    echo
    echo "########################################################"
    echo "# $suite"
    echo "########################################################"
    out=$(bash "scripts/tests/$suite" 2>&1); rc=$?
    echo "$out"
    # Suites print a trailing "N passed, M failed" line; harvest it so the
    # summary reflects individual assertions rather than just suite exit codes.
    line=$(printf '%s\n' "$out" | grep -E '^[[:space:]]*[0-9]+ passed, [0-9]+ failed' | tail -1)
    if [ -n "$line" ]; then
        p=$(echo "$line" | awk '{print $1}')
        f=$(echo "$line" | awk '{print $3}')
        TOTAL_PASS=$((TOTAL_PASS + p)); TOTAL_FAIL=$((TOTAL_FAIL + f))
        RESULTS+=("$suite: $p passed, $f failed")
    elif [ "$rc" -eq 2 ]; then
        SKIPPED+=("$suite")
        RESULTS+=("$suite: SKIPPED")
    else
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        RESULTS+=("$suite: FAILED (exit $rc, no summary line)")
    fi
done

echo
echo "########################################################"
echo "# packaging test summary"
echo "########################################################"
for r in "${RESULTS[@]}"; do echo "  $r"; done
echo
echo "  TOTAL: $TOTAL_PASS passed, $TOTAL_FAIL failed"
[ ${#SKIPPED[@]} -gt 0 ] && echo "  SKIPPED: ${SKIPPED[*]}"
echo

[ "$TOTAL_FAIL" -eq 0 ] || exit 1
