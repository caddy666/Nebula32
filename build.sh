#!/usr/bin/env bash
# =============================================================================
# build.sh — configure + build the Nebula32 firmware out-of-source
# =============================================================================
# Usage:
#   ./build.sh                    # standard Pico 2 (no WiFi)         → build/
#   ./build.sh pico2_w            # Pico 2 W (WiFi + wolfSSL TLS)     → build-pico2w/
#   ./build.sh core2350b          # Waveshare Core2350B0 + RM2 WiFi   → build-core2350b/
#   ./build.sh pico2 -DBUILD_WITH_COMMO=ON     # extra args pass through to cmake
#   ./build.sh pico2_w -DBUILD_WITH_PSRAM=ON
#
# Always uses explicit -S/-B so a stale CMakeCache.txt in the source tree can
# never hijack the configure (cmake treats a path containing CMakeCache.txt as
# an existing build tree — that is how an in-source cache breaks `cmake ..`).
set -euo pipefail
cd "$(dirname "$0")"

BOARD="${1:-pico2}"
[ $# -gt 0 ] && shift

case "$BOARD" in
    pico2)       BUILD_DIR="build" ;;
    pico2_w)     BUILD_DIR="build-pico2w" ;;
    core2350b)   BUILD_DIR="build-core2350b"; BOARD="pico2_w" ;;
    *) echo "usage: $0 [pico2|pico2_w|core2350b] [extra -D cmake args...]" >&2; exit 1 ;;
esac

# Remove leftovers of any accidental in-source configure (git-ignored junk).
if [ -f CMakeCache.txt ] || [ -d CMakeFiles ]; then
    echo "Removing stale in-source CMakeCache.txt / CMakeFiles/"
    rm -rf CMakeCache.txt CMakeFiles
fi

cmake -S . -B "$BUILD_DIR" -DPICO_PLATFORM=rp2350 -DPICO_BOARD="$BOARD" "$@"
cmake --build "$BUILD_DIR" --target Nebula32 -j"$(nproc)"

echo
echo "Done: $BUILD_DIR/Nebula32.uf2  (drag onto the RP2350 BOOTSEL drive)"
