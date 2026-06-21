# Nebula32 — Commodore CD32 Optical Drive Emulator

Replaces the optical-drive mechanism in a Commodore Amiga **CD32** with an RP2350
microcontroller that serves disc images (ISO, BIN/CUE, NRG, MDF) from an SD card,
emulating the original Chinon drive + CXD2545Q DSP to the console's Akiko chip.

It also adds things the original drive never had: a WiFi web interface (with HTTPS),
an on-screen cover-art display + audio visualiser, a multi-disc carousel/jukebox,
on-the-fly virtual CDs synthesised from loose files, and SD-card firmware updates.

➡️ **Full feature list: [`FEATURES.md`](FEATURES.md)**

---

## Quick start

```bash
# Build for the Waveshare Core2350B0 (RP2350B) + RM2 WiFi
./build.sh core2350b
# → build-core2350b/Nebula32.uf2  — hold BOOTSEL, drag onto the RPI-RP2 drive

# Other targets:
./build.sh pico2                 # Pico 2 (no WiFi)
./build.sh pico2_w               # Pico 2 W (WiFi + TLS)
./build.sh core2350b --debug     # -Og -g3 build for SWD/gdb debugging
```

Put disc images (`.iso/.bin+.cue/.nrg+.mds/.mdf`) on the SD card; the firmware
scans, lists, and serves them. Settings live in `nebula32.cfg` on the card.

---

## How it works

The CD32 drive talks to the host Amiga chipset (Akiko, U5) over two serial buses,
and Nebula32 implements the **drive side** of both:

- **COMMO 3-wire bus** (IF_CLK/IF_DATA/IF_DIR) — command + status channel. The host
  issues opcodes (seek, play, TOC read); the drive answers with status packets.
- **DA serial bus** — an I2S bit stream carrying sector data and CD audio. Akiko and
  the LC78835M DAC read it directly. BCLK ≈ 2.12 MHz (1×) / 4.23 MHz (2×), LRCLK
  44.1 kHz, and each frame is **48 BCLK cycles (24-bit L + 24-bit R)**, not 32.

Implementation highlights:

- **PIO0** clocks the DA I2S stream (SM0) and the subcode signals (SM1) at exact CD timing.
- **PIO1** handles COMMO receive + transmit.
- **DMA ping-pong** feeds sector data to the DA PIO without CPU involvement.
- **Core 1** prefetches SD sectors into an 8-slot ring so the DA stream never stalls.
- **Core 0** polls COMMO, runs drive state, the web server, display, and UI.
- **FatFS + 4-bit SDIO** reads images far faster than real-time playback needs.

See [`CLAUDE.md`](CLAUDE.md) for the full architecture, GPIO map, and timing data,
and [`cd32_quirks.md`](cd32_quirks.md) for the deliberate spec deviations.

---

## Supported disc image formats

| Format | Files | Sector size | Audio |
|--------|-------|-------------|-------|
| ISO 9660 | `.iso` | 2048 | data only |
| Raw binary | `.bin` + `.cue` | 2352 | yes |
| Nero | `.nrg` | 2048 / 2352 | yes |
| Alcohol 120% | `.mdf` + `.mds` | 2048 / 2352 | yes |

For BIN, place the matching `.cue` alongside; for MDF, the matching `.mds`.

---

## Hardware

| Component | Notes |
|-----------|-------|
| **Waveshare Core2350B0** (RP2350B) | Primary target — 48 GPIOs, fits SD + display + WiFi + PSRAM |
| Raspberry Pi Pico 2 / 2 W | Also supported (`pico2` / `pico2_w`); the W variant adds the web interface |
| **RM2 WiFi module** (CYW43439) | For the web interface on Core2350B0 |
| MicroSD card | **4-bit SDIO wiring** required (not SPI-only breakouts) |
| ST7789 240×240 display | Cover art + visualiser (optional but recommended) |
| KY-040 / EC11 rotary encoder | On-device disc selection without a screen |
| Level translators | CD32 drive-connector signals are 5 V |

The Core2350B0 build needs **no MCP23017** — the rotary encoder is wired directly
to GPIO. The canonical pin assignments are in `include/gpio_map.h` and the full
grid is documented in [`CLAUDE.md`](CLAUDE.md).

### CD32 drive-connector signals (Core2350B0)

| GPIO | Signal | Dir | GPIO | Signal | Dir |
|------|--------|-----|------|--------|-----|
| 0 | DA_DATA | OUT | 8 | SUB_SCOR | OUT |
| 1 | DA_BCLK | OUT | 9 | M17SINE | IN |
| 2 | DA_LRCLK | OUT | 10 | ACTIVE | OUT |
| 3 | DA_C2PO | OUT | 11 | DOOR | IN |
| 4 | DA_EMPH | OUT | 13 | PASSIVE | OUT |
| 5 | SUB_DATA | OUT | 14 | /RESET | IN |
| 6 | SUB_CLK | OUT | 44 | IF_CLK | BIDIR |
| 7 | SUB_WFCLK | OUT | 45 | IF_DATA | BIDIR |
|   |          |     | 46 | IF_DIR | OUT |

---

## Documentation

| Doc | Purpose |
|-----|---------|
| [`FEATURES.md`](FEATURES.md) | Complete feature catalogue |
| [`CLAUDE.md`](CLAUDE.md) | Architecture reference (GPIO/PIO/DMA, per-file status, timing) |
| [`cd32_quirks.md`](cd32_quirks.md) | Intentional spec deviations & hardware oddities |
| [`CAPABILITY_BOUNDARY.md`](CAPABILITY_BOUNDARY.md) | "Can the ODE do X?" — CD-bus capability rules |
| [`virtual_directory.md`](virtual_directory.md) | Virtual-disc / ISO-9660 synthesis design |
| [`fw-upd.md`](fw-upd.md) | Firmware self-update design |
| [`CAROUSEL_PHASE5_BRINGUP.md`](CAROUSEL_PHASE5_BRINGUP.md) | Carousel/jukebox hardware bring-up runbook |
| [`openocd.md`](openocd.md) | SWD/OpenOCD + UART on-hardware debugging |
| [`TESTS.md`](TESTS.md) | Host test-suite catalogue |

---

*Grüße an a1k.org — wir dachten wohl alle dasselbe.* 🛰️
