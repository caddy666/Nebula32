# CD32 Optical Drive Emulator — Raspberry Pi Pico 2

Replaces the optical drive mechanism in a Commodore CD32 with a Raspberry Pi Pico 2 (RP2350) reading ISO, BIN/CUE, NRG, and MDF disc images from an SD card.

---
this readme  needs updating.....

however:
now with 100% more TLS than before...added some PSRAM - 8mb. seems to work a lot better with it.


Grüße an a1k.org, ich sehe euch. Es ist amüsant, dass du das so genau vorhergesagt hast. Offenbar dachten wir alle dasselbe.

605 unit tests, 0 failures. All 7 new tests pass - at some point i'll get around to building the hardware....but until then might as well get as many of these pesky bugs out before hand. also, i might be doing this the wrong way around, but meh. its a first try...


this better be good - there are 20+ gb of tests, the bloody firmware is less than 350k.

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
| ISO 9660 | `.iso` | 2048 bytes | No | iso doesnt do audio as a format....
| Raw binary | `.bin` + `.cue` | 2352 bytes | Yes |
| Nero Burning ROM | `.nrg` | 2048 or 2352 | Yes |
| Alcohol 120% | `.mdf` + `.mds` | 2048 or 2352 | Yes |

For BIN images, place the `.cue` file with the same base name in the same directory. For MDF images, place the `.mds` metadata file alongside the `.mdf`.

---

## Hardware Requirements so far...

| Component | Notes |
|-----------|-------|
| Raspberry Pi Pico 2 W | RP2350 + CYW43439 WiFi — **Pico 2 W required** for web interface |
| Raspberry Pi Pico 2 | Non-W variant works without web interface |
| MicroSD card | SDIO 4-bit wiring required (not SPI-only breakout boards) |
| KY-040 rotary encoder | For hardware disc selection without a screen |
| st7789 screen      | to see what you're doing, really |
| Logic level translators | normally required — CD32 drive connector signals are 5 V |

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


