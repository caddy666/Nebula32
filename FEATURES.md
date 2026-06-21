# Nebula32 — Feature Catalogue

Nebula32 is an **optical-drive emulator (ODE) for the Commodore Amiga CD32**. It
replaces the original CD mechanism with a Raspberry Pi Pico 2 / RP2350-class MCU
that serves disc images from an SD card to the CD32's Akiko chip over the console's
native CD-drive connector.

This document is the single, current catalogue of **what the firmware does**. For
*how it's built* see [`CLAUDE.md`](CLAUDE.md) (architecture reference) and the
topic docs linked throughout.

- **Targets:** Pico 2 (`pico2`), Pico 2 W (`pico2_w`), Waveshare **Core2350B0 + RM2 WiFi** (`core2350b`)
- **Build:** `./build.sh <target> [--debug]` → `<build-dir>/Nebula32.uf2`
- **Firmware size:** ~1.8 MB UF2 (≈350 KB text)

> Developer-facing material (the host test suite, etc.) lives in [`TESTS.md`](TESTS.md), not here.

---

## 1. CD32 drive emulation (the core)

The ODE impersonates the original Chinon drive + CXD2545Q DSP from Akiko's point
of view. 

| Feature | Detail | Source |
|---------|--------|--------|
| **COMMO command bus** | 3-wire (IF_CLK/IF_DATA/IF_DIR) command + status protocol; full opcode set, additive checksums, Chinon power-on handshake | `src/commo.c`, `src/commo_bridge.c`, `pio/commo.pio` |
| **DA audio/data stream** | 24-bit I2S to Akiko — **48-bit frames** (24L+24R), 2.12 MHz BCLK, 44.1 kHz LRCLK, driven by PIO + DMA | `src/da_output.c`, `pio/da_output.pio` |
| **1× / 2× drive speed** | Runtime speed switch; PIO clkdiv + subcode clkdiv updated atomically | `src/da_output.c` |
| **M17SINE phase-lock** | DA clkdiv trimmed every 2 s against the 16.9344 MHz console reference | `src/main.c` |
| **Subcode / Q-channel** | P–W subcode + Q-channel (CRC-16, BCD track/index, absolute & relative MSF) generated per sector via PIO | `src/subcode.c`, `pio/subcode_encoder.pio` |
| **EDC / ECC** | ECMA-130 P/Q parity + EDC for Mode-1 sectors | `src/ecc.c` |
| **Disc-change events** | Runtime eject/insert makes Akiko clear the DISC bit and re-read the TOC (reuses door/tray transitions) | `src/commo_bridge.c` |
| **/RESET handling** | Honours the CD32 `/RESET` line; clears drive state, preserves door state | `src/commo_bridge.c` |

**Provenance:** DA timing is not theoretical — it's confirmed against a 100M-row
logic-analyser capture (`digital.csv`). See [`cd32_quirks.md`](cd32_quirks.md) for
the 48-bit-frame finding and other deliberate spec deviations.

---

## 2. Disc image support

| Format | Notes | Source |
|--------|-------|--------|
| **ISO** | 2048-byte Mode-1 | `src/disc_image.c` |
| **BIN/CUE** | Multi-track, PREGAP + INDEX handling, raw 2352-byte sectors | `src/disc_image.c` |
| **NRG** (Nero) | v1 + v2, lead-in/lead-out skip, >4 GB offsets (uint64) | `src/disc_image.c` |
| **MDF** (Alcohol) | Session/track parsing | `src/disc_image.c` |
| **Multi-track / mixed-mode** | Audio + data tracks, pregap LBAs, TOC synthesis | `src/disc_image.c` |

Robustness: malformed-image guards for zero chunk sizes, oversized/under-length
records, and unaligned reads.

---

## 3. Virtual disc / virtual directory

Instead of a pre-built image, the firmware can **synthesise an ISO 9660 CD-ROM
on-the-fly** from the files on a dedicated SD partition — drop files on a FAT
partition and they appear to the CD32 as a Mode-1 data disc. Full design in
[`virtual_directory.md`](virtual_directory.md).

| Capability | Detail | Source |
|------------|--------|--------|
| On-the-fly ISO 9660 | PVD/VDST, L+M path tables, directory records, per-sector dispatch | `src/virtual_disc.c` |
| Directory traversal | BFS scan → LBA assignment, nested subdirs, name sanitising | `src/virtual_disc.c` |
| Two-partition SD | Partition 1 = images, Partition 2 = virtual-dir source (`FF_MULTI_PARTITION`) | `src/hw_config.c`, `include/ffconf.h` |

---

## 4. Storage & performance

| Feature | Detail | Source |
|---------|--------|--------|
| **SD via 4-bit SDIO** | High-bandwidth SDIO (not SPI); FatFS mount + scan | `src/sd_card.c`, `src/hw_config.c` |
| **Dual-core sector prefetch** | Core 1 prefetches sectors into an 8-slot ring while Core 0 serves the bus; race-safe via `flush_gen` + atomics | `src/sector_cache.c` |
| **Image library** | Scans `.iso/.bin/.nrg/.mdf`; alphabetical sort for stable indices; pagination (64/page) | `src/sd_card.c`, `src/main.c` |
| **PSRAM (8 MB QSPI)** | Optional `BUILD_WITH_PSRAM`; QMI CS1 on GPIO 47 | `src/psram.c` |
| **Real-time safety** | DA DMA ISR call-graph pinned to SRAM (`sram_attr.h`); silence-pad buffer on end-of-disc | `src/da_output.c`, `include/sram_attr.h` |

---

## 5. Multi-disc carousel & jukebox

Host-free disc switching — change discs without a real CD in the machine.

| Feature | Detail | Source |
|---------|--------|--------|
| **Carousel model** | Ordered set of selectable discs (ALL or PLAYLIST source) with wrap navigation; pure-logic + host-tested | `src/carousel.c` |
| **`.m3u` playlists** | Curated disc subsets from `0:/playlists/*.m3u`; basename match, case-insensitive | `src/carousel.c`, `src/main.c` |
| **Jukebox auto-advance** | At the end of an all-audio disc, auto-advances + auto-plays the next carousel disc; gated so data games never trigger it | `src/main.c`, `src/da_output.c` |
| **Core-1 quiesce on swap** | `disc_swap_locked()` parks Core 1 via `multicore_lockout` so an in-flight SD read is never torn by `f_close` | `src/main.c` |
| **Persistence** | Active playlist + jukebox toggle saved to flash; restored on boot | `src/config.c`, `include/config.h` |

**Controls:** console `n`/`p` (next/prev), `m`/`a` (playlist/all), `j` (jukebox),
`P` (step playlist menu); web `/api/carousel/*` and `/api/playlist/*`; and the
rotary **hold-knob + turn** playlist gesture (see §7). Hardware bring-up runbook:
[`CAROUSEL_PHASE5_BRINGUP.md`](CAROUSEL_PHASE5_BRINGUP.md).

---

## 6. Connectivity — WiFi web interface

| Feature | Detail | Source |
|---------|--------|--------|
| **WiFi** | RM2 / CYW43439 radio (GPIO 23/24/25/29 on Core2350B0) | CMake `NEBULA_WIFI` block |
| **HTTPS / TLS** | wolfSSL 5.9.1 over lwIP-native; RP2350 TRNG port | `src/webserver.c`, `include/user_settings.h` |
| **Disc selector** | Grid UI with cover art, pagination, click-to-load, eject | `src/webserver.c` |
| **Carousel / playlist panel** | Prev/next disc, playlist picker populated from `/api/playlist/list`, apply | `src/webserver.c` |
| **Firmware flash** | Upload + flash UF2 from the browser, token-guarded | `src/webserver.c`, `src/fw_update.c` |
| **Security** | Path-traversal guards, HTML/JSON escaping, constant-time token compare, static conn pool | `src/webserver.c` |

API endpoints: `/api/status`, `/api/images`, `/api/load/{i}`, `/api/page/*`,
`/api/eject`, `/api/carousel/next|prev`, `/api/playlist/list|set/{name}|all`,
`/api/fw/list`, `/api/fw/flash/{file}`.

---

## 7. Local UI — display, rotary, console

| Feature | Detail | Source |
|---------|--------|--------|
| **ST7789 240×240 display** | JPEG cover art (JPEGDEC), async SPI-DMA scanline pushes | `src/display.cpp` |
| **Audio visualiser** | 256-pt Q15 FFT + 6 demoscene effects (Spectrum/Scope/Raster/Combo/Spaceballs/Juggler), 30 fps cap | `src/effects.c`, `src/fft.c`, `src/vis_audio.c` |
| **Boing Ball** | Spherical-UV-mapped firmware-update success animation | `src/display.cpp` |
| **Rotary encoder** | Direct-GPIO quadrature (12/15/18/19), edge-IRQ decode, acceleration, push-button | `src/rotary_gpio.c`, `src/ui.c` |
| **Rotary gestures** | Turn = scroll discs · press = load · long-press = eject · **hold + turn = cycle playlists** · logger button | `src/ui.c`, `src/main.c` |
| **USB-CDC + UART console** | `H` help, `#` self-test, status/TOC, carousel keys; stdio mirrored to USB-CDC + UART0 (GPIO 16/17) | `src/main.c`, `src/selftest.c` |

---

## 8. Firmware & system

| Feature | Detail | Source |
|---------|--------|--------|
| **SD-card UF2 self-update** | Dual-bank, fault-tolerant flash: validate → write Bank 1 → verify via XIP → copy to Bank 0 → reboot. Power-loss safe outside the final copy. | `src/fw_update.c`, [`fw-upd.md`](fw-upd.md) |
| **Flash-backed config** | `ode_config_t` in the last 4 KB flash page (last image, speed, jukebox, active playlist); CRC-validated, version-guarded | `src/config.c`, `include/config.h` |
| **SD text config** | Hand-editable `nebula32.cfg` (sdcard dir, WiFi, logging, `playlist_save_ms` debounce, fw token) | `src/logger.c` |
| **SD logging** | Ring-buffered activity log flushed to SD; per-category toggles | `src/logger.c` |
| **Boot self-test** | `#` at boot — GPIO/DA/SUB pin checks, /RESET idle, SD mount + sector read; PASS/FAIL over console | `src/selftest.c` |
| **Watchdog** | 10 s watchdog, `pause_on_debug` so it won't fire under a debugger | `src/main.c` |

---

## 9. Hardware targets & build

| Target | Board | Output | Radio |
|--------|-------|--------|-------|
| `pico2` | Raspberry Pi Pico 2 | `build/` | — |
| `pico2_w` | Pico 2 W | `build-pico2w/` | CYW43 |
| `core2350b` | **Waveshare Core2350B0 (RP2350B) + RM2** | `build-core2350b/` | RM2 / CYW43439 |

```bash
./build.sh core2350b            # release (-O2) → build-core2350b/Nebula32.uf2
./build.sh core2350b --debug    # -Og -g3 for SWD/gdb → build-core2350b-debug/
```

The Core2350B0 build remaps all pins for the RP2350B's 48 GPIOs + RM2 WiFi and
needs **no MCP23017** (the rotary encoder is wired direct to GPIO). Full pin grid
in [`CLAUDE.md`](CLAUDE.md); on-hardware debugging notes in [`openocd.md`](openocd.md).

---

## 10. What the ODE deliberately can't do

The CD connector is a **read-mostly** position: a wide one-way DA/sector stream
(drive→Akiko) plus a skinny bidirectional COMMO command bus. That fixes what is
cheap (serving content), what is precious (small Amiga→ODE writes), and what is
off-bus entirely (floppy/IDE/SCSI). Use [`CAPABILITY_BOUNDARY.md`](CAPABILITY_BOUNDARY.md)
to answer "can the ODE do X?" in ten seconds.

---

## Documentation map

| Doc | Purpose |
|-----|---------|
| **FEATURES.md** (this file) | What the firmware does — the feature catalogue |
| [`README.md`](README.md) | Project overview + quick start |
| [`CLAUDE.md`](CLAUDE.md) | Architecture reference — GPIO map, PIO/DMA allocation, per-file status, timing |
| [`cd32_quirks.md`](cd32_quirks.md) | Intentional spec deviations & hardware oddities |
| [`CAPABILITY_BOUNDARY.md`](CAPABILITY_BOUNDARY.md) | CD-bus capability rules ("can it do X?") |
| [`virtual_directory.md`](virtual_directory.md) | Virtual-disc / ISO-9660-synthesis design |
| [`fw-upd.md`](fw-upd.md) | Firmware self-update design |
| [`CAROUSEL_PHASE5_BRINGUP.md`](CAROUSEL_PHASE5_BRINGUP.md) | Carousel/jukebox hardware bring-up runbook |
| [`openocd.md`](openocd.md) | SWD/OpenOCD + UART on-hardware debugging |
| [`TESTS.md`](TESTS.md) | Host test-suite catalogue (developer-facing) |
