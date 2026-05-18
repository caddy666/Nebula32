#!/usr/bin/env bash
# ci.sh — full test gate: all four binaries + warn + cppcheck
# Usage: ./ci.sh
# Run from tests/host/ or anywhere (script cds to its own directory).
set -euo pipefail
cd "$(dirname "$0")"

PASS=0
FAIL=0

run() {
    local label="$1"; shift
    echo ""
    echo "=== $label ==="
    if "$@"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAILED: $label"
    fi
}

run "vendor libs clean"                     bash check-vendor-clean.sh
run "build + warn + cppcheck + cd32_tests"  make -j"$(nproc)" check
run "parser_tests"                          bash -c 'make -j"$(nproc)" parser_tests && ./parser_tests -v'
run "vdisc_tests"                           bash -c 'make -j"$(nproc)" vdisc_tests && ./vdisc_tests -v'
run "stress_sector_cache"                   bash -c 'make -j"$(nproc)" stress_sector_cache && ./stress_sector_cache'

echo ""
echo "=== results: $PASS passed, $FAIL failed ==="
exit "$FAIL"
