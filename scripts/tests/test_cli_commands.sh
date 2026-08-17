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

echo "=== 14. sparx plan validates and exports ==="
PLAN_FILE="$V2/examples/automotive_assistant/plans/turn-off-ac.yaml"
if [ -f "$PLAN_FILE" ]; then
  "$SPARX" plan validate "$PLAN_FILE" >/dev/null 2>&1
  check "$?" "0" "plan validate exits 0 for valid plan"

  OUT_PLAN=$("$SPARX" plan show "$PLAN_FILE" 2>&1)
  echo "$OUT_PLAN" | grep -q "✓ valid" && t_pass "plan show marks valid" \
    || t_fail "plan show missing valid marker"
  echo "$OUT_PLAN" | grep -q "read_temp" && t_pass "plan show lists nodes" \
    || t_fail "plan show missing node"

  OUT_MERMAID=$("$SPARX" plan export "$PLAN_FILE" --format=mermaid 2>&1)
  echo "$OUT_MERMAID" | grep -q "flowchart TD" && t_pass "mermaid export" \
    || t_fail "mermaid export broken"

  OUT_JSON=$("$SPARX" plan export "$PLAN_FILE" --format=json 2>&1)
  echo "$OUT_JSON" | grep -q '"schema_version"' && t_pass "json export" \
    || t_fail "json export broken"

  # Invalid plan (P0 without authorization) must be rejected.
  TMP_P0="/tmp/sparx_test_invalid_p0.yaml"
  cat > "$TMP_P0" <<'YML'
plan: test-invalid-p0
priority: p0
deadline_ms: 1000
nodes:
  - id: brake
    action: vehicle.safety.brake
YML
  "$SPARX" plan validate "$TMP_P0" >/dev/null 2>&1
  check "$?" "1" "plan validate rejects P0 without authorization"
  rm -f "$TMP_P0"
else
  echo "  SKIP  plan spec not found: $PLAN_FILE"
fi

echo "=== 15. sparx plan --help ==="
"$SPARX" plan --help >/dev/null 2>&1 && t_pass "plan --help exits 0" \
  || t_fail "plan --help failed"
"$SPARX" plan >/dev/null 2>&1 && t_pass "plan with no args exits 0 (shows help)" \
  || t_fail "plan with no args should show help"

echo "=== 16. sparx demo plan ==="
OUT_DEMO_PLAN=$("$SPARX" demo plan 2>&1)
check "$?" "0" "demo plan exits 0"
echo "$OUT_DEMO_PLAN" | grep -q "Execution Plan Demo" && t_pass "demo plan header" \
  || t_fail "demo plan missing header"
echo "$OUT_DEMO_PLAN" | grep -q "DAG visualization" && t_pass "demo plan shows DAG" \
  || t_fail "demo plan missing DAG"
echo "$OUT_DEMO_PLAN" | grep -q "Plan VALID" && t_pass "demo plan validates" \
  || t_fail "demo plan missing validation"
echo "$OUT_DEMO_PLAN" | grep -q "DISPATCH" && t_pass "demo plan shows execution" \
  || t_fail "demo plan missing execution trace"

echo "=== 17. sparx trace reads runtime TaskEvent records ==="
TRACE_FILE="/tmp/sparx_test_trace.jsonl"
cat > "$TRACE_FILE" <<'JSONL'
{"event_id":"e1","event_type":"PLAN_COMMITTED","plan_id":"plan-1","pid":"","activation_id":"","execution_id":"","plan_version":1,"orchestrator_epoch":1,"occurred_at_utc_ms":1785200000000,"payload_digest":"d1","trace_id":"t1"}
{"event_id":"e2","event_type":"NODE_READY","plan_id":"plan-1","pid":"pid-1","activation_id":"a1","execution_id":"x1","plan_version":2,"orchestrator_epoch":1,"occurred_at_utc_ms":1785200000010,"payload_digest":"d2","trace_id":"t1"}
{"event_id":"e3","event_type":"PLAN_TERMINAL","plan_id":"plan-2","pid":"","activation_id":"","execution_id":"","plan_version":3,"orchestrator_epoch":1,"occurred_at_utc_ms":1785200000020,"payload_digest":"d3","trace_id":"t2"}
JSONL

OUT_TRACE=$("$SPARX" trace show "$TRACE_FILE" 2>&1)
check "$?" "0" "trace show exits 0"
echo "$OUT_TRACE" | grep -q "3 events" && t_pass "trace show counts events" \
  || t_fail "trace show wrong event count"
echo "$OUT_TRACE" | grep -q "PLAN_COMMITTED" && t_pass "trace show lists event types" \
  || t_fail "trace show missing event type"

# A plan selector must narrow the projection, not silently return everything.
OUT_FILTERED=$("$SPARX" trace show "$TRACE_FILE" --plan plan-1 2>&1)
echo "$OUT_FILTERED" | grep -q "2 events" && t_pass "trace --plan filters events" \
  || t_fail "trace --plan did not filter"
echo "$OUT_FILTERED" | grep -q "PLAN_TERMINAL" \
  && t_fail "trace --plan leaked another plan's events" \
  || t_pass "trace --plan excludes other plans"

OUT_EXEC=$("$SPARX" trace show "$TRACE_FILE" --execution x1 2>&1)
echo "$OUT_EXEC" | grep -q "1 event" && t_pass "trace --execution filters events" \
  || t_fail "trace --execution did not filter"

OUT_CAP=$("$SPARX" trace show "$TRACE_FILE" --max-records 2 2>&1)
echo "$OUT_CAP" | grep -q "2 events" && t_pass "trace --max-records bounds output" \
  || t_fail "trace --max-records ignored"

OUT_TRACE_JSON=$("$SPARX" trace export "$TRACE_FILE" --format=json 2>&1)
echo "$OUT_TRACE_JSON" | grep -q '"event_type": "PLAN_COMMITTED"' \
  && t_pass "trace export emits JSON" || t_fail "trace export JSON broken"

# A JSON array is accepted so exported traces can be re-read.
echo "$OUT_TRACE_JSON" > /tmp/sparx_test_trace_array.json
"$SPARX" trace show /tmp/sparx_test_trace_array.json >/dev/null 2>&1
check "$?" "0" "trace show accepts a JSON array"

"$SPARX" trace show /tmp/sparx_test_trace_missing.jsonl >/dev/null 2>&1
check "$?" "1" "trace show exits 1 for a missing file"

# An incomplete record must fail loudly rather than render as a partial trace.
printf '{"event_id":"only-id"}\n' > /tmp/sparx_test_trace_bad.jsonl
"$SPARX" trace show /tmp/sparx_test_trace_bad.jsonl >/dev/null 2>&1
check "$?" "1" "trace show rejects an incomplete record"

"$SPARX" trace show "$TRACE_FILE" --format=yaml >/dev/null 2>&1
check "$?" "1" "trace show rejects an unknown format"

"$SPARX" trace >/dev/null 2>&1 && t_pass "trace with no args exits 0 (shows help)" \
  || t_fail "trace with no args should show help"
"$SPARX" trace bogus >/dev/null 2>&1
check "$?" "1" "unknown trace subcommand exits 1"

rm -f "$TRACE_FILE" /tmp/sparx_test_trace_array.json /tmp/sparx_test_trace_bad.jsonl

echo "=== 18. sparx help lists trace ==="
echo "$HELP_OUT" | grep -q "trace" && t_pass "help shows trace command" \
  || t_fail "help missing trace"

echo "=== 19. sparx pull lists models with default marked ==="
OUT_PULL=$("$SPARX" pull 2>&1)
check "$?" "0" "pull with no args shows help"
echo "$OUT_PULL" | grep -q "qwen2.5-0.5b-instruct" && t_pass "pull lists qwen2.5-0.5b" \
  || t_fail "pull missing qwen2.5-0.5b"
echo "$OUT_PULL" | grep -q "← start here" && t_pass "pull marks default model" \
  || t_fail "pull does not mark default"
echo "$OUT_PULL" | grep -q "models land in" && t_pass "pull shows models directory" \
  || t_fail "pull missing models directory"

echo "=== 20. sparx pull rejects unknown model names ==="
OUT_BAD=$("$SPARX" pull nonexistent-model 2>&1)
check "$?" "1" "pull unknown model exits 1"
echo "$OUT_BAD" | grep -q "✗ unknown model" && t_pass "pull reports unknown model" \
  || t_fail "pull error message missing"
echo "$OUT_BAD" | grep -q "known names:" && t_pass "pull lists known names inline" \
  || t_fail "pull does not list alternatives"

echo "=== 21. sparx run no-model banner mentions sparx pull ==="
OUT_NO_MODEL=$(printf '\n' | "$SPARX" run 2>&1)
echo "$OUT_NO_MODEL" | grep -q "sparx pull" && t_pass "no-model banner mentions pull" \
  || t_fail "no-model banner missing pull command"
echo "$OUT_NO_MODEL" | grep -q "qwen2.5-0.5b-instruct" \
  && t_pass "no-model banner names default model" \
  || t_fail "no-model banner missing model name"

echo "=== 22. sparx run simulation stub mentions sparx pull ==="
OUT_STUB=$(printf 'tell me about quantum computing\n' | "$SPARX" run 2>&1)
echo "$OUT_STUB" | grep -q "sparx pull" && t_pass "stub response mentions pull" \
  || t_fail "stub response missing pull hint"

echo "=== 23. sparx learn shows help with no args ==="
OUT_LEARN=$("$SPARX" learn 2>&1)
check "$?" "0" "learn with no args exits 0"
echo "$OUT_LEARN" | grep -q "correct" && t_pass "learn help lists correct subcommand" \
  || t_fail "learn help missing correct"
echo "$OUT_LEARN" | grep -q "status" && t_pass "learn help lists status subcommand" \
  || t_fail "learn help missing status"
echo "$OUT_LEARN" | grep -q "on-device" && t_pass "learn help mentions on-device" \
  || t_fail "learn help missing on-device context"

echo "=== 24. sparx learn correct requires arguments ==="
OUT_CORRECT=$("$SPARX" learn correct 2>&1)
check "$?" "1" "learn correct with no args exits 1"
echo "$OUT_CORRECT" | grep -q "usage" && t_pass "learn correct shows usage" \
  || t_fail "learn correct missing usage hint"

echo "=== 25. sparx learn status works without agent.yaml ==="
OUT_STATUS=$(cd /tmp && "$SPARX" learn status 2>&1)
check "$?" "0" "learn status exits 0"
echo "$OUT_STATUS" | grep -q "training pairs" && t_pass "learn status shows pair count" \
  || t_fail "learn status missing pair count"

echo "=== 26. sparx learn correct records a pair ==="
LEARN_TMPDIR=$(mktemp -d)
cd "$LEARN_TMPDIR"
cat > agent.yaml <<'EOF'
name: test-learn-agent
skills: []
EOF
OUT_PAIR=$("$SPARX" learn correct "turn on AC" "Setting AC to 22°C" 2>&1)
check "$?" "0" "learn correct exits 0"
echo "$OUT_PAIR" | grep -q "correction recorded" && t_pass "learn correct confirms save" \
  || t_fail "learn correct missing confirmation"
echo "$OUT_PAIR" | grep -q "/5" && t_pass "learn correct shows pair count" \
  || t_fail "learn correct missing count"
rm -rf "$LEARN_TMPDIR"
cd "$REPO_ROOT"

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
