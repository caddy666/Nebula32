#!/usr/bin/env bash
# check-codebase-status.sh — verify every tracked source file has a row in the
# CLAUDE.md "Codebase status" table.
# Checks: src/*.c, src/*.cpp, upstream/core/*.c, upstream/utils/*.c
# Exits non-zero and lists offenders if any are missing.
# Usage: ./check-codebase-status.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

CLAUDE_MD="CLAUDE.md"

# Extract filenames already documented in CLAUDE.md status table.
# Rows look like: | `src/foo.c` | ✅ ...
doc_files=$(python3 -c "
import re, sys
text = open(sys.argv[1]).read()
for m in re.findall(r'\x60([^\x60]+\.(c|cpp))\x60', text):
    print(m[0])
" "$CLAUDE_MD" | sort -u)

errors=0
for f in src/*.c src/*.cpp upstream/core/*.c upstream/utils/*.c; do
    [[ -f "$f" ]] || continue
    rel="${f#./}"
    if ! echo "$doc_files" | grep -qF "$rel"; then
        echo "UNDOCUMENTED: $rel"
        errors=$((errors + 1))
    fi
done

if [[ $errors -eq 0 ]]; then
    echo "OK — all source files documented in CLAUDE.md codebase status table"
fi
exit "$errors"
