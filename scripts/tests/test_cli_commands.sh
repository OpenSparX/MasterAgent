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

# The CLI target installs to build/cli/sparx (see cli/CMakeLists.txt), not
# build/sparx. Prefer an already-built binary; fall back to a dist/ one.
find_sparx() {
  for c in "$V2/build/cli/sparx" "$V2/build/sparx" "$V2/dist/sparx"; do
    [ -x "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

if ! SPARX="$(find_sparx)"; then
  echo "=== Building sparx CLI ==="
  cmake -S "$V2" -B "$V2/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || {
    echo "  FAIL  cmake configure failed"
    exit 1
  }
  cmake --build "$V2/build" --target sparx --parallel >/dev/null 2>&1 || {
    echo "  FAIL  build of target 'sparx' failed"
    exit 1
  }
  SPARX="$(find_sparx)" || {
    echo "  FAIL  build succeeded but no sparx binary found"
    exit 1
  }
fi
echo "=== Using sparx: ${SPARX#$V2/} ==="

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

echo "=== 9. sparx add skill scaffolds and registers ==="
cd "$ROOT/test-agent"
"$SPARX" add skill climate >/dev/null 2>&1
check "$?" "0" "add skill exits 0"
[ -f skills/climate.yaml ] && t_pass "skills/climate.yaml created" \
  || t_fail "skills/climate.yaml missing"
# Registration is the half developers forget by hand; an unregistered skill is
# silently inert, so assert the agent.yaml edit specifically.
grep -q "^  - climate$" agent.yaml && t_pass "climate registered in agent.yaml" \
  || t_fail "climate not registered in agent.yaml"
# The pre-existing entry must survive the rewrite.
grep -q "^  - hello$" agent.yaml && t_pass "existing skill entry preserved" \
  || t_fail "existing skill entry lost"
# Comments are why a hand-written agent.yaml is readable, and the line-wise
# rewrite must keep them. The init template ships none, so seed one first —
# otherwise this asserts nothing.
cd "$ROOT"
mkdir -p comment-agent/skills
cat > comment-agent/agent.yaml <<'CFG'
# top-level comment
name: comment-agent
version: "0.1.0"

skills:
  # which skills are registered
  - hello

reliability: D2
CFG
cd comment-agent
"$SPARX" add skill extra >/dev/null 2>&1
grep -q "^# top-level comment$" agent.yaml \
  && grep -q "# which skills are registered" agent.yaml \
  && t_pass "agent.yaml comments preserved" \
  || t_fail "agent.yaml comments stripped"
# The new entry must land inside the skills block, after the last item.
grep -q "^  - extra$" agent.yaml && t_pass "entry appended to skills block" \
  || t_fail "entry not appended correctly"
# and must not be pushed past the block into an unrelated key
awk '/^skills:/{f=1;next} /^[a-z]/{f=0} f&&/- extra/{found=1} END{exit !found}' agent.yaml \
  && t_pass "entry stayed within skills block" \
  || t_fail "entry escaped the skills block"
cd "$ROOT/test-agent"

echo "=== 10. sparx add skill rejects unsafe and duplicate names ==="
"$SPARX" add skill climate >/dev/null 2>&1
check "$?" "1" "duplicate skill refused"
"$SPARX" add skill "../escape" >/dev/null 2>&1
check "$?" "1" "path traversal in skill name refused"
[ -f "$ROOT/escape.yaml" ] && t_fail "traversal wrote outside skills/" \
  || t_pass "no file written outside skills/"
"$SPARX" add skill >/dev/null 2>&1
check "$?" "1" "missing skill name exits 1"
"$SPARX" add widget foo >/dev/null 2>&1
check "$?" "1" "unknown add kind exits 1"

echo "=== 11. edited skill yaml actually fires ==="
# The whole point of the scaffold: edit patterns, and the skill routes
# deterministically. Before the loader existed this silently never matched.
cat > skills/climate.yaml <<'SKILL'
name: climate
description: "test"
trigger:
  patterns:
    - "aircon"
handler:
  type: deterministic
  response: "AC on."
SKILL
OUT_SKILL=$(printf 'aircon please\n' | "$SPARX" run 2>&1)
echo "$OUT_SKILL" | grep -q "route=deterministic  skill=climate" \
  && t_pass "edited skill routes deterministically" \
  || t_fail "edited skill did not fire"
echo "$OUT_SKILL" | grep -q "AC on." && t_pass "skill response emitted" \
  || t_fail "skill response missing"
echo "$OUT_SKILL" | grep -q "skills: 2/2 loaded" && t_pass "loader counts skills" \
  || t_fail "loader count wrong"

echo "=== 12. run declares simulation vs real honestly ==="
OUT_SIM=$(printf '\n' | "$SPARX" run 2>&1)
echo "$OUT_SIM" | grep -q "reality=SIMULATED" && t_pass "no model -> SIMULATED" \
  || t_fail "missing SIMULATED label"
echo "$OUT_SIM" | grep -q "runtime=none" && t_pass "no model -> runtime=none" \
  || t_fail "runtime label wrong with no model"
# A hardcoded version in the banner drifts from the real one; assert they agree.
BANNER_VER=$(echo "$OUT_SIM" | sed -n 's/.*OpenSparX v\([^ ]*\) .*/\1/p' | head -1)
REAL_VER=$("$SPARX" version 2>&1 | head -1 | awk '{print $2}')
check "$BANNER_VER" "$REAL_VER" "run banner version matches sparx version"
# --resume must not claim WAL recovery it did not perform.
OUT_RESUME=$(printf '\n' | "$SPARX" run --resume 2>&1)
echo "$OUT_RESUME" | grep -q "torn tail repaired" \
  && t_fail "--resume still claims false WAL recovery" \
  || t_pass "--resume makes no false WAL claim"
# A missing model file must fail loudly, not fall back to simulation.
printf '\n' | "$SPARX" run --model /nonexistent-model.gguf >/dev/null 2>&1
check "$?" "1" "missing --model file exits 1"

echo "=== 13. shipped examples fully load their skills ==="
for ex in "$V2"/examples/*/; do
  name=$(basename "$ex")
  OUT_EX=$(cd "$ex" && printf '\n' | "$SPARX" run 2>&1 | grep "skills:")
  if echo "$OUT_EX" | grep -q "no yaml:"; then
    t_fail "$name registers skills with no yaml file: $OUT_EX"
  else
    t_pass "$name skills all present"
  fi
done

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
