#!/usr/bin/env bash
#
# Synchronizes the version field in all npm package.json files to match the
# current git tag. Called by CI before packaging tests and before publish.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')
VERSION="${VERSION:-0.0.0-dev}"

echo "Syncing npm package versions to $VERSION"

# Root package
ROOT_PKG="$REPO_ROOT/packaging/npm/package.json"
python3 -c "
import json, sys
p = json.load(open('$ROOT_PKG'))
p['version'] = '$VERSION'
for k in list(p.get('optionalDependencies', {}).keys()):
    p['optionalDependencies'][k] = '$VERSION'
json.dump(p, open('$ROOT_PKG', 'w'), indent=2)
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
json.dump(p, open('$pkg', 'w'), indent=2)
print('  ✓ ' + '$pkg')
"
done

echo "Done — all npm packages at $VERSION"
