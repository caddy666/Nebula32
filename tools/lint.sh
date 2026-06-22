#!/usr/bin/env bash
# Run clang-tidy over our firmware src/ using the core2350b compile DB.
# Derives the arm-none-eabi system include dirs from the compiler so clang
# can find newlib headers (string.h, sys/cdefs.h, ...) — without these,
# several TUs fail to parse. Pass extra clang-tidy args through ("$@").
set -euo pipefail
cd "$(dirname "$0")/.."

DB=build-core2350b
[ -f "$DB/compile_commands.json" ] || { echo "no $DB/compile_commands.json — run ./build.sh core2350b first"; exit 1; }

# arm system include dirs, straight from the compiler's own search list.
ARGS=()
while read -r d; do ARGS+=("--extra-arg=-isystem$d"); done < <(
  arm-none-eabi-gcc -E -Wp,-v -xc /dev/null 2>&1 \
    | grep '/arm-none-eabi/' | grep -v '^ *#' | sed 's/^ *//'
)

clang-tidy -p "$DB" "${ARGS[@]}" "$@" src/*.c
