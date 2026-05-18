#!/usr/bin/env bash
# check-srcs.sh — verify every test_*.cpp in tests/host/ is wired into SRCS_CPP
# in the Makefile, or is a known separate-binary file.
# Exits non-zero and lists missing files if any are found.
# Usage: ./check-srcs.sh
set -euo pipefail
cd "$(dirname "$0")"

# These are compiled into their own binaries, not cd32_tests.
declare -A SEPARATE
SEPARATE[test_disc_parser.cpp]=1        # parser_tests binary
SEPARATE[test_sector_cache_stress.cpp]=1 # stress_sector_cache binary
SEPARATE[test_vis_audio_stress.cpp]=1   # stress_tests binary
SEPARATE[test_virtual_disc.cpp]=1       # vdisc_tests binary

errors=0
for f in test_*.cpp; do
    [[ -n "${SEPARATE[$f]+x}" ]] && continue
    if ! grep -qF "    $f" Makefile; then
        echo "MISSING from SRCS_CPP: $f"
        errors=$((errors + 1))
    fi
done

if [[ $errors -eq 0 ]]; then
    echo "OK — all test_*.cpp files accounted for"
fi

exit "$errors"
