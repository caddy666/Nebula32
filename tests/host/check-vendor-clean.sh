#!/usr/bin/env bash
# check-vendor-clean.sh — verify no tracked files under libs/ have been modified.
# libs/ is entirely vendor-supplied; project-specific overrides go in include/.
# Exits non-zero and lists modified files if any are found.
# Usage: ./check-vendor-clean.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

modified=$(git diff --name-only HEAD -- libs/ 2>/dev/null; git diff --name-only -- libs/ 2>/dev/null)
modified=$(echo "$modified" | sort -u | grep -v '^$' || true)

if [[ -z "$modified" ]]; then
    echo "OK — libs/ is clean (no vendor files modified)"
    exit 0
fi

echo "MODIFIED vendor files in libs/ — move overrides to include/ instead:"
echo "$modified" | sed 's/^/  /'
exit 1
