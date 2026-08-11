#!/usr/bin/env bash
#
# Synchronizes the version field in all npm package.json files to match the
# current git tag. Called by CI before packaging tests and before publish.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -n "${SPARX_VERSION:-}" ]]; then
  VERSION="$SPARX_VERSION"
else
  # --abbrev=0 gives the nearest tag with no commit-distance suffix. Plain
  # `describe` would yield 2.1.13-1-gf5ba4eb one commit after a tag, and writing
  # that into package.json makes the manifests disagree with every release
  # artifact. npm accepts it as a prerelease, so nothing complains here — the
  # damage only shows up as manifest mismatches later.
  VERSION=$(git -C "$REPO_ROOT" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
  VERSION="${VERSION:-0.0.0-dev}"
fi

# Refuse anything npm would not accept as a version.
#
# This guard exists because a bad version here is silent: npm accepts it, the
# files are written, and the failure surfaces much later as unrelated-looking
# "manifest wrong" errors. It previously caught `git describe --tags --always`
# returning a bare commit SHA ("7b3ee26") in CI, where actions/checkout does not
# fetch tags unless asked — that blocked the v2.1.10 release. The describe call
# above no longer produces a SHA, so the remaining live case is a malformed
# SPARX_VERSION passed in from a workflow.
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
  echo "sync_npm_version.sh: refusing to write '$VERSION' as an npm version." >&2
  echo "" >&2
  echo "  Expected MAJOR.MINOR.PATCH (optionally -prerelease)." >&2
  echo "  If SPARX_VERSION is set, check the value the workflow passed." >&2
  echo "  If it is unset, no git tag was reachable — check out with" >&2
  echo "  fetch-depth: 0 so tags are available." >&2
  exit 1
fi

echo "Syncing npm package versions to $VERSION"

# Root package
ROOT_PKG="$REPO_ROOT/packaging/npm/package.json"
# The trailing newline matters: update_packaging.sh writes one, so omitting it
# here makes the two scripts alternately dirty the same files.
python3 -c "
import json, sys
p = json.load(open('$ROOT_PKG'))
p['version'] = '$VERSION'
for k in list(p.get('optionalDependencies', {}).keys()):
    p['optionalDependencies'][k] = '$VERSION'
with open('$ROOT_PKG', 'w') as f:
    json.dump(p, f, indent=2)
    f.write('\n')
print('  ✓ ' + '$ROOT_PKG')
"

# Platform packages
for dir in "$REPO_ROOT"/packaging/npm/platforms/*/; do
  pkg="$dir/package.json"
  [ -f "$pkg" ] || continue
  python3 -c "
import json
p = json.load(open('$pkg'))
p['version'] = '$VERSION'
with open('$pkg', 'w') as f:
    json.dump(p, f, indent=2)
    f.write('\n')
print('  ✓ ' + '$pkg')
"
done

echo "Done — all npm packages at $VERSION"
