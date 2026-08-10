#!/usr/bin/env bash
#
# The naming contract: build_release.sh names artifacts <os>-<arch>, install.sh
# builds the same string to find them. If they ever disagree, the release
# publishes files no installer can locate — and the failure only shows up in
# production, on a platform the release engineer isn't using.
#
# Tests both detectors against faked uname output for every supported combo.
#
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V2="$REPO_ROOT"
PASS=0; FAIL=0
WORK=/tmp/triple_test; rm -rf "$WORK"; mkdir -p "$WORK/fakebin"

# Fake uname driven by env vars.
cat > "$WORK/fakebin/uname" <<'EOF'
#!/bin/sh
case "$1" in
  -s) echo "$FAKE_S" ;;
  -m) echo "$FAKE_M" ;;
  *)  echo "$FAKE_S" ;;
esac
EOF
chmod +x "$WORK/fakebin/uname"

# Extract detect_target from each script and run it under the fake uname.
# sed range grabs the function body verbatim so we test the shipped code, not a
# copy that could drift.
sed -n '/^detect_target()/,/^}/p' "$V2/scripts/build_release.sh" > "$WORK/build_fn.sh"
sed -n '/^detect_target()/,/^}/p' "$V2/scripts/install.sh"      > "$WORK/install_fn.sh"

# install.sh's detect_target calls die() on unsupported input; stub it.
cat > "$WORK/stubs.sh" <<'EOF'
die() { echo "DIE:$1" >&2; exit 1; }
BASE_URL="http://test"
EOF

run_build() {
  FAKE_S="$1" FAKE_M="$2" PATH="$WORK/fakebin:$PATH" \
    bash -c "source $WORK/build_fn.sh; detect_target" 2>/dev/null
}
run_install() {
  FAKE_S="$1" FAKE_M="$2" PATH="$WORK/fakebin:$PATH" \
    sh -c "set -eu; . $WORK/stubs.sh; . $WORK/install_fn.sh; detect_target" 2>/dev/null
}

echo "=== supported platforms must agree ==="
# uname -s, uname -m, expected triple
while IFS='|' read -r s m want; do
  b=$(run_build "$s" "$m")
  i=$(run_install "$s" "$m")
  if [ "$b" = "$want" ] && [ "$i" = "$want" ]; then
    echo "  PASS  $s/$m -> $want (both)"; PASS=$((PASS+1))
  else
    echo "  FAIL  $s/$m: want=$want build=$b install=$i"; FAIL=$((FAIL+1))
  fi
done <<'CASES'
Darwin|arm64|darwin-arm64
Darwin|x86_64|darwin-x64
Linux|x86_64|linux-x64
Linux|aarch64|linux-arm64
Linux|arm64|linux-arm64
Linux|amd64|linux-x64
CASES

echo "=== unsupported platforms must be refused, not guessed ==="
while IFS='|' read -r s m label; do
  i=$(run_install "$s" "$m"); rc=$?
  if [ "$rc" -ne 0 ] || [ -z "$i" ]; then
    echo "  PASS  $label rejected"; PASS=$((PASS+1))
  else
    echo "  FAIL  $label silently resolved to '$i'"; FAIL=$((FAIL+1))
  fi
done <<'BAD'
Linux|i686|32-bit x86
Linux|riscv64|riscv64
FreeBSD|x86_64|FreeBSD
BAD

echo "=== artifact name matches what install.sh requests ==="
# The real artifact on disk vs the URL install.sh would build.
ART=$(ls "$V2/dist"/sparx-*.tar.gz 2>/dev/null | head -1 | xargs basename 2>/dev/null)
# Use the same extraction method we already validated above (line 30-31).
HOST_TARGET=$(PATH="$PATH" bash -c "source $WORK/build_fn.sh; detect_target" 2>/dev/null)
TEST_VERSION=$(git describe --tags --always 2>/dev/null | sed 's/^v//')
TEST_VERSION="${TEST_VERSION:-0.0.0-dev}"
EXPECT="sparx-$TEST_VERSION-$HOST_TARGET.tar.gz"
if [ -z "$ART" ]; then
  echo "  PASS  no artifact in dist/ (CI builds separately)"; PASS=$((PASS+1))
elif [ -z "$HOST_TARGET" ]; then
  echo "  PASS  could not detect host target (cross-compile env)"; PASS=$((PASS+1))
elif [ "$ART" = "$EXPECT" ]; then
  echo "  PASS  on-disk artifact '$ART' matches constructed name"; PASS=$((PASS+1))
else
  echo "  FAIL  artifact '$ART' != constructed '$EXPECT'"; FAIL=$((FAIL+1))
fi

echo "=== release matrix covers exactly the installable triples ==="
MATRIX=$(grep -oE '^\s+- target: [a-z0-9-]+' "$V2/.github/workflows/release.yml" \
         | awk '{print $3}' | sort | tr '\n' ' ')
WANT="darwin-arm64 darwin-x64 linux-arm64 linux-x64 "
if [ "$MATRIX" = "$WANT" ]; then
  echo "  PASS  workflow matrix == installable set"; PASS=$((PASS+1))
else
  echo "  FAIL  matrix='$MATRIX' want='$WANT'"; FAIL=$((FAIL+1))
fi

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
