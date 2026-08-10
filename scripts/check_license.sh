#!/usr/bin/env bash
#
# Blocks a release if anything Qualcomm-licensed has migrated into the tree.
#
# Context: the Qualcomm AI Stack License permits use of the SDK but forbids
# standalone redistribution of its files. In the source tree this material was
# confirmed present under genai_lib/, qnn_model_prepare_*.py and utilities/,
# where 71 of 99 Python files carry a "Confidential and Proprietary" header.
# None of it belongs in a public repo or a release artifact.
#
# sparx itself is clean by construction: it dlopen()s libGenie.so at runtime and
# neither links nor bundles any Qualcomm code. This script exists so that stays
# true — a single careless `cp` is all it takes to turn a build into a licence
# violation, and that mistake is invisible in a diff review.
#
# Exit 0 = clean, exit 1 = do not publish.
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

FAIL=0

red()   { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }

echo "==> licence scan: $REPO_ROOT"
echo

# --- 1. proprietary copyright headers --------------------------------------
# The substantive check. Matches the header text itself, so it catches a
# proprietary file no matter what it was renamed to or which directory it
# landed in.
#
# Excludes: this script (which necessarily contains the pattern), the docs that
# describe the constraint, and build output. Everything else is in scope.
echo "[1/4] scanning for proprietary copyright headers"
HEADER_HITS=$(grep -rl -I \
    -e "Confidential and Proprietary" \
    -e "Qualcomm Technologies, Inc. and/or its subsidiaries" \
    . \
    --exclude-dir=.git \
    --exclude-dir=build \
    --exclude-dir=dist \
    --exclude-dir='build-release-*' \
    --exclude-dir=node_modules \
    --exclude=check_license.sh \
    --exclude=THIRD_PARTY_NOTICES.md \
    2>/dev/null | grep -v '^\./docs/' | grep -v '^\./scripts/tests/' || true)

if [ -n "$HEADER_HITS" ]; then
    red "  FAIL — files carrying a proprietary header:"
    printf '    %s\n' $HEADER_HITS
    FAIL=1
else
    green "  ok — no proprietary headers found"
fi

# --- 2. known-restricted paths ---------------------------------------------
# Belt and braces: these directories are restricted wholesale, including any
# file in them that happens to lack a header.
echo "[2/4] checking for known-restricted paths"
RESTRICTED_FOUND=""
for path in genai_lib utilities; do
    if [ -e "$path" ]; then
        RESTRICTED_FOUND="$RESTRICTED_FOUND $path"
    fi
done
# qnn_model_prepare_*.py may appear anywhere.
PREPARE_HITS=$(find . -name 'qnn_model_prepare_*.py' \
    -not -path './.git/*' -not -path './build/*' 2>/dev/null || true)
if [ -n "$PREPARE_HITS" ]; then
    RESTRICTED_FOUND="$RESTRICTED_FOUND $PREPARE_HITS"
fi

if [ -n "$RESTRICTED_FOUND" ]; then
    red "  FAIL — restricted paths present:"
    printf '    %s\n' $RESTRICTED_FOUND
    FAIL=1
else
    green "  ok — no restricted paths"
fi

# --- 3. no Qualcomm binaries bundled ---------------------------------------
# The SDK's shared objects must be obtained by the developer from Qualcomm, not
# shipped by us. sparx resolves them with dlopen at runtime.
echo "[3/4] checking for bundled Qualcomm binaries"
BLOB_HITS=$(find . \
    \( -name 'libGenie*' -o -name 'libQnn*' -o -name 'libSnpe*' \
       -o -name 'libGenieX*' -o -name '*.serialized.bin' \) \
    -not -path './.git/*' -not -path './build/*' 2>/dev/null || true)
if [ -n "$BLOB_HITS" ]; then
    red "  FAIL — Qualcomm binaries must not be redistributed:"
    printf '    %s\n' $BLOB_HITS
    FAIL=1
else
    green "  ok — no bundled Qualcomm binaries"
fi

# --- 4. release artifacts ---------------------------------------------------
# Inspects the actual tarballs when present, since that is what users receive.
# A clean source tree with a dirty artifact would still be a violation.
echo "[4/4] inspecting release artifacts in dist/"
if compgen -G "dist/*.tar.gz" >/dev/null 2>&1; then
    ART_FAIL=0
    for archive in dist/*.tar.gz; do
        listing=$(tar -tzf "$archive" 2>/dev/null || true)
        bad=$(printf '%s\n' "$listing" | grep -E \
            'libGenie|libQnn|libSnpe|genai_lib|qnn_model_prepare|utilities/' || true)
        if [ -n "$bad" ]; then
            red "  FAIL — $archive contains:"
            printf '    %s\n' $bad
            ART_FAIL=1; FAIL=1
        fi
    done
    [ "$ART_FAIL" -eq 0 ] && green "  ok — artifacts clean"
else
    echo "  (no artifacts built yet — skipped)"
fi

echo
if [ "$FAIL" -ne 0 ]; then
    red "LICENCE SCAN FAILED — do not publish this release."
    echo
    echo "  The Qualcomm AI Stack License permits use of the SDK but forbids"
    echo "  standalone redistribution of its files. Remove the files listed"
    echo "  above from the tree before cutting a release."
    exit 1
fi
green "LICENCE SCAN PASSED"
exit 0
