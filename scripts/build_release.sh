#!/usr/bin/env bash
#
# Builds one release artifact for the current (or cross-compiled) platform.
#
# Produces, in dist/:
#   sparx-<version>-<target>.tar.gz   the tarball users download
#   sparx-<version>-<target>.tar.gz.sha256
#
# This script is the single source of truth for what a release artifact is.
# install.sh, the Homebrew formula, and the npm package all consume its output,
# so the naming scheme here is a contract — changing it breaks all three.
#
# Usage:
#   ./scripts/build_release.sh                       # host platform
#   SPARX_TARGET=linux-arm64 CC=... ./scripts/build_release.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ---- version ---------------------------------------------------------------
# Prefer an explicit SPARX_VERSION (the release workflow passes the tag).
# Otherwise derive from git describe, and fall back to 0.0.0-dev outside a repo.
if [[ -n "${SPARX_VERSION:-}" ]]; then
    VERSION="$SPARX_VERSION"
elif git rev-parse --git-dir >/dev/null 2>&1; then
    VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo "0.0.0-dev")"
    VERSION="${VERSION#v}"
else
    VERSION="0.0.0-dev"
fi

if git rev-parse --git-dir >/dev/null 2>&1; then
    GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
else
    GIT_SHA="unknown"
fi

# ---- target triple ---------------------------------------------------------
# Normalised to <os>-<arch> with exactly the spellings install.sh probes for.
detect_target() {
    local os arch
    case "$(uname -s)" in
        Darwin) os="darwin" ;;
        Linux)  os="linux" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64) arch="x64" ;;
        arm64|aarch64) arch="arm64" ;;
        *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
    esac
    echo "${os}-${arch}"
}
TARGET="${SPARX_TARGET:-$(detect_target)}"

BUILD_DIR="${SPARX_BUILD_DIR:-$REPO_ROOT/build-release-$TARGET}"
DIST_DIR="$REPO_ROOT/dist"
STAGE_DIR="$BUILD_DIR/stage"

echo "==> sparx release build"
echo "    version : $VERSION"
echo "    commit  : $GIT_SHA"
echo "    target  : $TARGET"
echo "    build   : $BUILD_DIR"

# ---- configure + build -----------------------------------------------------
# Only the CLI is needed for the artifact. Tests are off: a release build is
# not the place to discover a test failure, CI gates that separately before
# this script ever runs.
CMAKE_ARGS=(
    -S "$REPO_ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DMASTER_AGENT_BUILD_CLI=ON
    -DMASTER_AGENT_BUILD_TESTS=OFF
    "-DSPARX_VERSION=$VERSION"
    "-DSPARX_GIT_SHA=$GIT_SHA"
    "-DSPARX_BUILD_TARGET=$TARGET"
)
if command -v ninja >/dev/null 2>&1; then
    CMAKE_ARGS+=(-G Ninja)
fi
if [[ -n "${SPARX_CMAKE_EXTRA:-}" ]]; then
    # shellcheck disable=SC2206
    CMAKE_ARGS+=(${SPARX_CMAKE_EXTRA})
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --target sparx --parallel

BIN="$BUILD_DIR/cli/sparx"
[[ -f "$BIN" ]] || { echo "build did not produce $BIN" >&2; exit 1; }

# ---- strip -----------------------------------------------------------------
# Cuts the artifact roughly in half. Kept non-fatal because a cross-toolchain
# may not ship a matching strip, and an unstripped binary is still correct.
if [[ "$TARGET" == darwin-* ]]; then
    strip -x "$BIN" 2>/dev/null || echo "    (strip skipped)"
else
    "${STRIP:-strip}" "$BIN" 2>/dev/null || echo "    (strip skipped)"
fi

# ---- stage + package -------------------------------------------------------
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/sparx-$VERSION-$TARGET/bin"
cp "$BIN" "$STAGE_DIR/sparx-$VERSION-$TARGET/bin/"
for doc in README.md THIRD_PARTY_NOTICES.md VERSION.json; do
    [[ -f "$doc" ]] && cp "$doc" "$STAGE_DIR/sparx-$VERSION-$TARGET/"
done

mkdir -p "$DIST_DIR"
ARCHIVE="sparx-$VERSION-$TARGET.tar.gz"
tar -czf "$DIST_DIR/$ARCHIVE" -C "$STAGE_DIR" "sparx-$VERSION-$TARGET"

# ---- checksum --------------------------------------------------------------
# Written as "<hash>  <basename>" so `shasum -c` works from inside dist/.
cd "$DIST_DIR"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$ARCHIVE" > "$ARCHIVE.sha256"
else
    shasum -a 256 "$ARCHIVE" > "$ARCHIVE.sha256"
fi

echo "==> $DIST_DIR/$ARCHIVE"
echo "    $(cat "$ARCHIVE.sha256")"
echo "    size: $(du -h "$ARCHIVE" | cut -f1)"
