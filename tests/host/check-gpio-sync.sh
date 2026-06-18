#!/usr/bin/env bash
# check-gpio-sync.sh — verify the 26-pin connector assignments in CLAUDE.md match
# the authoritative #define PIN_* entries in include/gpio_map.h.
#
# gpio_map.h format:  #define PIN_NAME  <GPIO>  /**< conn <N>  — ... */
# CLAUDE.md format:   | <GPIO> | Signal | Dir | <N> | Notes |   (GPIO is row key)
#
# Usage: ./check-gpio-sync.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

GPIO_MAP="include/gpio_map.h"
CLAUDE_MD="CLAUDE.md"

errors=0

# Extract GPIO→conn pairs from gpio_map.h.
# Line: #define PIN_FOO   5   /**< conn 17 — ...
while IFS= read -r line; do
    gpio=$(echo "$line" | awk '{print $3}')
    conn=$(echo "$line" | grep -oE 'conn [0-9]+' | grep -oE '[0-9]+')
    [[ -z "$conn" ]] && continue

    # Find the row for this GPIO in CLAUDE.md: | <GPIO>  | Signal | Dir | Conn | Notes |
    # Column 4 (after the 4th pipe) is the connector pin number.
    md_conn=$(grep -E "^\| ${gpio}\s*\|" "$CLAUDE_MD" | awk -F'|' '{gsub(/ /,"",$5); print $5}' | head -1)
    if [[ -z "$md_conn" ]]; then
        echo "MISSING  GPIO $gpio (conn $conn) not found in CLAUDE.md table"
        errors=$((errors + 1))
        continue
    fi
    if [[ "$md_conn" != "$conn" ]]; then
        echo "MISMATCH GPIO $gpio: gpio_map.h says conn $conn, CLAUDE.md says conn $md_conn"
        errors=$((errors + 1))
    fi
done < <(grep -E '^#define PIN_[A-Z0-9_]+\s+[0-9]+\s+/\*\*< conn [0-9]+' "$GPIO_MAP")

if [[ $errors -eq 0 ]]; then
    echo "OK — all GPIO→connector assignments match between gpio_map.h and CLAUDE.md"
fi
exit "$errors"
