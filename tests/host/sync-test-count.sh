#!/usr/bin/env bash
# sync-test-count.sh — rebuild test binaries and patch the test counts in CLAUDE.md.
# Updates the three CppUTest binaries (cd32_tests, parser_tests, vdisc_tests) whose
# output contains a parseable "OK (N tests, ...)" line.  The stress_sector_cache count
# is a manually curated number (raw pthread binary, no CppUTest output) and is
# left unchanged.
# Usage: ./sync-test-count.sh
set -euo pipefail
cd "$(dirname "$0")"

CLAUDE_MD="../../CLAUDE.md"

echo "Building cd32_tests, parser_tests, vdisc_tests..."
make -j"$(nproc)" cd32_tests parser_tests vdisc_tests > /dev/null

extract_count() {
    local bin="$1"
    "./$bin" -v 2>&1 | grep -oE 'OK \([0-9]+ tests' | grep -oE '[0-9]+'
}

main_count=$(extract_count cd32_tests)
parser_count=$(extract_count parser_tests)
vdisc_count=$(extract_count vdisc_tests)

echo "cd32_tests:   $main_count tests"
echo "parser_tests: $parser_count tests"
echo "vdisc_tests:  $vdisc_count tests"

python3 - "$CLAUDE_MD" "$main_count" "$parser_count" "$vdisc_count" << 'EOF'
import sys, re

path, main_n, parser_n, vdisc_n = sys.argv[1:]
text = open(path).read()

text = re.sub(
    r'(\*\*Result:\*\* )\d+( tests, 0 failures)',
    lambda m: m.group(1) + main_n + m.group(2), text)
text = re.sub(
    r'(\*\*Parser tests:\*\*[^→]+→ )\d+( tests, 0 failures)',
    lambda m: m.group(1) + parser_n + m.group(2), text)
text = re.sub(
    r'(\*\*Virtual disc tests:\*\*[^→]+→ )\d+( tests, 0 failures)',
    lambda m: m.group(1) + vdisc_n + m.group(2), text)

open(path, 'w').write(text)
EOF

echo "CLAUDE.md updated."
