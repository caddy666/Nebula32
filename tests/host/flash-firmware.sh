#!/usr/bin/env bash
# flash-firmware.sh — copy build/Nebula32.uf2 to a Pico 2 in BOOTSEL mode.
# Usage: ./flash-firmware.sh [mount-point]
#
# Default mount point is /media/$USER/RP2350.
# To flash: hold BOOTSEL on the Pico 2 while plugging USB, then run this script.
set -euo pipefail
cd "$(dirname "$0")/../.."

UF2="build/Nebula32.uf2"
# TODO: adjust the default mount point for your system if it differs
MOUNT="${1:-/media/${USER}/RP2350}"

if [[ ! -f "$UF2" ]]; then
    echo "ERROR: $UF2 not found — run 'cmake --build build' first" >&2
    exit 1
fi

if [[ ! -d "$MOUNT" ]]; then
    echo "ERROR: Pico 2 not found at $MOUNT" >&2
    echo "  Hold BOOTSEL and replug USB, then retry." >&2
    echo "  Or pass the mount point as: $0 /path/to/mount" >&2
    exit 1
fi

echo "Flashing $UF2 → $MOUNT ..."
cp "$UF2" "$MOUNT/"
echo "Done. Pico 2 will reboot automatically."
