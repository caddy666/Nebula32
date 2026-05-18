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
| `upstream/core/commo.c` | ✅ Correct | COMMO command/status protocol; last_command cleared on CMD_ERROR; vestigial commo_data_is_low() during TX documented |
| `upstream/core/dispatcher.c` | ✅ Correct | Packet routing |
| `upstream/core/cmd_hndl.c` | ✅ Correct | Opcode dispatch |
| `upstream/core/sts_q_id.c` | ✅ Correct | Status/Q-channel buffer |
| `upstream/utils/maths.c` | ✅ Correct | BCD/time arithmetic; tracks_calc() widened to uint64_t |
| `upstream/utils/timer.c` | ✅ Correct | 8 ms software timer; delay() zero-entry guard; SCOR IRQ guarded by #if !BUILD_WITH_COMMO (F25) |
| `upstream/pio/commo.pio` | ✅ Correct | COMMO PIO; RX acknowledge pulse extended to [31] delay ≈ 237 ns (F21) |
| `src/disc_image.c` | ✅ Correct | ISO/BIN/NRG/MDF parsers; CUE PREGAP+INDEX00 fix; NRG lead-out skip fix; NRG chunk_size=0/DAOX-too-short guards; MDF sector_size=0 guard; CUE/MDF track-length underflow clamp; NRG uint64_t aligned memcpy; NRG v1 unaligned reads use memcpy; disc_find_track includes pregap LBAs; NRG v1 file_off uses idx1_lba (F11); disc_build_toc_response emits 4-byte entries [track,min,sec,frame] (F15); file_offset widened to uint64_t (F29+F30) |
| `src/sector_cache.c` | ✅ Correct | SD prefetch ring buffer + flush_gen race fix; hard_assert null-cache guard in prefetch_tick; __atomic_acquire/release builtins; SD read error retries once before skipping; sector_cache_is_full() added; error slots marked valid with valid_bytes=0 (permanent miss sentinel) |
| `src/subcode.c` | ✅ Correct | Q-channel generation; dead ORIG function comment removed |
| `pio/subcode_encoder.pio` | ✅ Correct | SUB signal output on GPIO 5-8; .side_set 1 drives SUB_CLK (FINDING-20); WFCLK/SCOR pulsed by subcode_pulse_sector_clocks() |
| `src/sd_card.c` | ✅ Correct | SDIO mount/scan; sd_scan_images offset param; sd_count_images for total count; qsort alphabetical order for stable indices (F24) |
| `src/hw_config.c` | ✅ Correct | FatFS hardware config; VolToPart[] maps "0:/"→partition 1, "1:/"→partition 2 |
| `src/virtual_disc.c` | ✅ Correct | ISO 9660 synthesis from SD partition 2; BFS scan → LBA assignment → per-sector dispatch (PVD/VDST/path tables/dir records/file data) |
| `include/virtual_disc.h` | ✅ Correct | vdisc_t / vdisc_entry_t types; vdisc_mount / vdisc_read_sector API |
| `include/ffconf.h` | ✅ Correct | Project-owned FatFS config; FF_MULTI_PARTITION=1 enables two-partition support; shadows vendor copy in libs/ |
| `src/ecc.c` | ✅ Correct | EDC/ECC computation; P-parity fixed to 24 rows; Q-parity uses correct ECMA-130 diagonal interleave (stride 44 mod 2236) |
| `src/logger.c` | ✅ Correct | SD activity log; WiFi fields (ssid/password/hostname) added to logger_config_t; log_max_kb negative guard; dead cmd_name/_log_cmd/_log_cmd_resp/_log_irq removed; state_names[] offset corrected (RESET removed, bounds ≤7) (F7+F18) |
| `src/config.c` | ✅ Correct | Flash-backed settings; multicore_lockout_start/end_blocking() wraps flash erase/program (BUG-1) |
| `src/display.cpp` | ✅ Correct | ST7789 240×240 cover art + scanline API |
| `src/ui.c` | ✅ Correct | UI event handler |
| `src/webserver.c` | ✅ Correct | WiFi web interface; use-after-free fixed; HCAT overflow guard; html_escape + json_escape; static conn pool; tcp_recved before tcp_close (BUG-3); JPEG ERR_MEM retry; cover path cache (≤3 f_stat/render); pagination (page offset/count/total, prev/next buttons, /api/page/*); cover_exists base[] widened to MAX_PATH_LEN (F22) |
| `src/da_output.c` | ✅ Correct | DA DMA engine — 24-bit I2S expand, DRQ flag, FIFO drain on stop/pause, M17SINE clkdiv trim; hard_assert on DMA ch claims; s_drq_pending __atomic_*; s_audio_mode __atomic_*; dma_channel_abort in end-of-disc ISR (BUG-2); resume_lba from min(buf_lba) (SMELL-5); NULL s_cache guard in IRQ (F23); subcode PIO clkdiv updated on 2× change (F14); da_get_resume_lba() added (F12); spin-wait after seek in da_resume() (F9) |
| `src/rotary_mcp.cpp` | ✅ Correct | MCP23017 encoder + logger toggle button (GPB0) |
| `src/commo_bridge.c` | ✅ Correct | PLAY_TRACK_OPC BCD decode, TRAY_IN seek, audio mode set, DRQ packet; _send_toc_packets guards first_track==0; PIN_RESET init+poll; send_status/qchannel pkt stack-allocated; _wait_commo_ready 5 ms timeout; FAKE_TIMING enabled + 1.8s spinup (F2); TOC 0xA1 uses ctrl_first (F5); _wait_commo_ready removed from _send_toc_packets (F10); da_get_resume_lba in PAUSE_OFF (F12); Dispatcher/cmd_hndl called only when Path A idle (F13) |
| `src/upstream_player_shim.c` | ✅ Correct | Linkage shim — provides player_interface globals and no-op player() for upstream cmd_hndl.c |
| `src/fft.c` | ✅ Correct | 256-point Q15 radix-2 FFT with Hann window |
| `src/effects.c` | ✅ Correct | 5 demoscene visualiser effects (SPECTRUM/SCOPE/RASTER/COMBO/SPACEBALLS) |
| `src/vis_audio.c` | ✅ Correct | DA DMA snoop — SPSC ring, left-channel extraction |
| `pio/da_output.pio` | ✅ Correct | 24-bit I2S frames, clkdiv=32 (2.12 MHz), 96 SM cycles/pair → 44.1 kHz; LRCLK polarity corrected (low=L, high=R) (F3) |
| `include/subcode.h` | ✅ Correct | subcode_push_to_pio() clears FIFO on partial push to prevent orphaned words (F8) |
| `include/disc_image.h` | ✅ Correct | track_t.file_offset widened to uint64_t for >4 GB images (F29+F30) |
| `tests/host/test_csv_replay_pon_poff.cpp` | ✅ Correct | CsvReplayPonPoff: 10 windowed tests against pon-poff-idle.csv |
| `tests/host/test_csv_replay_zool2.cpp` | ✅ Correct | CsvReplayZool2: 10 windowed tests against zool2.csv |
| `tests/host/test_csv_replay_pinball.cpp` | ✅ Correct | CsvReplayPinball: 10 windowed tests against pinball.csv |
| `tests/host/test_door_tray.cpp` | ✅ Correct | DoorTray: 10 tests — insert/eject state, ACTIVE pin/LED, status bits; DoorPin: 8 tests — GPIO edge detection, boot snapshot, motor/LED state; HostReset: 7 tests — /RESET contract (GPIO 14), drive state cleared, door state preserved |
| `tests/host/test_opc_responses.cpp` | ✅ Correct | OpcResponses: 34-case table test — every COMMO opcode → status + state (incl. SEEK from PLAYING, SEEK invalid BCD) |
| `tests/host/test_motor_sled_fake.cpp` | ✅ Correct | MotorSledFake: 10 tests — motor active states, BCD→LBA, virtual sled seek |
| `tests/host/test_webserver_html.cpp` | ✅ Correct | WebserverHtml: 10 tests — compiles real build_html_page() via WEBSERVER_TEST_BUILD; verifies buffer, title, starfield, SD space, CSS escaping, disc grid, drive state label |

---

## Vendor libraries (`libs/`)

**Do not modify any file under `libs/`.** All subdirectories are third-party libraries
tracked as git submodules or vendored snapshots. Project-specific configuration that
would otherwise require editing a vendor file (e.g. FatFS `ffconf.h`) must instead be
provided by a shadowing copy in `include/`, which appears earlier on every include path.

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
**Result:** 449 tests, 0 failures  
**Parser tests:** `make parser_tests && ./parser_tests -v` → 28 tests, 0 failures (separate binary; uses FatFS injectable sim)
**Virtual disc tests:** `make vdisc_tests && ./vdisc_tests -v` → 53 tests, 0 failures (separate binary; uses vdisc_sim with directory traversal support)
**Stress tests:** `make stress_sector_cache && ./stress_sector_cache` → 5 tests, 0 failures (TSan binary; concurrent producer/consumer)
**Sanitizer:** `-fsanitize=undefined -fno-sanitize-recover=all` active on all C and C++ objects and the link step

| Group | Tests | What it covers |
|-------|-------|----------------|
| `LbaMsf` | 11 | LBA↔MSF conversion, BCD encoding, round-trips |
| `Ecc` | 13 | EDC round-trip, single-bit flip detection, MSF header flip, last-data-byte flip, sync pattern inside payload passes EDC |
| `Subcode` | 14 | CRC-16, CONAD byte, BCD track, absolute MSF |
| `SubcodePio` | 3 | subcode_push_to_pio() PIO TX FIFO back-pressure: all 3 words pushed (never-full), first word rejected (full before push), 3rd word blocked (full after 2) |
| `SubcodeClk` | 6 | subcode_pulse_sector_clocks(): SCOR+WFCLK idle low after pulse, both return to low from HIGH, DATA/CLK pins unaffected, pin numbers distinct, pulse+push integration |
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
| `HostReset` | 7 | /RESET pin (GPIO 14) contract: PLAYING/SEEKING/SPINUP → IDLE, ThenTrayIn restarts spinup, multiple resets idempotent, door GPIO snapshot preserved across reset, DRIVE_ERROR fault cleared |
| `WebserverHtml` | 10 | Compiles and runs the real build_html_page() via WEBSERVER_TEST_BUILD hook; checks: fits 32 KB, DOCTYPE present, Nebula32 title, starfield script, SD card status label, GB space output, loadDisc JS, CSS %% escaping, disc names in grid, Drive State label |
| `SectorCadence` | 5 | DA delivery timing budget: zero misses when on-time, miss counted per late sector, all-late saturates, exact-at-budget passes, one-over-budget fails |
| `RingBuffer` | 5 | Generic SPSC ring: push to capacity succeeds, push when full fails, pop in order, pop when empty underflows, wraparound preserves order |
| `SdStallSim` | 4 | Stochastic SD stall injection: zero stalls below threshold, seed-42 count in expected range, deterministic replay same-seed, count scales with sector count |
| `QSubchannel` | 5 | Q-channel field encoding: relative MSF 75 sectors in, absolute MSF at LBA 225, pregap relative countdown, index byte 0x01 in programme area, index byte 0x00 in pregap |
| `CommandFuzz` | 5 | Opcode classification exhaustive sweep: all 256 opcodes no UB, status/control/query sets classified correctly, null and 0xFF return unknown |
| `DmaDoubleBuffer` | 4 | Ping-pong DMA buffer isolation: fill patterns isolated between A/B, swap gives correct active buffer, standby write does not corrupt active, after swap new standby is overwritable |

**vdisc_tests groups (separate binary — `make vdisc_tests`):**

| Group | Tests | What it covers |
|-------|-------|----------------|
| `VdiscMount` | 7 | Empty dir, single file, nested subdirs, max-entry limit, max-depth skip, filename uppercasing, illegal-char replacement |
| `VdiscLbaLayout` | 8 | System area zeros, PVD at LBA 16, VDST at LBA 17, path tables at 18/19, root dir at LBA 20, file LBAs after all dir LBAs, no overlapping LBA ranges |
| `VdiscPvd` | 7 | Magic "CD001", type byte 0x01, version 0x01, logical block size 2048 LE+BE, volume space size matches total_sectors, root dir record LBA=20, volume identifier "NEBULA32" |
| `VdiscVdst` | 2 | Type byte 0xFF, magic "CD001" |
| `VdiscPathTable` | 5 | L-table root LBA=20 LE, M-table root LBA=20 BE, root identifier byte 0x00, parent number=1, subdir entry present |
| `VdiscDirSector` | 8 | Self-ref first (id=\x00), parent-ref second (id=\x01), file flags=0x00, dir flags=0x02, file version suffix ";1", dir no version suffix, file size field matches, all record lengths even |
| `VdiscFileData` | 6 | First sector content matches SD data, second sector of large file, last sector zero-padded, system area zeros, LBA beyond total → zeros, nested file path reconstruction |
| `VdiscToc` | 5 | One data track, TRACK_TYPE_DATA, start LBA=0, lead-out LBA matches total_sectors, TOC response encodes correctly |
| `VdiscIntegration` | 5 | disc_open_vdir sets DISC_FORMAT_VDIR, LBA 16 read via disc_read_sector returns PVD magic in payload, LBA 0 has Mode 1 sync pattern, file data round-trip through disc_read_sector, disc_close clears vdisc pointer |

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

_All items complete (including first-impressions review fixes 1-28, 2026-05-17)._
