# CD32 Optical Drive Emulator — Raspberry Pi Pico 2

Replaces the optical drive mechanism in a Commodore CD32 with a Raspberry Pi Pico 2 (RP2350) reading ISO, BIN/CUE, NRG, and MDF disc images from an SD card.

---

## How It Works

The Commodore CD32 drive communicates with the host Amiga chipset (Akiko, U5) over two serial interfaces:

- **COMMO 3-wire bus** (GPIO 15/16/17) — command and status channel. The host issues opcodes (seek, play, TOC read) and the drive responds with status packets. This uses an upstream port of the original Philips/Commodore 8051 drive MCU firmware running on the RP2350's PIO.
- **DA serial bus** (GPIO 0/1/2) — I2S bit stream carrying sector data and CD audio. Akiko and the LC78835M DAC both read this stream directly. BCLK runs at 2.12 MHz (1×) or 4.23 MHz (2×); LRCLK at 44.1 kHz. Each I2S frame is 48 BCLK cycles (24 bits L + 24 bits R), not 32.

This project implements the drive side of both buses:

- **PIO0 SM0** clocks the DA I2S bit stream at exact CD timing (1× or 2× speed)
- **PIO0 SM1** drives the subcode signals (SUB_DATA/CLK/WFCLK/SCOR)
- **PIO1 SM0/SM1** handle COMMO receive and transmit
- **DMA ping-pong** feeds sector data from SRAM to the DA PIO TX FIFO without CPU involvement
- **Core 1** prefetches sectors from the SD card into an 8-sector ring buffer so the DA DMA never stalls
- **Core 0** polls the COMMO bus, updates drive state, serves the web interface, and handles the UI
- **FatFS + SDIO** accesses disc images on the SD card at up to 25 MB/s (≈70× real-time)

---

## Supported Disc Image Formats

| Format | Extension | Sector size | Audio tracks |
|--------|-----------|-------------|--------------|
| ISO 9660 | `.iso` | 2048 bytes | No |
| Raw binary | `.bin` + `.cue` | 2352 bytes | Yes |
| Nero Burning ROM | `.nrg` | 2048 or 2352 | Yes |
| Alcohol 120% | `.mdf` + `.mds` | 2048 or 2352 | Yes |

For BIN images, place the `.cue` file with the same base name in the same directory. For MDF images, place the `.mds` metadata file alongside the `.mdf`.

---

## Hardware Requirements

| Component | Notes |
|-----------|-------|
| Raspberry Pi Pico 2 W | RP2350 + CYW43439 WiFi — **Pico 2 W required** for web interface |
| Raspberry Pi Pico 2 | Non-W variant works without web interface |
| MicroSD card | SDIO 4-bit wiring required (not SPI-only breakout boards) |
| KY-040 rotary encoder | For hardware disc selection without a screen |
| Logic level translator | Not normally required — CD32 drive connector signals are 3.3 V |

---

## Pin Wiring

### CD32 26-Pin Drive Connector

| Pico 2 GPIO | Signal      | Direction | Notes |
|-------------|-------------|-----------|-------|
| GPIO 0      | DA_DATA     | OUT       | I2S serial data — sectors + audio |
| GPIO 1      | DA_BCLK     | OUT       | I2S bit clock (2.12 MHz at 1×, 4.23 MHz at 2×) |
| GPIO 2      | DA_LRCLK    | OUT       | I2S word-select (44.1 kHz) |
| GPIO 3      | DA_C2PO     | OUT       | C2 error pointer (driven low = no errors) |
| GPIO 4      | DA_EMPH     | OUT       | Pre-emphasis flag (driven low = no emphasis) |
| GPIO 5      | SUB_DATA    | OUT       | Subcode serial data |
| GPIO 6      | SUB_CLK     | OUT       | Subcode clock |
| GPIO 7      | SUB_WFCLK   | OUT       | Subcode word-frame clock |
| GPIO 8      | SUB_SCOR    | OUT       | Subcode sync correlator |
| GPIO 9      | M17SINE     | IN        | 16.9344 MHz master clock reference (GPIN0) |
| GPIO 10     | ACTIVE      | OUT       | Drive active/spinning status |
| GPIO 11     | DOOR        | IN        | Door/tray switch |
| GPIO 13     | PASSIVE     | OUT       | Drive passive/standby status |
| GPIO 14     | /RESET      | IN        | Active-low reset from CD32 |
| GPIO 15     | IF_CLK      | BIDIR     | COMMO clock |
| GPIO 16     | IF_DATA     | BIDIR     | COMMO data |
| GPIO 17     | IF_DIR      | OUT       | COMMO direction control |



> **DA_EMPH (GPIO 4):** Driven permanently LOW. Pre-emphasis was a 1982–1986 Red Book audio feature used on a small number of early audiophile CDs; no CD32 game title uses it. The LC78835M DAC on the CD32 mainboard ties its de-emphasis select pin to ground, so the signal has no effect regardless.

### SD Card (4-bit SDIO)

```
Pico 2 GPIO    SD Card Pin    Signal
─────────────────────────────────────
GPIO 18        CLK            Clock
GPIO 19        CMD            Command
GPIO 20        DAT0           Data bit 0
GPIO 21        DAT1           Data bit 1
GPIO 22        DAT2           Data bit 2
GPIO 23        DAT3 / CS      Data bit 3
3V3            VDD            3.3 V power
GND            VSS            Ground
```

> **Important:** Add 10 kΩ pull-ups on CMD, DAT0–DAT3 to 3.3 V.  Most SD breakout boards include these.

---

## Rotary Encoder Disc Selection

The rotary encoder connects to a **MCP23017 I2C GPIO expander**, not directly to the Pico. GPIO 15–17 are reserved for the COMMO bus (connection to the CD32 mainboard).

```
Pico 2 GPIO 28 (SDA) ──→ MCP23017 SDA
Pico 2 GPIO 29 (SCL) ──→ MCP23017 SCL
MCP23017 GPA0 ←── Encoder CLK
MCP23017 GPA1 ←── Encoder DT
MCP23017 GPA2 ←── Encoder SW
MCP23017 GPB0 ←── Logger button (momentary, to GND)
```

**Controls:**
- **Turn CW/CCW** — scroll through disc images (wraps around)
- **Fast spin** — jumps 5 entries per click for long lists
- **Short press** — load the highlighted disc
- **Long press** — eject current disc
- **Logger button** (GPB0) — toggle SD card logging on/off; flushed immediately when turning off

---

## Web Interface

The Pico 2 W includes a built-in WiFi chip. Add credentials to `cd32_ode.cfg` on the SD card:

```ini
wifi_ssid     = YourNetwork
wifi_password = YourPassword
wifi_hostname = cd32ode
```

Then open **http://cd32ode.local/** (or the printed IP address) in any browser.

The web interface shows all disc images as a card grid with cover art, lets you click to load a disc, and auto-refreshes every 5 seconds to show real-time drive status.

### Cover Art

Place JPEG images in a `covers/` folder on the SD card, named to match your disc images:

```
covers/
  Zool2.jpg          ← matches Zool2.iso
  AlienBreed3D.jpg   ← matches AlienBreed3D.bin
  Superfrog.jpg
```

Recommended: 300×300 pixels, 85% JPEG quality.

---

## Building

### Prerequisites

```bash
# Install Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=/path/to/pico-sdk

# Install the SD card library (place alongside this project)
git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git

# Install build tools
sudo apt install cmake gcc-arm-none-eabi build-essential
```

### Build

```bash
cd cd32_ode
mkdir build && cd build
cmake .. -DPICO_PLATFORM=rp2350
make -j8
```

This produces `cd32_ode.uf2`.  Hold BOOTSEL on the Pico 2, connect USB, then drag the UF2 to the `RPI-RP2` drive.

---

## SD Card Setup

1. Format the card as **FAT32** (cards up to 32 GB) or **exFAT** (larger cards)
2. Copy your disc image files to the **root directory**
3. For BIN images: ensure the matching `.cue` file is present with the same base name
4. For MDF images: ensure the matching `.mds` file is present

The firmware loads the **first image** it finds alphabetically on boot.  Connect a serial terminal (USB CDC, 115200 baud) to switch images at runtime:

```
Press 1–9  — select image by number
Press L    — list all found images
```

---

## Architecture Notes

### DA Timing

The DA serial bus carries all disc data to Akiko — both data sectors and CD audio — as a continuous I2S bit stream. There is no separate "data mode" signal: Akiko identifies sector type from the sync pattern embedded in the bit stream.

| Parameter | 1× speed | 2× speed |
|-----------|----------|----------|
| BCLK | 2.12 MHz (measured) | 4.23 MHz |
| LRCLK | 44.1 kHz | 44.1 kHz |
| BCLK cycles per LRCLK half | 48 (24-bit I2S) | 48 |
| Sector period | 13,333 µs | 6,667 µs |
| PIO clkdiv | 32 | 16 |

System clock is set to 135,475,200 Hz (= 16.9344 MHz × 8) so the PIO clock divider is an exact integer for both speeds. BCLK = 135,475,200 / clkdiv / 2. The I2S frame width is 48 BCLK cycles (24 bits per channel), confirmed by 100 M-row logic-analyser capture.

> **Audio note:** Even at 2× data speed, the CD32 will send a PLAY command to lock the drive back to 1× timing when playing audio tracks, to maintain the 44.1 kHz sample rate. The firmware watches for this via COMMO PLAY_TRACK_OPC.

### Sector Delivery

At 2× speed the drive must deliver one 2352-byte sector every 6.667 ms. SD card reads via SDIO take approximately 0.1–0.5 ms per sector, leaving ample margin. The 8-sector ring buffer absorbs any transient SD latency spikes.

### Audio Playback

CD-DA audio data is read from the image file and streamed through the same DA DMA path as data sectors. Raw 16-bit stereo samples are expanded to 24-bit before DMA so each I2S frame carries 24 bits per channel, matching the 48-BCLK-cycle frame width Akiko expects. The LC78835M DAC (U31 on the CD32 mainboard) converts the DA I2S stream to analogue audio directly — no external DAC is needed.

### Clock Reference

GPIO 9 carries the CD32 mainboard's 16.9344 MHz master clock (M17SINE, connector pin 5, GPIN0). The firmware currently receives this clock but does not act on it — the DA PIO runs from the Pico's own crystal at 135,475,200 Hz (= 16.9344 MHz × 8), which nominally matches but is not phase-locked to Akiko. For short sessions this is inaudible; long audio playback can accumulate ±20–50 ppm crystal drift between the two oscillators, eventually causing a faint click as the buffers drift apart. A planned fix is to trim `pio_sm_set_clkdiv()` using an RP2350 frequency counter measuring M17SINE edges.

---

## File Structure

```
cd32_ode/
├── CMakeLists.txt          Build configuration
├── README.md               This file
├── include/
│   ├── cd_types.h          Shared types (msf_t, track_t, disc_image_t)
│   ├── da_output.h         DA PIO + DMA engine API
│   ├── disc_image.h        Disc image abstraction and format structs
│   ├── sector_cache.h      Read-ahead ring buffer types and API
│   ├── commo_bridge.h      COMMO bus bridge API and GPIO/PIO notes
│   └── upstream_types.h    Type shim (cd_time_t ↔ msf_t)
├── src/
│   ├── main.c              Entry point, PIO setup, Core 1 launch, console
│   ├── da_output.c         DA I2S PIO + DMA ping-pong (GPIO 0/1/2)
│   ├── commo_bridge.c      COMMO bus driver + opcode dispatch
│   ├── upstream_player_shim.c  Satisfies upstream player.h linkage
│   ├── disc_image.c        ISO/BIN/NRG/MDF parsers
│   ├── sector_cache.c      Read-ahead ring buffer prefetch (Core 1)
│   ├── subcode.c           Q-channel P/Q/ISRC/MCN generation
│   ├── ecc.c               GF(2^8) EDC and P/Q ECC computation
│   ├── config.c            Persistent settings in RP2350 flash
│   ├── logger.c            Buffered SD card activity logger
│   ├── selftest.c          Hardware self-test suite
│   ├── sd_card.c           SDIO mount and image scan
│   ├── hw_config.c         no-OS-FatFS hardware configuration
│   ├── fft.c               256-point Q15 radix-2 FFT with Hann window
│   ├── effects.c           Demoscene visualiser effects (spectrum/scope/raster/combo/spaceballs)
│   ├── vis_audio.c         DA DMA audio snoop — SPSC ring, left-channel extraction
│   ├── display.cpp         ST7789 cover art display
│   ├── rotary_mcp.cpp      MCP23017 rotary encoder driver
│   ├── ui.c                Disc selector UI event handler
│   └── webserver.c         WiFi web interface (Pico 2 W only)
├── pio/
│   ├── da_output.pio       DA I2S bit-stream PIO (GPIO 0/1/2)
│   └── subcode_encoder.pio Subcode signals PIO (GPIO 5/6/7/8)
├── upstream/               cd32_pico — verbatim 8051 MCU firmware port
│   ├── core/commo.c        COMMO 3-wire serial bus state machine
│   ├── core/dispatcher.c   Packet routing
│   ├── core/cmd_hndl.c     Opcode dispatch
│   ├── core/sts_q_id.c     Status/Q-channel buffer
│   ├── utils/maths.c       BCD/time arithmetic
│   ├── utils/timer.c       8 ms software timer
│   └── pio/commo.pio       COMMO PIO program
└── docs/
    ├── u5.png              Akiko (U5) schematic
    └── u31.png             LC78835M DAC (U31) schematic
```

---

## Settings File (`cd32_ode.cfg`)

On first boot, the firmware creates a `cd32_ode.cfg` template in the root of the SD card. Edit it with any text editor to configure the drive emulator. Changes take effect on the next boot.

```ini
# CD32 ODE Settings File
# Edit this file on the SD card to configure the drive emulator.
# Lines starting with # are comments.  Changes take effect on next boot.

# --- Logging ---
logging_enabled = 1     # 1 = write activity log, 0 = disabled
log_commands    = 1     # Log every CXD command and response
log_sectors     = 0     # Log every sector delivery (very verbose)
log_seeks       = 1     # Log seek start/complete events
log_state       = 1     # Log drive state machine transitions
log_errors      = 1     # Log all error conditions
log_irq         = 0     # Log every IRQ assertion (very verbose)
log_max_kb      = 4096  # Maximum log file size KB (0 = unlimited)
```

---

## Activity Log (`cd32_cd.log`)

When `logging_enabled = 1`, every instruction the drive emulator processes is recorded to `cd32_cd.log` on the SD card root.

### Log format

Each line follows the pattern `[timestamp_ms] LEVEL TAG  message`:

```
================================================================================
[     142] SESSION START — CD32 ODE Firmware Boot
================================================================================
[     142] INFO BOOT  SD card mounted, 3 image(s) found
[     142] INFO BOOT    [1] 0:/Zool2.iso <- selected
[     143] INFO BOOT    [2] 0:/AlienBreed.bin
[     143] INFO BOOT    [3] 0:/Superfrog.nrg
[     701] INFO CMD   0x07 MOTORON
[     701] INFO DRV   state IDLE     → SPINUP
[     701] INFO CMD   0x07 MOTORON   → [02] (BUSY DISC)
[    1401] INFO DRV   state SPINUP   → READY
[    1402] INFO IRQ   flags=0x04 [DISC_DET ]
[    1403] INFO CMD   0x01 GETSTAT   → [04] (DISC)
[    1404] INFO CMD   0x0E SETMODE   params=[30]
[    1404] INFO CMD   0x0E SETMODE   → [04] (DISC)
[    1405] INFO CMD   0x13 GETTN     → [04:01:01] (DISC)
[    1406] INFO CMD   0x14 GETTD     params=[01]
[    1406] INFO CMD   0x14 GETTD     → [04:00:02] (DISC)
[    1407] INFO CMD   0x02 SETLOC    params=[00:02:00]
[    1408] INFO DRV   state READY    → SEEKING
[    1408] INFO SEEK  start from=0      to=0      est=80ms
[    1408] INFO CMD   0x06 READN     → [22] (BUSY DISC DRQ)
[    1488] INFO SEEK  complete LBA=0
[    1488] INFO DRV   state SEEKING  → READING
[    1489] INFO IRQ   flags=0x01 [DATA_END ]
[    2100] WARN WARN  SECT cache miss LBA=300 (bytes=0)
[    4230] ERR  ERR   disc f_read ISO fail lba=512 fr=1 br=0
```

### Turning logging on and off

- **Settings file**: Set `logging_enabled = 0` in `cd32_ode.cfg` before booting.
- **At runtime**: Press `G` in the USB serial console to toggle logging without rebooting.
- **Force flush**: Press `F` to immediately write any buffered log lines to the SD card.

### Log categories

| Category | Setting key | Default | What it captures |
|----------|-------------|---------|-----------------|
| Commands | `log_commands` | On | Every CXD opcode, parameters, and response bytes |
| Sectors | `log_sectors` | **Off** | Every sector LBA, type (MODE1/AUDIO/XA), and byte count |
| Seeks | `log_seeks` | On | Seek start with estimated time, and seek completion |
| State | `log_state` | On | All drive state machine transitions |
| Errors | `log_errors` | On | SD card read failures, cache misses, unknown commands |
| IRQ | `log_irq` | **Off** | Every interrupt assertion (very high volume) |

> **Note:** `log_sectors` and `log_irq` are off by default because at 2× CD speed the drive delivers 150 sectors/second, generating ~9,000 log lines per minute — about 1 MB every 10 minutes. Enable them only for short debugging sessions.

### Log rotation

Set `log_max_kb` to limit the log file size. When the limit is reached the firmware truncates the file and inserts a rotation marker, then continues logging. Setting `log_max_kb = 0` disables rotation (log grows until the SD card is full).

### Reading the log on your PC

The log file is plain UTF-8 text and can be opened in any text editor or processed with standard Unix tools:

```bash
# Show the last 50 lines
tail -50 cd32_cd.log

# Count every command type issued
grep "INFO CMD" cd32_cd.log | awk '{print $5}' | sort | uniq -c | sort -rn

# Find all errors
grep "ERR\|WARN" cd32_cd.log

# Show seek timeline
grep "SEEK\|DRV" cd32_cd.log

# Count total sectors delivered
grep "INFO SECT" cd32_cd.log | wc -l
```

---

## USB Serial Console Commands

Connect a serial terminal at 115200 baud to the Pico 2's USB port:

| Key | Action |
|-----|--------|
| `1`–`9` | Switch to disc image by list number (auto-saves to flash) |
| `L` | List all disc images found on the SD card |
| `S` | Show drive status (DA speed, playing state, COMMO active) |
| `T` | Show disc Table of Contents |
| `X` | Toggle drive speed between 1× and 2× |
| `R` | Reset drive — flush cache and seek to LBA 0 |
| `G` | Toggle SD card logging on/off at runtime |
| `F` | Force-flush the log buffer to `cd32_cd.log` now |
| `H` / `?` | Show help |

---

## Troubleshooting

**Pico not detected by CD32 akiko**
- Scope IF_CLK (GPIO 15) and IF_DATA (GPIO 16) with a logic analyser — you should see COMMO packets after power-on
- Verify GPIO 17 (IF_DIR) toggles direction correctly when the Pico sends responses
- Enable logging and check `cd32_cd.log` for COMMO entries — if none appear, the COMMO bus is not reaching the Pico
- Confirm M17SINE (GPIO 9) shows the 16.9344 MHz clock — the firmware receives it as a reference but does not currently phase-lock to it; the DA PIO runs from the Pico's own 135.475 MHz crystal

**SD card mount fails**
- Check pull-up resistors on CMD and DAT0–3 (10 kΩ to 3.3 V)
- Try a different, freshly formatted card
- Reduce `baud_rate` in `hw_config.c` from 25 MHz to 10 MHz as a debug step

**Games freeze mid-load**
- Enable logging with `log_errors = 1` and look for cache-miss or SD read error lines
- Increase `SECTOR_BUFFER_COUNT` in `CMakeLists.txt` from 8 to 16
- Check that the disc image is not corrupted — run `tools/validate_image.py image.iso`

**Audio tracks silent**
- Audio goes through the DA I2S bus (GPIO 0/1/2) to the LC78835M DAC (U31) on the CD32 mainboard — no external DAC is needed
- Scope DA_BCLK (GPIO 1) — should toggle at ~2.12 MHz during audio playback
- Check COMMO log for PLAY_TRACK_OPC — if absent, akiko is not sending play commands
- Verify DA_LRCLK (GPIO 2) toggles at 44.1 kHz; if it doesn't, the DA PIO has stalled

---


## References

- Sony CXD2545Q datasheet (internal Sony document, referenced via reverse engineering)
- DuckStation CDROM.cpp — GPL-2.0, Connor McLaughlin — command protocol reference
- fuseiso — GPL, Heikki Hannikainen — image format parsing reference
- ECMA-130 (ISO/IEC 10149) — CD-ROM physical format specification
- no-OS-FatFS-SD-SDIO-SPI-RPi-Pico — MIT, Carl Kugler — SD card library

##
Thanks to Xvortex, Mick & FastDruid on the EAB forums.

---

## Licence

This project is released under the GNU General Public Licence v2.0, consistent with DuckStation and fuseiso, whose work informed the protocol and format parsing here.
