# Nebula32 — Code Index

Subsystem map for fast navigation.  
Symbol lookups: `grep "^symbol_name" tags`  
Concept lookups: `grep -n "anchor:tag" src/`  
Full architecture: `CLAUDE.md`

---

## GPIO / Pin assignments

| File | Role |
|------|------|
| `include/gpio_map.h` | **Single source of truth** — every `PIN_*` define with connector cross-refs |
| `src/hw_config.c` | SDIO bus descriptor + FatFS `VolToPart[]` partition map |

Key anchors: `[anchor:sdio_pins]` · `[anchor:partition_map]`  
PIO resource table: see `CLAUDE.md § PIO resource allocation`

---

## Boot sequence & main loop  `src/main.c`

| Anchor | What it marks |
|--------|--------------|
| `[anchor:sysclk]` | `TARGET_SYS_CLK_KHZ` — 135,475,200 Hz = 16.9344 MHz × 8 |
| `[anchor:boot_sequence]` | `int main()` — 13-step init (clock → SD → DA PIO → COMMO → Core 1) |
| `[anchor:da_pio_init]` | DA output PIO + DMA init |
| `[anchor:m17sine_clk]` | `clk_peri` optionally slaved to Sony 16.9344 MHz reference on GPIO 9 |
| `[anchor:commo_bridge_init]` | COMMO PIO1 SM0/SM1 arm |
| `[anchor:core1_prefetch]` | Core 1 sector-prefetch loop (`sector_cache_prefetch_tick`) |
| `[anchor:main_loop]` | Core 0 polling loop — COMMO · webserver · UI · visualiser |

---

## DA audio output  (SD → DMA → Akiko)

| File | Role |
|------|------|
| `src/da_output.c` | DMA engine: `da_start_play` · `da_stop` · `da_resume` · `da_nudge_clkdiv_to_m17sine` |
| `pio/da_output.pio` | 24-bit I2S PIO; clkdiv=32 → BCLK 2.117 MHz; 48 BCLK per LRCLK half |
| `src/ecc.c` | EDC/ECC P+Q parity for Mode 1 sector synthesis |
| `src/vis_audio.c` | SPSC snoop ring — DMA → FFT pipeline (SRAM-pinned) |
| `src/fft.c` | 256-point Q15 radix-2 FFT with Hann window |

GPIO: 0 DA_DATA · 1 DA_BCLK · 2 DA_LRCLK · 3 DA_C2PO · 4 DA_EMPH  
PIO: PIO0 SM0

---

## Subcode output

| File | Role |
|------|------|
| `src/subcode.c` | Q-channel byte generation (`subcode_generate_q`) |
| `pio/subcode_encoder.pio` | SUB serial output; `.side_set 1` drives SUB_CLK |
| `include/subcode.h` | `subcode_push_to_pio()` — clears FIFO on partial push |

GPIO: 5 SUB_DATA · 6 SUB_CLK · 7 SUB_WFCLK · 8 SUB_SCOR  
PIO: PIO0 SM1; called from DMA ISR in `da_output.c`

---

## COMMO 3-wire bus  (CD32 ↔ Drive MCU)

| File | Role |
|------|------|
| `src/commo.c` | Protocol state machine — `commo_poll()` / `commo_ctx_t` |
| `include/commo_hal.h` | HAL interface: `commo_hal_rxd` · `txd` · `data_is_low` · `release` |
| `src/commo_hal_pico.c` | Real PIO + GPIO implementation `[anchor:commo_hal]` |
| `tests/host/commo_hal_stub.c` | Test-harness HAL (byte queue + TX capture log) |
| `src/commo_bridge.c` | Opcode dispatch: `commo_bridge_poll` · `commo_bridge_init` |
| `src/dispatcher.c` | Packet routing |
| `src/cmd_hndl.c` | Per-opcode handler table |
| `pio/commo.pio` | Bit-bang RX/TX; RX ack pulse extended to [31] ≈ 237 ns |

GPIO: 44 IF_CLK · 45 IF_DATA · 46 IF_DIR  
PIO: PIO1 SM0 (RX) · SM1 (TX)

---

## Disc images & sector pipeline

| File | Role |
|------|------|
| `src/disc_image.c` | Parsers: ISO / BIN+CUE / NRG / MDF; `disc_open` · `disc_read_sector` |
| `src/sector_cache.c` | 8-slot prefetch ring; `sector_cache_prefetch_tick` (Core 1); flush_gen race guard |
| `src/virtual_disc.c` | ISO 9660 synthesis from SD partition 2 (`disc_open_vdir`) |
| `include/disc_image.h` | `disc_image_t` · `track_t` (file_offset = uint64_t for >4 GB) |
| `include/sector_cache.h` | `sector_cache_t` · `sector_slot_t` |

---

## SD card

| File | Role |
|------|------|
| `src/sd_card.c` | SDIO mount · `sd_scan_images` · `sd_count_images` (alphabetical order) |
| `src/hw_config.c` | Library hw_config hook (`sd_get_num` / `sd_get_by_num`) |
| `include/ffconf.h` | Project-owned FatFS config (shadows `libs/`); `FF_MULTI_PARTITION=1` |

GPIO: 30 CLK · 31 CMD · 32–35 D0–D3  
PIO: PIO1 SM2/SM3 (claimed by library)  
Partitions: `"0:/"` → images/config/covers · `"1:/"` → virtual CD content

---

## WiFi / Web server  (pico2_w build only)

| File | Role |
|------|------|
| `src/webserver.c` | lwIP TCP server — `webserver_init` · `webserver_poll`; disc list, cover JPEG, firmware flash endpoint |
| `include/user_settings.h` | wolfSSL shadow config (mirrors `ffconf.h` pattern) |
| `libs/wolfssl-master/` | TLS engine — do not modify; integration path: `WOLFSSL_LWIP_NATIVE` |

GPIO (RM2 WiFi): 23 WL_ON · 24 WL_DIN · 25 WL_CS/blue LED · 29 WL_CLK · 39 LED_RED

---

## Display & UI

| File | Role |
|------|------|
| `src/display.cpp` | ST7789 240×240; `display_show_cover` · `display_fw_progress` · Boing Ball animation |
| `src/effects.c` | 6 visualiser effects: SPECTRUM · SCOPE · RASTER · COMBO · SPACEBALLS · JUGGLER |
| `src/ui.c` | Event handler — `ui_tick` · `ui_on_disc_loaded` |
| `src/rotary_mcp.cpp` | MCP23017 rotary encoder + logger toggle (GPB0) |

GPIO display: 40 DC · 41 CS · 42 SCK · 43 MOSI (SPI1)  
GPIO encoder: 26 SDA · 27 SCL (I2C1 @ 400 kHz)

---

## Firmware update

| File | Role |
|------|------|
| `src/fw_update.c` | UF2 self-flash: `fw_validate` → `fw_flash_and_reboot`; Bank1 write → verify → copy → watchdog reboot |
| `include/fw_update.h` | `fw_result_t` · `FW_UPDATE_MAGIC` watchdog scratch sentinel |

Triggered at boot if `0:/NEBULA32.UF2` exists; also via web endpoint `/api/fw/flash/`.

---

## Config & Logger

| File | Role |
|------|------|
| `src/config.c` | Flash-backed `ode_config_t`; `config_init` · `config_save` (multicore lockout around flash write) |
| `src/logger.c` | SD ring log; `logger_init` · `logger_write` · `logger_flush_if_due` |

Config reads `nebula32.cfg` from SD root. Logger writes `cd32_cd.log`.

---

## PSRAM

| File | Role |
|------|------|
| `src/psram.c` | QSPI init (`psram_fw_init`); `psram_alloc` bump allocator |
| `include/psram.h` | `PSRAM_CS1_PIN = 47`; `BUILD_WITH_PSRAM` compile gate |

wolfSSL allocators deliberately NOT routed to PSRAM (bump allocator never reclaims).

---

## Math utilities

| File | Role |
|------|------|
| `src/maths.c` | BCD↔hex · `lba_to_msf` · `msf_to_lba` · `tracks_calc` (uint64_t) |
| `src/ecc.c` | EDC CRC-32 · P-parity (24 rows) · Q-parity (ECMA-130 diagonal stride 44 mod 2236) |
| `src/timer.c` | 8 ms software timer · `delay()` · SCOR IRQ (gated by `BUILD_WITH_COMMO`) |

---

## Tests  `tests/host/`

```
make && ./cd32_tests -v          # 605 tests — main suite
make parser_tests && ./parser_tests -v    # 33 tests — disc parsers
make vdisc_tests && ./vdisc_tests -v      # 73 tests — virtual disc ISO 9660
make stress_sector_cache && ./stress_sector_cache  # 5 TSan tests
```

HAL stub for COMMO tests: `tests/host/commo_hal_stub.c` / `commo_hal_stub.h`  
Sanitizer: `-fsanitize=undefined -fno-sanitize-recover=all` on all objects.

---

## Vendor libraries  `libs/`

**Never modify.** Override vendor headers by shadowing in `include/` (same pattern as `ffconf.h` / `user_settings.h`).

| Library | Path | Used for |
|---------|------|----------|
| no-OS-FatFS-SD-SDIO-SPI-RPi-Pico | `libs/no-OS-FatFS-*` | 4-bit SDIO + FatFS |
| wolfSSL 5.9.1 | `libs/wolfssl-master/` | TLS (`WOLFSSL_LWIP_NATIVE`) |
| JPEGDEC | (linked in display) | Cover art JPEG decode |
