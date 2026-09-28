#!/usr/bin/env bash
# Package the installed SDK + CLI and verify the relocated archive.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build}"
ARCHIVE="${2:-${ROOT}/dist/sparx-sdk.tar.gz}"
mkdir -p "$(dirname "${ARCHIVE}")"
ARCHIVE="$(cd "$(dirname "${ARCHIVE}")" && pwd)/$(basename "${ARCHIVE}")"
STAGING="$(mktemp -d "${TMPDIR:-/tmp}/sparx-package.XXXXXX")"
trap 'rm -rf "${STAGING}"' EXIT
cmake --install "${BUILD_DIR}" --prefix "${STAGING}/sdk" --config Release
test -x "${STAGING}/sdk/bin/sparx"
cp "${ROOT}/LICENSE" "${ROOT}/README.md" "${STAGING}/sdk/"
cp -R "${ROOT}/examples/reference_agent" "${STAGING}/sdk/example"
python3 "${ROOT}/scripts/build_manifest.py" "${ROOT}" "${BUILD_DIR}" "${STAGING}/sdk/BUILD_INFO.json"
tar -czf "${STAGING}/candidate.tar.gz" -C "${STAGING}/sdk" .
mkdir "${STAGING}/unpacked"
tar -xzf "${STAGING}/candidate.tar.gz" -C "${STAGING}/unpacked"
"${STAGING}/unpacked/bin/sparx" version
"${STAGING}/unpacked/bin/sparx" demo automotive
cmake -S "${STAGING}/unpacked/example" -B "${STAGING}/consumer" \
    -DCMAKE_PREFIX_PATH="${STAGING}/unpacked"
cmake --build "${STAGING}/consumer" --config Release
ctest --test-dir "${STAGING}/consumer" -C Release --output-on-failure
# Publish the file only after all verification has succeeded.
cp "${STAGING}/candidate.tar.gz" "${ARCHIVE}"
python3 - "${ARCHIVE}" <<'PY_CHECKSUM'
import hashlib
from pathlib import Path
import sys
p = Path(sys.argv[1])
p.with_name(p.name + '.sha256').write_text(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n')
PY_CHECKSUM
printf 'Verified archive: %s\n' "${ARCHIVE}"
