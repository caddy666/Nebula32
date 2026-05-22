#!/usr/bin/env bash
# check-pio-files.sh — verify every .pio file on disk is mentioned in CLAUDE.md
# and every .pio file mentioned in CLAUDE.md actually exists.
# Usage: ./check-pio-files.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

CLAUDE_MD="CLAUDE.md"

errors=0

# 1. Every .pio file on disk must appear in CLAUDE.md.
while IFS= read -r pio; do
    rel="${pio#./}"
    if ! grep -qF "$rel" "$CLAUDE_MD"; then
        echo "UNDOCUMENTED .pio file: $rel"
        errors=$((errors + 1))
    fi
done < <(find pio upstream/pio -name "*.pio" 2>/dev/null | sort)

# 2. Every backtick-quoted .pio reference in CLAUDE.md must resolve to a file on disk.
# Both full paths (`pio/da_output.pio`) and bare names (`da_output.pio`) are checked.
# A bare name resolves if any file with that basename exists anywhere under the tree.
while IFS= read -r ref; do
    if [[ "$ref" == */* ]]; then
        # Full path — must exist exactly.
        if [[ ! -f "$ref" ]]; then
            echo "MISSING .pio file referenced in CLAUDE.md: $ref"
            errors=$((errors + 1))
        fi
    else
        # Bare name — accept if found anywhere under pio/ or upstream/pio/.
        if ! find pio upstream/pio -name "$ref" 2>/dev/null | grep -q .; then
            echo "MISSING .pio file referenced in CLAUDE.md (not found): $ref"
            errors=$((errors + 1))
        fi
    fi
done < <(python3 -c "
import re, sys
text = open(sys.argv[1]).read()
for m in re.findall(r'\x60([^\x60]+\.pio)\x60', text):
    print(m)
" "$CLAUDE_MD" | sort -u)

if [[ $errors -eq 0 ]]; then
    echo "OK — all .pio files present and documented in CLAUDE.md"
fi
exit "$errors"
