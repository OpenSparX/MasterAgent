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
  VERSION=$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')
  VERSION="${VERSION:-0.0.0-dev}"
fi

# Refuse anything npm would not accept as a version.
#
# `git describe --tags --always` falls back to a bare commit SHA when no tag is
# reachable — which is exactly what happens in CI, because actions/checkout does
# not fetch tags unless asked. That silently wrote "7b3ee26" into every
# package.json and blocked the v2.1.10 release with five confusing "manifest
# wrong" failures far downstream. Failing here names the actual problem.
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
  echo "sync_npm_version.sh: refusing to write '$VERSION' as an npm version." >&2
  echo "" >&2
  echo "  Expected MAJOR.MINOR.PATCH (optionally -prerelease)." >&2
  echo "  This usually means SPARX_VERSION was not set and 'git describe'" >&2
  echo "  returned a commit SHA because no tag was reachable. In CI, either" >&2
  echo "  set SPARX_VERSION or check out with fetch-depth: 0." >&2
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
