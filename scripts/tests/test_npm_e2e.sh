#!/usr/bin/env bash
#
# Tests the npm channel by assembling the node_modules layout npm would produce
# and driving the real launcher. Verifies resolution, arg passing, exit codes,
# and the failure message when the platform package is absent.
#
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V2="$REPO_ROOT"
if [[ -n "${SPARX_VERSION:-}" ]]; then
  VERSION="$SPARX_VERSION"
else
  VERSION=$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')
  VERSION="${VERSION:-2.1.0}"
fi
# Detect current platform to match npm platform packages
detect_target() {
    local os arch
    case "$(uname -s)" in
        Darwin) os="darwin" ;;
        Linux)  os="linux" ;;
        *) os="unknown" ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64) arch="x64" ;;
        arm64|aarch64) arch="arm64" ;;
        *) arch="unknown" ;;
    esac
    echo "${os}-${arch}"
}
TARGET=$(detect_target)
ROOT=/tmp/sparx_npm_e2e
PASS=0; FAIL=0
t_pass(){ echo "  PASS  $1"; PASS=$((PASS+1)); }
t_fail(){ echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
check(){ if [ "$1" = "$2" ]; then t_pass "$3"; else t_fail "$3 (want '$2', got '$1')"; fi; }

echo "=== node version ==="
node --version || { echo "  node absent - cannot test npm channel"; exit 2; }

rm -rf "$ROOT"; mkdir -p "$ROOT/pkg"
# Root package as npm would install it.
cp "$V2/packaging/npm/package.json" "$ROOT/pkg/"
cp "$V2/packaging/npm/install.js"   "$ROOT/pkg/"
mkdir -p "$ROOT/pkg/bin"
cp "$V2/packaging/npm/bin/sparx.js" "$ROOT/pkg/bin/"

# Hoisted platform package (npm's normal layout).
PLATDIR="$ROOT/pkg/node_modules/@sparx/cli-$TARGET"
mkdir -p "$PLATDIR/bin"
# Use the release-built binary from dist/ (has the correct version stamp).
# Fall back to the platforms/ tree only for local dev where dist/ doesn't exist.
if [ -f "$V2/dist/sparx-$VERSION-$TARGET.tar.gz" ]; then
  tar -xzf "$V2/dist/sparx-$VERSION-$TARGET.tar.gz" -C /tmp "sparx-$VERSION-$TARGET/bin/sparx"
  cp "/tmp/sparx-$VERSION-$TARGET/bin/sparx" "$PLATDIR/bin/"
elif [ -f "$V2/packaging/npm/platforms/$TARGET/bin/sparx" ]; then
  cp "$V2/packaging/npm/platforms/$TARGET/bin/sparx" "$PLATDIR/bin/"
else
  echo "  SKIP  no binary available for $TARGET - skipping npm e2e"
  exit 0
fi
# Ensure the platform package.json exists
if [ -f "$V2/packaging/npm/platforms/$TARGET/package.json" ]; then
  cp "$V2/packaging/npm/platforms/$TARGET/package.json" "$PLATDIR/"
else
  # Generate a minimal one for the current platform
  cat > "$PLATDIR/package.json" <<PKGJSON
{
  "name": "@sparx/cli-$TARGET",
  "version": "$VERSION",
  "os": ["${TARGET%-*}"],
  "cpu": ["${TARGET#*-}"],
  "bin": { "sparx": "bin/sparx" }
}
PKGJSON
fi
chmod 755 "$PLATDIR/bin/sparx"

cd "$ROOT/pkg"

echo "=== 1. postinstall ==="
OUT=$(node install.js 2>&1); RC=$?
echo "$OUT" | sed 's/^/      /'
check "$RC" "0" "postinstall exits 0"
echo "$OUT" | grep -q "sparx ready ($TARGET)" && t_pass "reports ready with triple" || t_fail "no ready message"

echo "=== 2. launcher passes through to the binary ==="
check "$(node bin/sparx.js version 2>&1 | head -1)" "sparx $VERSION" "sparx version via launcher"
node bin/sparx.js --help >/dev/null 2>&1 && t_pass "--help via launcher" || t_fail "--help via launcher"

echo "=== 3. exit codes propagate ==="
node bin/sparx.js this-command-does-not-exist >/dev/null 2>&1
check "$?" "1" "unknown command -> exit 1"
node bin/sparx.js version >/dev/null 2>&1
check "$?" "0" "success -> exit 0"

echo "=== 4. arguments with spaces survive ==="
rm -rf "$ROOT/wk"; mkdir -p "$ROOT/wk"; cd "$ROOT/wk"
node "$ROOT/pkg/bin/sparx.js" init "my agent" >/dev/null 2>&1
[ -f "my agent/agent.yaml" ] && t_pass "quoted arg with space passed intact" || t_fail "arg with space mangled"
cd "$ROOT/pkg"

echo "=== 5. stdout is a real pipe, not buffered by node ==="
LINES=$(node bin/sparx.js --help 2>&1 | wc -l | tr -d ' ')
[ "$LINES" -gt 10 ] && t_pass "full help output relayed ($LINES lines)" || t_fail "output truncated ($LINES lines)"

echo "=== 6. missing platform package gives actionable error ==="
mv "$PLATDIR" "$PLATDIR.hidden"
OUT2=$(node bin/sparx.js version 2>&1); RC2=$?
check "$RC2" "1" "missing binary -> exit 1"
echo "$OUT2" | grep -q "could not find the sparx binary" && t_pass "explains what is missing" || t_fail "unclear error"
echo "$OUT2" | grep -q "install.sh" && t_pass "offers the curl fallback" || t_fail "no fallback offered"
# postinstall must warn but NOT fail the install
node install.js >/dev/null 2>&1
check "$?" "0" "postinstall warns without failing npm install"
mv "$PLATDIR.hidden" "$PLATDIR"

echo "=== 7. platform manifest gating ==="
for t in darwin-arm64 darwin-x64 linux-arm64 linux-x64; do
  f="$V2/packaging/npm/platforms/$t/package.json"
  os="${t%-*}"; arch="${t#*-}"
  python3 -c "
import json,sys
p=json.load(open('$f'))
assert p['os']==['$os'], p['os']
assert p['cpu']==['$arch'], p['cpu']
assert p['version']=='$VERSION', p['version']
assert p['name']=='@sparx/cli-$t', p['name']
" 2>/dev/null && t_pass "$t manifest os/cpu/version correct" || t_fail "$t manifest wrong"
done

echo "=== 8. root package optionalDependencies match version ==="
python3 -c "
import json
p=json.load(open('$V2/packaging/npm/package.json'))
od=p['optionalDependencies']
assert p['version']=='$VERSION', p['version']
assert len(od)==4, od
for k,v in od.items(): assert v=='$VERSION', (k,v)
" 2>/dev/null && t_pass "optionalDependencies pinned to $VERSION" || t_fail "optionalDependencies drift"

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
