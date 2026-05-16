# CD32 ODE — Architecture Reference

## Verified hardware architecture (schematics docs/u5.png, docs/u31.png)
_Use_ _incremental_ _edit/write_ _calls_ - breaks larger files into logistical sections 
_Never_ _output_ _entire_ _large_ _files_ _in_ _a_ _single_ _tool_ _call_
_Split_ _by:_ _imports,_ _functions,_ _classes,_ _configuration_ _blocks_

```
68EC020 CPU
    ↕ chipset bus
Akiko (U5, 391563-01)          ← custom Commodore chip
    ↕ COMMO 3-wire serial (IF_CLK / IF_DATA / IF_DIR)
Drive MCU (originally 8051; Pico 2 in this project)
    ↕ SPI-like serial
CXD2500BQ / CXD2545Q (CD DSP — EFM decode, error correction)
    ↕
DSIC2 (servo — focus, radial, sled)
    ↕
Optical pickup / disc
```

The CXD chip outputs data BACK to Akiko over the DA/SUB lines:

```
CXD2545Q → DA_DATA / DA_BCLK / DA_LRCLK ──→ Akiko (pin 82/89/79)
                                          └──→ LC78835M U31 DAC (pin 6/5/7)
CXD2545Q → DA_C2PO / DA_EMPH            ──→ Akiko (pin 83) / LC78835M (pin 14)
CXD2545Q → SUB_DATA / SUB_CLK / SUB_WFCLK / SUB_SCOR ──→ Akiko (pin 90/−/91/88)
```

**There is no parallel bus.** The CXD chip is not register-mapped by Akiko. Akiko
reads a serial bit stream from the DA lines and a serial subcode stream from the
SUB lines. The COMMO 3-wire bus is the command/status channel.

### 26-pin CD connector — complete signal list

| Pin | Signal     | Pico GPIO | Direction | Notes |
|-----|------------|-----------|-----------|-------|
| 5   | M17SINE    | 9         | IN        | 16.9344 MHz master clock reference |
| 7   | RESET      | 14        | IN        | Active-low reset from CD32 |
| 8   | DA_LRCLK   | 2         | OUT       | I2S word-select (44.1 kHz) |
| 9   | DA_DATA    | 0         | OUT       | I2S serial data — audio + sector bits |
| 11  | DA_BCLK    | 1         | OUT       | I2S bit clock (~2.12 MHz measured) |
| 12  | DA_C2PO    | 3         | OUT       | C2 error pointer (drive low = no errors) |
| 13  | DA_EMPH    | 4         | OUT       | Pre-emphasis flag (drive low = no emphasis) |
| 14  | SUB_WFCLK  | 7         | OUT       | Subcode word-frame clock |
| 15  | SUB_SCOR   | 8         | OUT       | Subcode sync correlator |
| 17  | SUB_DATA   | 5         | OUT       | Subcode serial data |
| 18  | SUB_CLK    | 6         | OUT       | Subcode clock |
| 20  | IF_CLK     | 15        | BIDIR     | COMMO clock (idles high, active-low pulses) |
| 21  | IF_DATA    | 16        | BIDIR     | COMMO data (setup ≥150 ns before CLK edge) |
| 23  | PASSIVE    | 13        | OUT       | Drive passive/standby status |
| 24  | ACTIVE     | 10        | OUT       | Drive active/spinning status; also drives front-panel drive LED |
| 25  | IF_DIR     | 17        | OUT       | COMMO direction control |
| 26  | DOOR       | 11        | IN        | Door/tray switch |

### DA timing — confirmed from 100M-row logic-analyzer capture (digital.csv)

**Measured values supersede earlier theoretical calculations.**

| Parameter | Measured (1×) | Notes |
|-----------|--------------|-------|
| DA_BCLK half-period | **236 ns** mean, σ=28 ns | → **2.12 MHz** frequency |
| DA_BCLK high time | **245 ns** | duty cycle ~52% (asymmetric) |
| DA_BCLK low time | **226 ns** | high phase is longer |
| DA_LRCLK half-period | **11 339 ns** | → **44.1 kHz** word clock |
| LRCLK/BCLK half-period ratio | **48** | 48 BCLK cycles per channel per LRCLK period |
| Min event gap (analyzer limit) | **62 ns** | 16 MHz sample rate |

**Critical finding — 48-cycle I2S frame, not 32:**
The LRCLK/BCLK ratio of 48 means the CXD2545Q transmits **48-bit frames**
(24 bits L + 24 bits R per LRCLK period), not the 32-bit frames (16+16) originally
assumed.  The ODE's `da_output.pio` must clock 48 BCLK edges per LRCLK half-period
or Akiko's deserialiser will lose framing sync.

**Required sys_clock calculation:**
With sys_clk = 135,475,200 Hz, to achieve BCLK ≈ 2.12 MHz:
- If PIO toggles BCLK every SM clock: SM_clk = 2 × 2.1168 MHz = 4.234 MHz
- clkdiv = 135,475,200 / 4,234,000 ≈ **32** for 1× speed
- clkdiv = **16** for 2× speed (~4.23 MHz BCLK)

The firmware was originally using clkdiv=48 (→ 1.41 MHz).
`da_output.pio` and `da_output_init()` have been corrected to clkdiv=32 (see HIGH-4).

**DA_DATA sampling convention:**
Data transitions on the **falling** BCLK edge; Akiko samples on the **rising** edge.

---

## Codebase status

| File | Status | Notes |
|------|--------|-------|
| `upstream/core/commo.c` | ✅ Correct | COMMO command/status protocol; last_command cleared on CMD_ERROR |
| `upstream/core/dispatcher.c` | ✅ Correct | Packet routing |
| `upstream/core/cmd_hndl.c` | ✅ Correct | Opcode dispatch |
| `upstream/core/sts_q_id.c` | ✅ Correct | Status/Q-channel buffer |
| `upstream/utils/maths.c` | ✅ Correct | BCD/time arithmetic; tracks_calc() widened to uint64_t |
| `upstream/utils/timer.c` | ✅ Correct | 8 ms software timer; delay() zero-entry guard |
| `upstream/pio/commo.pio` | ✅ Correct | COMMO PIO |
| `src/disc_image.c` | ✅ Correct | ISO/BIN/NRG/MDF parsers; CUE PREGAP+INDEX00 fix; NRG lead-out skip fix; NRG chunk_size=0/DAOX-too-short guards; MDF sector_size=0 guard; CUE/MDF track-length underflow clamp; NRG uint64_t aligned memcpy |
| `src/sector_cache.c` | ✅ Correct | SD prefetch ring buffer + flush_gen race fix; hard_assert null-cache guard in prefetch_tick; __atomic_acquire/release builtins replace volatile+__dmb() |
| `src/subcode.c` | ✅ Correct | Q-channel generation |
| `pio/subcode_encoder.pio` | ✅ Correct | SUB signal output on GPIO 5-8 |
| `src/sd_card.c` | ✅ Correct | SDIO mount/scan |
| `src/hw_config.c` | ✅ Correct | FatFS hardware config |
| `src/ecc.c` | ✅ Correct | EDC/ECC computation |
| `src/logger.c` | ✅ Correct | SD activity log |
| `src/config.c` | ✅ Correct | Flash-backed settings |
| `src/display.cpp` | ✅ Correct | ST7789 240×240 cover art + scanline API |
| `src/ui.c` | ✅ Correct | UI event handler |
| `src/webserver.c` | ✅ Correct | WiFi web interface |
| `src/da_output.c` | ✅ Correct | DA DMA engine — 24-bit I2S expand, DRQ flag, FIFO drain on stop, M17SINE clkdiv trim; hard_assert on DMA ch claims |
| `src/rotary_mcp.cpp` | ✅ Correct | MCP23017 encoder + logger toggle button (GPB0) |
| `src/commo_bridge.c` | ✅ Correct | PLAY_TRACK_OPC BCD decode, TRAY_IN seek, audio mode set, DRQ packet |
| `src/upstream_player_shim.c` | ✅ Correct | Linkage shim — provides player_interface globals and no-op player() for upstream cmd_hndl.c |
| `src/fft.c` | ✅ Correct | 256-point Q15 radix-2 FFT with Hann window |
| `src/effects.c` | ✅ Correct | 5 demoscene visualiser effects (SPECTRUM/SCOPE/RASTER/COMBO/SPACEBALLS) |
| `src/vis_audio.c` | ✅ Correct | DA DMA snoop — SPSC ring, left-channel extraction |
| `pio/da_output.pio` | ✅ Correct | 24-bit I2S frames, clkdiv=32 (2.12 MHz), 96 SM cycles/pair → 44.1 kHz |
| `tests/host/test_csv_replay_pon_poff.cpp` | ✅ Correct | CsvReplayPonPoff: 10 windowed tests against pon-poff-idle.csv |
| `tests/host/test_csv_replay_zool2.cpp` | ✅ Correct | CsvReplayZool2: 10 windowed tests against zool2.csv |
| `tests/host/test_csv_replay_pinball.cpp` | ✅ Correct | CsvReplayPinball: 10 windowed tests against pinball.csv |
| `tests/host/test_door_tray.cpp` | ✅ Correct | DoorTray: 10 tests — insert/eject state, ACTIVE pin/LED, status bits; DoorPin: 8 tests — GPIO edge detection, boot snapshot, motor/LED state |
| `tests/host/test_opc_responses.cpp` | ✅ Correct | OpcResponses: 34-case table test — every COMMO opcode → status + state (incl. SEEK from PLAYING, SEEK invalid BCD) |
| `tests/host/test_motor_sled_fake.cpp` | ✅ Correct | MotorSledFake: 10 tests — motor active states, BCD→LBA, virtual sled seek |

---

## PIO resource allocation (merged build)

| PIO | SM | Program | Pins | Purpose |
|-----|----|---------|------|---------|
| PIO0 | 0 | `da_output.pio` | GPIO 0-2 | DA_DATA/BCLK/LRCLK output |
| PIO0 | 1 | `subcode_encoder.pio` | GPIO 5-8 | SUB_DATA/CLK/WFCLK/SCOR |
| PIO1 | 0 | `commo.pio` | GPIO 15-17 | COMMO RX |
| PIO1 | 1 | `commo.pio` | GPIO 15-17 | COMMO TX |

All four PIO programs are always loaded. The upstream servo PIO programs
(cxd2500_tx.pio, dsic2.pio, qchannel_rx.pio) have been deleted.

---

## Test suite

**Location:** `tests/host/`  
**Run:** `make && ./cd32_tests -v`  
**Result:** 426 tests, 0 failures  
**Parser tests:** `make parser_tests && ./parser_tests -v` → 28 tests, 0 failures (separate binary; uses FatFS injectable sim)
**Stress tests:** `make stress_sector_cache && ./stress_sector_cache` → 5 tests, 0 failures (TSan binary; concurrent producer/consumer)
**Sanitizer:** `-fsanitize=undefined -fno-sanitize-recover=all` active on all C and C++ objects and the link step

| Group | Tests | What it covers |
|-------|-------|----------------|
| `LbaMsf` | 11 | LBA↔MSF conversion, BCD encoding, round-trips |
| `Ecc` | 13 | EDC round-trip, single-bit flip detection, MSF header flip, last-data-byte flip, sync pattern inside payload passes EDC |
| `Subcode` | 14 | CRC-16, CONAD byte, BCD track, absolute MSF |
| `SubcodePio` | 3 | subcode_push_to_pio() PIO TX FIFO back-pressure: all 3 words pushed (never-full), first word rejected (full before push), 3rd word blocked (full after 2) |
| `Fft` | 7 | Init, zero input, range, peak hold/decay |
| `VisAudio` | 6 | SPSC ring, left-channel extraction, FIFO order |
| `SectorCache` | 12 | Slot injection, seek/flush, flush_gen counter, partial-sector valid_bytes |
| `Maths` | 30 | BCD↔hex, add/subtract time, compare, calc_tracks |
| `CsvReplay` | 10 | Real hardware signal validation (digital.csv, 4.9 GB, 100M rows) |
| `CsvReplayPonPoff` | 10 | Power-on/power-off idle capture (pon-poff-idle.csv, 5.2 GB) |
| `CsvReplayZool2` | 10 | Zool 2 gameplay capture (zool2.csv, 12.1 GB) |
| `CsvReplayPinball` | 10 | Pinball Illusions capture (pinball.csv, 5.6 GB) |
| `DoorTray` | 10 | Disc insert/eject state machine, ACTIVE pin/LED, status bits |
| `DoorPin` | 8 | GPIO door-pin edge detection: rising edge ejects, stable/falling silent, boot snapshot, motor/LED state |
| `OpcResponses` | 1 | All 34 COMMO opcodes → correct status byte + drive state (table-driven; incl. SEEK from PLAYING, SEEK invalid BCD) |
| `MotorSledFake` | 10 | Motor active states, BCD MSF→LBA, seek LBA, virtual sled positioning |
| `DiscFindTrack` | 8 | disc_find_track boundaries, single/multi-track, track type |
| `TocResponse` | 6 | disc_build_toc_response BCD encoding, lead-out 0xAA, truncation |
| `ConfigCrc` | 5 | CRC-32 known values, determinism, single-bit sensitivity |
| `CueMsfArith` | 6 | CUE decimal→BCD→LBA conversion, PREGAP file-offset arithmetic |
| `NrgTrackCalc` | 6 | NRG track length, lead-out/lead-in skip conditions |
| `IsoLayout` | 6 | ISO parser output structure, disc_find_track, TOC lead-out |
| `LbaFileOffset` | 3 | LBA→byte-offset formula: ISO (×2048), raw BIN (×2352), multi-track non-zero start_lba |
| `Webserver` | 19 | basename_no_ext, state_name, config key=val parser, ".." traversal guard, load-index bounds |
| `Logger` | 22 | parse_bool truthy/falsy set, trim all whitespace variants, cmd_name known/unknown, ring buffer append/wrap/overflow, status byte flag decode |
| `Config` | 17 | struct size, defaults field values, CRC validity after defaults, CRC excludes crc32 field, magic/version/CRC guard, flag bit orthogonality |
| `DaExpand` | 10 | I2S word packing: zero sector, L/R separation, max/min int16_t, lower-half always zero, pair-N addressing, last pair, clkdiv 32/16 |
| `DaSpeed` | 16 | DA playback state machine: init 1×/2×, start/pause/resume/stop transitions, da_set_double_speed idempotency, clkdiv write capture, speed change while playing |
| `CommoProtocol` | 13 | COMMO state machine: single-byte opcode, opcode+param, bad checksum, same-command detection, zero opcode ERR_SEND countdown, free-buffer clear, max-param opcode, A→B→A sequence, CMD_ERROR clears last_command (fixed), TX data+checksum byte capture, BUSY/READY states |
| `CommoFuzz` | 6 | Adversarial COMMO paths: aborted command (param byte consumed as checksum → CMD_ERROR + recovery), 50-tick rapid poll stays IDLE, TX blocked by spurious data strobe without corruption, TXD_CHECKSUM state also blocks on data_is_low, successive errors keep last_command=0 so retry is NEW_COMMAND, rapid-fire second command overwrites first (Amiga game engine bug) |
| `EffectsColor` | 18 | rgb() RGB565 bswap packing, hsv() grey/red/black/distinct hues, copper_color() darkest/brightest/monotone, sample_to_y() centre/top/bottom/bounds |
| `CoverDir` | 6 | display.cpp and webserver.c agree on cover-art directory; FatFS volume prefix; trailing slash; default path starts in covers dir |
| `SectorLayout` | 5 | disc_synthesise_sector() sync pattern, MSF header bytes, mode byte 0x01, data payload copy |
| `FormatDetect` | 29 | ext_match() case-insensitive extension detection for .iso/.bin/.nrg/.mdf; uppercase; unknown format rejected; Unix slash path; Windows backslash path; space in filename; dot-in-directory-component not confused; Swedish/Danish/Norwegian (Ä Ö Å Ø Æ); German umlauts + ß; French accents; Spanish Ñ; Czech/Slovak carons (Š Č Ž); Polish (Ł Ź Ą); Hungarian double-acute (Ő Ű); Icelandic Þ/Ð; Portuguese Ã/Ç; multi-script Latin paths; Japanese katakana (3-byte); Japanese path; Chinese simplified hanzi; Korean hangul; mixed Latin+CJK paths; uppercase ASCII ext still folds over non-ASCII stems; non-ASCII filename with no extension never matches |
| `AkikoDma` | 8 | Fake Akiko DMA engine (REPLICA pattern): ping-pong sequence, interrupt mid-transfer, cache-miss abort, next_lba tracking, card-yank exact delivery count, post-reset buf/lba zeroing, silence-pad UINT32_MAX never delivered, I2S word-packing data integrity (left/right channel expand) |
| `CoreIpcDesync` | 5 | Dual-core IPC boundary: Core 1 stall → graceful stop, tick-after-stop no-op, all-slots-full prefetch_tick blocked safely, fill/drain/fill second batch correct, single-sector silence-pad buf_lba[1]=UINT32_MAX never received |
| `SubchannelMath` | 15 | Q-channel relative time: index 01 zero at track start, index 00 countdown 1 frame/1 second, pregap boundary no uint32_t underflow, absolute time 2-second lead-in offset, index BCD 0x00/0x01 flip, data/audio CTRL nibble (0x41/0x01), two-digit track BCD, 1-minute relative time, CRC self-consistency; multiple indices: index 2 BCD, index 10 double-digit BCD, relative time from index 2 uses track_start_lba |

**stress_sector_cache groups (TSan binary — `make stress_sector_cache`):**

| Test | What it covers |
|------|----------------|
| `queue_empty_boundary` | sector_cache_get returns false on fresh cache |
| `queue_full_boundary` | prefetch_tick returns early when all 8 slots are valid; no crash or overwrite |
| `flush_gen_stale_guard` | seek flushes gen; post-seek prefetch lands at new LBA; stale sector evicted |
| `fill_drain_fill_cycle` | fill 8 slots, drain all, seek + fill 8 more; second batch data integrity verified |
| `concurrent_produce_consume` | producer+consumer pthreads: 500 sectors, integrity check, TSan reports no races |

**parser_tests groups (separate binary):**

| Group | Tests | What it covers |
|-------|-------|----------------|
| `ParseIso` | 4 | Empty file, exact 1 sector, partial sector, multi-sector count |
| `ParseBin` | 7 | No CUE fallback, track 0/over-limit skip, INDEX-before-TRACK guard, unknown mode default, pregap underflow clamp, overlapping-track length clamp |
| `ParseNrg` | 6 | File too small, no magic, chunk_size=0 guard (FIX-1), DAOX too short (FIX-2), lead-out skip, valid track, astronomical end_lba |
| `ParseMdf` | 4 | Wrong signature, short header, zero sessions, sector_size=0 guard (FIX-3), lead-in/lead-out skip |
| `SectorAccess` | 5 | Read LBA 0 on ISO, read LBA 0 on raw BIN, read LBA past total_sectors returns 0, unaligned buffer no UBSan fault, non-sequential backwards read returns correct data per LBA |

### CsvReplay test windows

Each test reads 50 000 rows from a distinct 10% slice of its capture file.
All four captures share the same 10 test names (W0–W9) and assertion thresholds.

**digital.csv** — 4.9 GB, DA_BCLK=col4, DA_LRCLK=col2 (original reference capture):

| Test | Window offset | Asserts |
|------|--------------|---------|
| `W0_BclkMeanHalfPeriodInRange` | 0 MB | BCLK half-period 150–400 ns |
| `W1_BclkJitterLow` | 520 MB | BCLK σ < 60 ns |
| `W2_LrclkHalfPeriodInRange` | 1040 MB | LRCLK half-period 9 000–14 000 ns |
| `W3_LrclkBclkRatioIs48` | 1560 MB | ratio 44–52 (24-bit I2S frames) |
| `W4_TimestampsMonotonic` | 2080 MB | timestamps non-decreasing |
| `W5_MinEventSpacingAtLeast62ns` | 2600 MB | min gap ≥ 62 ns |
| `W6_BclkHighTimeLongerThanLow` | 3120 MB | duty cycle > 50% |
| `W7_CaptureIsBclkEdgeDominated` | 3640 MB | > 90% of rows are BCLK transitions |
| `W8_AllSignalColumnsAreBinary` | 4160 MB | all 8 signals ∈ {0,1} |
| `W9_BclkHighTimeStdDevUnder50ns` | 4680 MB | high-time σ < 50 ns |

**pon-poff-idle.csv** — 5.2 GB, DA_BCLK=col4, DA_LRCLK=col2, window step 519 912 970 B:

| Test | Window offset |
|------|--------------|
| W0 | 0 B |
| W1 | 519 912 970 B |
| W2 | 1 039 825 940 B |
| W3 | 1 559 738 910 B |
| W4 | 2 079 651 880 B |
| W5 | 2 599 564 850 B |
| W6 | 3 119 477 820 B |
| W7 | 3 639 390 790 B |
| W8 | 4 159 303 760 B |
| W9 | 4 679 216 730 B |

**zool2.csv** — 12.1 GB, DA_BCLK=col5, DA_LRCLK=col3, window step 1 212 752 795 B:

| Test | Window offset |
|------|--------------|
| W0 | 0 B |
| W1 | 1 212 752 795 B |
| W2 | 2 425 505 590 B |
| W3 | 3 638 258 385 B |
| W4 | 4 851 011 180 B |
| W5 | 6 063 763 976 B |
| W6 | 7 276 516 771 B |
| W7 | 8 489 269 566 B |
| W8 | 9 702 022 361 B |
| W9 | 10 914 775 156 B |

**pinball.csv** — 5.6 GB, DA_BCLK=col4, DA_LRCLK=col3, window step 563 033 060 B:

| Test | Window offset |
|------|--------------|
| W0 | 0 B |
| W1 | 563 033 060 B |
| W2 | 1 126 066 120 B |
| W3 | 1 689 099 180 B |
| W4 | 2 252 132 240 B |
| W5 | 2 815 165 300 B |
| W6 | 3 378 198 360 B |
| W7 | 3 941 231 420 B |
| W8 | 4 504 264 480 B |
| W9 | 5 067 297 540 B |

---

## Completed fixes (critical/high priority)

| ID | File | Fix |
|----|------|-----|
| CRITICAL-1 | `commo_bridge.c` | `PLAY_TRACK_OPC` decodes p1 as BCD track number, looks up start LBA |
| CRITICAL-2 | `da_output.c` | `subcode_push_to_pio()` called from DMA ISR on every sector |
| CRITICAL-3 | `da_output.c` | `da_stop()` drains PIO TX FIFO + restarts SM to clean sync alignment |
| CRITICAL-5 | `sector_cache.c/h` | `flush_gen` counter prevents Core 1 committing stale in-flight reads |
| HIGH-1 | `commo_bridge.c` | `TRAY_IN_OPC` calls `sector_cache_seek(&g_cache, 0)` |
| HIGH-2 | `commo_bridge.c` | Unknown opcodes return `_build_status()`, not `DRIVE_STATUS_ERROR` |
| HIGH-5 | `da_output.c` | `da_resume()` calls `sector_cache_seek()` to realign Core 1 prefetch |
| MEDIUM-7 | `commo_bridge.c` | `da_set_audio_mode()` called in `PLAY_TRACK_OPC` and `PAUSE_OFF_OPC` |
| HIGH-3 | `da_output.c` / `commo_bridge.c` | DRQ flag set by DMA ISR; `commo_bridge_poll()` sends `DRIVE_STATUS_DRQ` packet |
| HIGH-4 | `da_output.pio` / `da_output.c` | 24-bit I2S frames (clkdiv=32), `expand_to_i2s24()` for DMA buffers |
| UPSTREAM-1 | `upstream/core/commo.c` | `last_command = 0` on CMD_ERROR so retry is treated as NEW_COMMAND |
| UPSTREAM-2 | `upstream/utils/maths.c` | `isqrt()` + `tracks_calc()` widened to uint64_t; A×T overflowed uint32_t for discs > ~20 min |
| UPSTREAM-3 | `upstream/utils/timer.c` | `delay()` guard for `delay_byte == 0` on entry; do-while wrapped to 255 and blocked 127.5 ms |
| UPSTREAM-4 | `upstream/core/play.c` | Pointer truncation: `param1 = (uint8_t)(uintptr_t)&store` → side-channel `play_subcode_result` pointer |
| UPSTREAM-5 | `upstream/core/play.c` | `jump_time()` overshoot: `compare_time` guard before `subtract_time`; underflow wrapped `delta.frm` to 75−N |

---

## Remaining work

_All items complete._
