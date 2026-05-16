#!/usr/bin/env bash
# check-test-docs.sh — verify every TEST_GROUP in cd32_tests has a row in CLAUDE.md.
# Catches groups that were added to source but never documented.
# Exits non-zero and lists offending groups if any are found.
# Usage: ./check-test-docs.sh
set -euo pipefail
cd "$(dirname "$0")"

# Groups declared in source files wired into cd32_tests (SRCS_CPP).
# Parsing source is more reliable than parsing binary output.
srcs_groups=$(grep -h 'TEST_GROUP(' test_*.cpp \
    | grep -oE 'TEST_GROUP\([A-Za-z0-9]+' | grep -oE '[A-Za-z0-9]+$' | sort -u)

doc_groups=$(grep -oE '^\| `[A-Za-z0-9]+`' ../../CLAUDE.md \
    | grep -oE '[A-Za-z0-9]+' | sort -u)

errors=0
while IFS= read -r group; do
    if ! echo "$doc_groups" | grep -qx "$group"; then
        echo "UNDOCUMENTED group: $group"
        errors=$((errors + 1))
    fi
done <<< "$srcs_groups"

if [[ $errors -eq 0 ]]; then
    echo "OK — all test groups documented in CLAUDE.md"
fi
exit "$errors"
