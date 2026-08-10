#!/usr/bin/env bash
# Mutation test for check_license.sh: plant a violation of each kind, confirm
# the gate rejects it, then remove it and confirm the gate goes green again.
# A gate that has never failed is not known to work.
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

PASS=0; FAIL=0
check() { # check <label> <expected exit: 0|1>
    ./scripts/check_license.sh >/tmp/lic_out.txt 2>&1
    got=$?
    if [ "$got" -eq "$2" ]; then
        echo "  PASS  $1 (exit $got)"; PASS=$((PASS+1))
    else
        echo "  FAIL  $1 (expected exit $2, got $got)"; FAIL=$((FAIL+1))
        sed 's/^/        /' /tmp/lic_out.txt | tail -8
    fi
}

echo "=== baseline ==="
check "clean tree passes" 0

echo "=== mutant 1: proprietary header, disguised filename ==="
mkdir -p src/vendor
cat > src/vendor/helper_utils.cpp <<'EOF'
//=============================================================================
//
//  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
//  All rights reserved.
//  Confidential and Proprietary - Qualcomm Technologies, Inc.
//
//=============================================================================
void helper() {}
EOF
check "header in renamed file is caught" 1
rm -rf src/vendor
check "removed -> clean" 0

echo "=== mutant 2: restricted directory ==="
mkdir -p genai_lib && echo "x = 1" > genai_lib/config.py
check "genai_lib/ is caught" 1
rm -rf genai_lib
check "removed -> clean" 0

echo "=== mutant 2b: qnn_model_prepare_*.py anywhere ==="
mkdir -p tools/nested && echo "x = 1" > tools/nested/qnn_model_prepare_qwen3.py
check "qnn_model_prepare_*.py nested is caught" 1
rm -rf tools/nested
check "removed -> clean" 0

echo "=== mutant 3: bundled Qualcomm binary ==="
mkdir -p cli/lib && echo "ELF" > cli/lib/libGenie.so
check "libGenie.so is caught" 1
rm -rf cli/lib
check "removed -> clean" 0

echo "=== mutant 4: dirty release artifact, clean source tree ==="
mkdir -p /tmp/dirty_art/sparx-9.9.9-test/lib
echo "ELF" > /tmp/dirty_art/sparx-9.9.9-test/lib/libQnnHtp.so
tar -czf dist/sparx-9.9.9-test.tar.gz -C /tmp/dirty_art sparx-9.9.9-test
check "proprietary blob inside tarball is caught" 1
rm -f dist/sparx-9.9.9-test.tar.gz; rm -rf /tmp/dirty_art
check "removed -> clean" 0

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
