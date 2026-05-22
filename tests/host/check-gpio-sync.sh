#!/usr/bin/env bash
# check-gpio-sync.sh — verify the 26-pin connector table in CLAUDE.md matches
# the authoritative #define PIN_* entries in upstream/include/gpio_map.h.
#
# gpio_map.h format:  #define PIN_NAME   <GPIO>   /**< conn <N>  — ... */
# CLAUDE.md format:   | <N>  | SIGNAL  | <GPIO> | DIR | Notes |
#
# Skips GPIO 12 (no connector pin) and GPIO 13 (PASSIVE/ST7789_CS special case).
# Usage: ./check-gpio-sync.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

GPIO_MAP="upstream/include/gpio_map.h"
CLAUDE_MD="CLAUDE.md"

errors=0

# Extract conn→GPIO pairs from gpio_map.h.
# Line: #define PIN_FOO   5   /**< conn 17 — ...
while IFS= read -r line; do
    gpio=$(echo "$line" | awk '{print $3}')
    conn=$(echo "$line" | grep -oE 'conn [0-9]+' | grep -oE '[0-9]+')
    [[ -z "$conn" ]] && continue

    # Look for this conn pin in CLAUDE.md connector table.
    # Row: | 17  | SUB_DATA   | 5  | ...
    md_gpio=$(grep -E "^\| ${conn}\s*\|" "$CLAUDE_MD" | grep -oE '\| *[0-9]+ *\|' | sed -n '2p' | tr -d '| ')
    if [[ -z "$md_gpio" ]]; then
        echo "MISSING  conn $conn (GPIO $gpio) not found in CLAUDE.md table"
        errors=$((errors + 1))
        continue
    fi
    md_gpio=$(echo "$md_gpio" | tr -d ' ')
    if [[ "$md_gpio" != "$gpio" ]]; then
        echo "MISMATCH conn $conn: gpio_map.h says GPIO $gpio, CLAUDE.md says GPIO $md_gpio"
        errors=$((errors + 1))
    fi
done < <(grep -E '^#define PIN_[A-Z0-9_]+\s+[0-9]+\s+/\*\*< conn [0-9]+' "$GPIO_MAP")

if [[ $errors -eq 0 ]]; then
    echo "OK — all connector pin→GPIO assignments match between gpio_map.h and CLAUDE.md"
fi
exit "$errors"
