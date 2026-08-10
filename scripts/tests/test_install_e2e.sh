#!/usr/bin/env bash
#
# End-to-end test of install.sh against a local HTTP server standing in for
# GitHub Releases. Serves the REAL artifact built by build_release.sh, so this
# exercises download -> checksum -> extract -> atomic install -> exec check.
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
# Detect current platform to match build_release.sh output
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
ROOT=/tmp/sparx_e2e
PASS=0; FAIL=0

t_pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
t_fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
check()  { if [ "$1" = "$2" ]; then t_pass "$3"; else t_fail "$3 (expected '$2', got '$1')"; fi; }

rm -rf "$ROOT"; mkdir -p "$ROOT/serve/download/v$VERSION" "$ROOT/api"

# --- fake release host ------------------------------------------------------
cp "$V2/dist/sparx-$VERSION-$TARGET.tar.gz"        "$ROOT/serve/download/v$VERSION/"
cp "$V2/dist/sparx-$VERSION-$TARGET.tar.gz.sha256" "$ROOT/serve/download/v$VERSION/"
# GitHub's /releases/latest API shape, trimmed to what resolve_version parses.
printf '{"tag_name":"v%s","name":"sparx %s","draft":false}\n' "$VERSION" "$VERSION" \
    > "$ROOT/serve/latest.json"

cd "$ROOT/serve"
python3 -m http.server 19555 >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 1.2

BASE="http://127.0.0.1:19555"
export SPARX_BASE_URL="$BASE"
export SPARX_API_URL="$BASE/latest.json"

echo "=== 1. clean install into an empty prefix ==="
export SPARX_INSTALL_DIR="$ROOT/prefix1/bin"
OUT=$(sh "$V2/scripts/install.sh" 2>&1)
RC=$?
echo "$OUT" | sed 's/^/      /'
check "$RC" "0" "installer exits 0"
[ -x "$ROOT/prefix1/bin/sparx" ] && t_pass "binary is executable" || t_fail "binary missing/not executable"
echo "$OUT" | grep -q "checksum verified" && t_pass "checksum was verified" || t_fail "checksum not verified"
echo "$OUT" | grep -q "not on your PATH" && t_pass "PATH guidance shown for off-PATH dir" || t_fail "no PATH guidance"

echo "=== 2. version auto-resolution from the API ==="
echo "$OUT" | grep -q "version:  $VERSION" && t_pass "resolved latest=$VERSION from API" || t_fail "version not resolved"

echo "=== 3. the installed binary actually works ==="
SPX="$ROOT/prefix1/bin/sparx"
check "$("$SPX" version | head -1)" "sparx $VERSION" "sparx version"
"$SPX" --help >/dev/null 2>&1 && t_pass "sparx --help" || t_fail "sparx --help"
cd "$ROOT" && rm -rf wk && mkdir wk && cd wk
"$SPX" init demo >/dev/null 2>&1 && [ -f demo/agent.yaml ] && t_pass "sparx init produces agent.yaml" || t_fail "sparx init"
cd demo && "$SPX" doctor >/dev/null 2>&1; t_pass "sparx doctor ran (rc=$?)"

echo "=== 4. idempotent re-install over an existing binary ==="
OUT2=$(sh "$V2/scripts/install.sh" 2>&1); RC2=$?
check "$RC2" "0" "re-install exits 0"
check "$("$SPX" version | head -1)" "sparx $VERSION" "still works after overwrite"
ls "$ROOT/prefix1/bin/" | grep -q '^\.sparx\.new$' && t_fail "temp file left behind" || t_pass "no temp file left behind"

echo "=== 5. explicit SPARX_VERSION skips the API ==="
export SPARX_INSTALL_DIR="$ROOT/prefix2/bin"
OUT3=$(SPARX_VERSION="$VERSION" SPARX_API_URL="$BASE/does-not-exist.json" \
       sh "$V2/scripts/install.sh" 2>&1); RC3=$?
check "$RC3" "0" "pinned version installs without the API"

echo "=== 6. failure: nonexistent version is rejected ==="
export SPARX_INSTALL_DIR="$ROOT/prefix3/bin"
OUT4=$(SPARX_VERSION=9.9.9 sh "$V2/scripts/install.sh" 2>&1); RC4=$?
check "$RC4" "1" "missing artifact exits 1"
[ -f "$ROOT/prefix3/bin/sparx" ] && t_fail "wrote a binary on failure" || t_pass "no binary written on failure"

echo "=== 7. failure: corrupted checksum is fatal ==="
CORRUPT="$ROOT/serve/download/v8.8.8"
mkdir -p "$CORRUPT"
cp "$V2/dist/sparx-$VERSION-$TARGET.tar.gz" "$CORRUPT/sparx-8.8.8-$TARGET.tar.gz"
echo "0000000000000000000000000000000000000000000000000000000000000000  sparx-8.8.8-$TARGET.tar.gz" \
    > "$CORRUPT/sparx-8.8.8-$TARGET.tar.gz.sha256"
export SPARX_INSTALL_DIR="$ROOT/prefix4/bin"
OUT5=$(SPARX_VERSION=8.8.8 sh "$V2/scripts/install.sh" 2>&1); RC5=$?
check "$RC5" "1" "checksum mismatch exits 1"
echo "$OUT5" | grep -q "MISMATCH" && t_pass "mismatch is reported clearly" || t_fail "mismatch not reported"
[ -f "$ROOT/prefix4/bin/sparx" ] && t_fail "installed despite bad checksum" || t_pass "refused to install bad download"

echo "=== 8. no temp dirs leaked ==="
LEAKED=$(find /tmp -maxdepth 1 -name 'sparx*' -newer "$V2/scripts/install.sh" -type d 2>/dev/null | grep -v sparx_e2e | grep -v sparx_verify | wc -l | tr -d ' ')
check "$LEAKED" "0" "no leaked temp dirs"

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
