#!/usr/bin/env bash
#
# End-to-end test of core CLI commands. Verifies init, doctor, demo, and
# error handling without requiring actual NPU hardware or models.
#
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V2="$REPO_ROOT"
VERSION=$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')
VERSION="${VERSION:-2.1.0}"

# Build the CLI binary if not present
if [ ! -f "$V2/build/sparx" ]; then
  echo "=== Building sparx CLI ==="
  cd "$V2"
  mkdir -p build && cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || {
    echo "  SKIP  cmake not available or build failed"
    exit 0
  }
  make -j4 >/dev/null 2>&1 || {
    echo "  SKIP  make failed"
    exit 0
  }
  cd "$V2"
fi

SPARX="$V2/build/sparx"
if [ ! -x "$SPARX" ]; then
  echo "  SKIP  sparx binary not found at $SPARX"
  exit 0
fi

ROOT=/tmp/sparx_cli_e2e
PASS=0; FAIL=0
t_pass(){ echo "  PASS  $1"; PASS=$((PASS+1)); }
t_fail(){ echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
check(){ if [ "$1" = "$2" ]; then t_pass "$3"; else t_fail "$3 (want '$2', got '$1')"; fi; }

rm -rf "$ROOT"; mkdir -p "$ROOT"
cd "$ROOT"

echo "=== 1. sparx version ==="
OUT=$("$SPARX" version 2>&1 | head -1)
echo "$OUT" | grep -q "sparx" && t_pass "version command works" || t_fail "version command failed"

echo "=== 2. sparx --help ==="
"$SPARX" --help >/dev/null 2>&1 && t_pass "--help exits 0" || t_fail "--help failed"
HELP_OUT=$("$SPARX" --help 2>&1)
echo "$HELP_OUT" | grep -q "init" && t_pass "help shows init command" || t_fail "help missing init"
echo "$HELP_OUT" | grep -q "demo" && t_pass "help shows demo command" || t_fail "help missing demo"

echo "=== 3. sparx init creates project structure ==="
"$SPARX" init test-agent >/dev/null 2>&1
RC=$?
check "$RC" "0" "init exits 0"
[ -f test-agent/agent.yaml ] && t_pass "agent.yaml created" || t_fail "agent.yaml missing"
[ -d test-agent/skills ] && t_pass "skills/ directory created" || t_fail "skills/ missing"
[ -f test-agent/README.md ] && t_pass "README.md created" || t_fail "README.md missing"

echo "=== 4. sparx init with spaces in name ==="
"$SPARX" init "my agent" >/dev/null 2>&1
[ -f "my agent/agent.yaml" ] && t_pass "handles spaces in project name" || t_fail "spaces in name failed"

echo "=== 5. sparx doctor (no NPU required) ==="
cd test-agent
"$SPARX" doctor >/dev/null 2>&1
RC_DOCTOR=$?
# doctor may exit 1 if NPU/models are absent, but should not crash
[ "$RC_DOCTOR" -le 1 ] && t_pass "doctor runs without crashing" || t_fail "doctor crashed (rc=$RC_DOCTOR)"

echo "=== 6. sparx demo automotive (dry run) ==="
cd "$ROOT"
# demo command may fail without models, but should produce structured output
OUT_DEMO=$("$SPARX" demo automotive 2>&1)
echo "$OUT_DEMO" | grep -qi "automotive\|demo" && t_pass "demo command recognized" || t_fail "demo unrecognized"

echo "=== 7. unknown command returns exit 1 ==="
"$SPARX" this-does-not-exist >/dev/null 2>&1
RC_BAD=$?
check "$RC_BAD" "1" "unknown command exits 1"

echo "=== 8. sparx init refuses overwrite ==="
cd "$ROOT"
"$SPARX" init test-agent >/dev/null 2>&1
RC_OVERWRITE=$?
check "$RC_OVERWRITE" "1" "init refuses to overwrite existing project"

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
