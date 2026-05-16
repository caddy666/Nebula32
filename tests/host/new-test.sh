#!/usr/bin/env bash
# new-test.sh — scaffold a new CppUTest group and wire it into the Makefile.
# Usage: ./new-test.sh <GroupName>
# Example: ./new-test.sh SectorCadence  →  creates test_sectorcadence.cpp
set -euo pipefail
cd "$(dirname "$0")"

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <GroupName>" >&2
    exit 1
fi

group="$1"
lower=$(echo "$group" | tr '[:upper:]' '[:lower:]')
file="test_${lower}.cpp"

if [[ -f "$file" ]]; then
    echo "ERROR: $file already exists" >&2
    exit 1
fi

# Write boilerplate.
cat > "$file" << EOF
#include <CppUTest/TestHarness.h>
extern "C" {
#include "cd_types.h"
}

TEST_GROUP(${group}) {};

TEST(${group}, Placeholder)
{
    // TODO: replace with real test
    CHECK_TRUE(1);
}
EOF

echo "Created $file"

# Add to SRCS_CPP in Makefile.
python3 - Makefile "$file" << 'PYEOF'
import sys

path, newfile = sys.argv[1:]
lines = open(path).read().split('\n')

in_srcs = False
last_cpp = -1

for i, line in enumerate(lines):
    if line.startswith('SRCS_CPP'):
        in_srcs = True
    if in_srcs and ('.cpp' in line):
        last_cpp = i
    if in_srcs and last_cpp >= 0 and not line.strip():
        break

if last_cpp < 0:
    print("ERROR: could not locate SRCS_CPP block", file=sys.stderr)
    sys.exit(1)

lines[last_cpp] = lines[last_cpp].rstrip() + ' \\'
lines.insert(last_cpp + 1, '    ' + newfile)
open(path, 'w').write('\n'.join(lines))
PYEOF

echo "Added $file to Makefile SRCS_CPP"

# Verify and compile.
bash check-srcs.sh
make -j"$(nproc)" "${file%.cpp}.o"
echo "Compiles OK — edit $file and run: make run"
