#!/usr/bin/env bash
# check-cmake-srcs.sh — verify every src/*.c and src/*.cpp appears in CMakeLists.txt.
# Catches source files added to src/ that were never wired into the firmware build.
# Usage: ./check-cmake-srcs.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

errors=0
for f in src/*.c src/*.cpp; do
    [[ -f "$f" ]] || continue
    if ! grep -qF "$f" CMakeLists.txt; then
        echo "UNWIRED: $f not found in CMakeLists.txt"
        errors=$((errors + 1))
    fi
done

if [[ $errors -eq 0 ]]; then
    echo "OK — all src/*.c src/*.cpp files are referenced in CMakeLists.txt"
fi
exit "$errors"
