#!/usr/bin/env bash
#
# Syncs the version and checksums from dist/ into every packaging channel.
#
# Run after build_release.sh has produced all four artifacts (or after
# downloading them from a release). Without this, the Homebrew formula and npm
# manifests carry stale hashes, which is the single most common way a
# multi-channel release breaks — brew reports a checksum mismatch and npm
# installs the previous version's binary.
#
# Usage:  ./scripts/update_packaging.sh 2.1.0
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
    echo "usage: $0 <version>   e.g. $0 2.1.0" >&2
    exit 1
fi
VERSION="${VERSION#v}"

DIST="$REPO_ROOT/dist"
TARGETS=(darwin-arm64 darwin-x64 linux-arm64 linux-x64)

sha_for() {
    local target="$1"
    local f="$DIST/sparx-$VERSION-$target.tar.gz"
    if [[ ! -f "$f" ]]; then
        echo "MISSING"; return
    fi
    if [[ -f "$f.sha256" ]]; then
        cut -d' ' -f1 < "$f.sha256"
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$f" | cut -d' ' -f1
    else
        shasum -a 256 "$f" | cut -d' ' -f1
    fi
}

echo "==> syncing packaging to v$VERSION"

# ---- Homebrew --------------------------------------------------------------
FORMULA="packaging/homebrew/sparx.rb"
echo "    $FORMULA"
# Uses a temp file + mv rather than sed -i, because BSD and GNU sed disagree on
# -i's argument and this script runs on both macOS and Linux CI.
cp "$FORMULA" "$FORMULA.tmp"
# Version appears in the version stanza and inside every URL.
sed -e "s|version \"[0-9][^\"]*\"|version \"$VERSION\"|" \
    -e "s|/download/v[0-9][^/]*/|/download/v$VERSION/|g" \
    -e "s|sparx-[0-9][^-]*-\(darwin\|linux\)-|sparx-$VERSION-\1-|g" \
    "$FORMULA.tmp" > "$FORMULA"
rm -f "$FORMULA.tmp"

for t in "${TARGETS[@]}"; do
    sha="$(sha_for "$t")"
    key="REPLACE_$(echo "$t" | tr '[:lower:]-' '[:upper:]_')_SHA256"
    if [[ "$sha" == "MISSING" ]]; then
        echo "      ! $t artifact missing — placeholder left in place"
        continue
    fi
    # Replace either the placeholder or a previously-written hash. The formula
    # lists targets in a fixed order, so anchor on the preceding URL line.
    awk -v target="$t" -v newsha="$sha" '
        /url .*sparx-.*\.tar\.gz"/ { in_block = ($0 ~ target) }
        /sha256 "/ && in_block { sub(/sha256 "[^"]*"/, "sha256 \"" newsha "\""); in_block=0 }
        { print }
    ' "$FORMULA" > "$FORMULA.tmp" && mv "$FORMULA.tmp" "$FORMULA"
    echo "      $t  ${sha:0:16}…"
done

# ---- npm root package ------------------------------------------------------
NPM_PKG="packaging/npm/package.json"
echo "    $NPM_PKG"
python3 - "$NPM_PKG" "$VERSION" <<'PY'
import json, sys
path, version = sys.argv[1], sys.argv[2]
with open(path) as f:
    pkg = json.load(f)
pkg["version"] = version
pkg["optionalDependencies"] = {
    k: version for k in pkg.get("optionalDependencies", {})
}
with open(path, "w") as f:
    json.dump(pkg, f, indent=2)
    f.write("\n")
PY

# ---- npm platform sub-packages --------------------------------------------
# Generated rather than checked in: there are four of them, they differ only in
# three fields, and hand-maintaining them is how versions drift apart.
for t in "${TARGETS[@]}"; do
    os="${t%-*}"; arch="${t#*-}"
    outdir="packaging/npm/platforms/$t"
    mkdir -p "$outdir/bin"
    cat > "$outdir/package.json" <<EOF
{
  "name": "@sparx/cli-$t",
  "version": "$VERSION",
  "description": "sparx binary for $t",
  "license": "Apache-2.0",
  "repository": {
    "type": "git",
    "url": "git+https://github.com/OpenSparX/MasterAgent.git"
  },
  "os": ["$os"],
  "cpu": ["$arch"],
  "files": ["bin/sparx"],
  "preferUnplugged": true
}
EOF
    # Stage the actual binary when the artifact is present, so `npm publish`
    # from this directory produces a complete package.
    archive="$DIST/sparx-$VERSION-$t.tar.gz"
    if [[ -f "$archive" ]]; then
        tmp="$(mktemp -d)"
        tar -xzf "$archive" -C "$tmp"
        cp "$tmp/sparx-$VERSION-$t/bin/sparx" "$outdir/bin/sparx"
        chmod 755 "$outdir/bin/sparx"
        rm -rf "$tmp"
        echo "    $outdir  (binary staged)"
    else
        echo "    $outdir  (manifest only — artifact missing)"
    fi
done

echo
echo "==> done. Review the diff before publishing:"
echo "    git diff packaging/"
