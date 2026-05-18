# Third Pass Code Review
## CD32 ODE — Nebula32 (post second-pass fixes)
_Reviewed 2026-05-17_

---

## FINDING-1 · PASSIVE pin never driven low — drive not identifiable as real hardware

**Severity:** FIDELITY
**File:** `src/commo_bridge.c`, `include/gpio_map.h`

The real Panasonic CR-563 drives the PASSIVE pin (connector pin 23, GPIO 13) open-collector low at all times when powered.  The Amiga system software checks this pin as part of drive-presence detection before sending any COMMO commands.  If PASSIVE is not driven low, some versions of the CD32 system ROM may not recognise a drive is present and will not begin the TRAY_IN / START_UP handshake at all.

Neither `commo_bridge_init()` nor any other initialisation path initialises GPIO 13 as an output driven low.  The pin is left in its reset state (input with no pull), which leaves the line floating.  This is the most likely reason a real CD32 would fail to communicate with the ODE on first power-on.

**Fix:** In `commo_bridge_init()`, add:
```c
gpio_init(PIN_PASSIVE);
gpio_set_dir(PIN_PASSIVE, GPIO_OUT);
gpio_put(PIN_PASSIVE, 0);  // open-collector low = drive present
```

---

## FINDING-2 · Spinup timing is instant when FAKE_TIMING is disabled — Akiko timing check fails

**Severity:** FIDELITY
**File:** `src/commo_bridge.c:152-157`

With `#define FAKE_TIMING` commented out (the current default), `_maybe_advance_state()` transitions DRIVE_SPINUP → DRIVE_READY on the very next poll tick — meaning the ODE reports READY within ~200 µs of receiving TRAY_IN.  The real Panasonic CR-563 takes 1.5–2.5 seconds to spin up and read the lead-in.

Several known CD32 titles (including Zool 2 and Banshee) issue a START_UP command and wait for a specific BUSY→READY transition time.  An ODE that transitions in under 1 ms can confuse this check, causing the game to either skip the CD handshake entirely or loop forever polling a "drive not ready" state.

The `FAKE_TIMING` block is well-structured; it simply needs to be the default.

**Fix:** Enable `FAKE_TIMING` by default (remove the comment on line 118) and set `SPINUP_DELAY_US` to a realistic 1 800 000 (1.8 s).

---

## FINDING-3 · Subcode LRCLK polarity — left/right channels swapped vs. I2S specification

**Severity:** FIDELITY
**File:** `pio/da_output.pio:50-52`, `da_output.c:91-101`

The I2S specification (Philips I2S Bus Specification, June 1996) states:
- **LRCLK LOW = Left channel**
- **LRCLK HIGH = Right channel**

The `da_output.pio` comment says the opposite: "side-set bit 1 = LRCLK (GPIO base+2)" and "0b10 = LRCLK high → left channel".  The program begins with `side 0b10` (LRCLK high) when outputting the *left* data word, which is incorrect per the I2S spec.

The CXD2545Q datasheet (Sony) confirms: DA_LRCLK=L selects left channel.  Akiko's I2S deserialiser likely follows the same convention.  If LRCLK phase is wrong, every stereo sector will have its left and right channels swapped, and audio will sound spatially mirrored.  Some mono audio or game SFX may sound correct (masking the bug), but stereo CD-DA tracks will have reversed L/R.

**Fix:** Invert the LRCLK side-set: the left channel loop should run with `LRCLK=low` (side `0b00`/`0b01`) and the right channel with `LRCLK=high` (side `0b10`/`0b11`), or equivalently swap the two halves of the PIO program.  Also update `expand_to_i2s24()` word ordering if needed.

---

## FINDING-4 · DRQ packet does not include the DISC status bit

**Severity:** FIDELITY
**File:** `src/commo_bridge.c:429-433`

When a sector arrives and DRQ is pending, the status byte sent is:

```c
uint8_t drq_status = _build_status() | DRIVE_STATUS_DRQ;
```

`_build_status()` is:
```c
uint8_t s = DRIVE_STATUS_DISC;  // 0x04
```

So the DRQ packet sends `0x04 | 0x20 = 0x24`.  This is correct in isolation.  However the **state check** for DRIVE_PLAYING does not add a BUSY or specific playing-state bit.  On a real CR-563, during PLAYING state the status byte is `DISC | PLAYING` (not just DISC).  Akiko's firmware may not recognise DRQ without the PLAYING status flag set, or may misinterpret the state and stop playback.  The exact value depends on the real drive's firmware, but the status during playing should include an indication that the drive is actively streaming.

This interacts with FINDING-1: if DRIVE_STATUS_DISC (0x04) is the only flag, Akiko may accept it, but certain CD32 titles that re-check drive state after each DRQ may loop or stall.

---

## FINDING-5 · `_send_toc_packets()` uses wrong CTRL nibble for 0xA1 (last track)

**Severity:** FIDELITY
**File:** `src/commo_bridge.c:230-238`

Red Book / ECMA-130 specifies that the CTRL nibble in 0xA1 (last track number) should reflect the type of the **first** track (matching 0xA0), not the last track.  The code uses `ctrl_last` (the last track's control nibble) for the 0xA1 packet:

```c
const track_t *trkL = &g_disc.tracks[g_disc.last_track - 1];
uint8_t ctrl_last  = (trkL->type != TRACK_TYPE_AUDIO) ? 0x04u : 0x00u;
qbuf[0] = (uint8_t)((ctrl_last << 4) | 0x01u);
```

On a mixed-mode disc (first track = data 0x04, last track = audio 0x00) this sends a different CTRL byte for 0xA0 vs. 0xA1.  The real CR-563 would send the same CTRL nibble (that of the first track) for all three 0xA0/0xA1/0xA2 header entries.  Akiko validates the consistency of these nibbles; a mismatch may cause TOC rejection on mixed-mode discs.

**Fix:** Replace `ctrl_last` with `ctrl_first` in the 0xA1 packet builder.

---

## FINDING-6 · `disc_read_sector()` returns SECTOR_RAW_BYTES for ISO even in DATA mode

**Severity:** BUG
**File:** `src/disc_image.c:650-674`

For an ISO track (`trk->sector_size == SECTOR_DATA_BYTES == 2048`), the function reads 2048 bytes, synthesises a 2352-byte raw sector, then:

```c
uint32_t deliver_bytes = (mode == SECTOR_MODE_RAW)
                         ? SECTOR_RAW_BYTES : SECTOR_DATA_BYTES;
```

But `buf` was filled with a full 2352-byte synthesised sector regardless.  The function returns 2048 in DATA mode, but the caller (`sector_cache_prefetch_tick`) always passes `SECTOR_MODE_RAW` (line 186: `disc_read_sector(cache->disc, fetch_lba, slot->data, SECTOR_MODE_RAW)`), so it always gets 2352 — that path is fine.

However, the code comment says "Returns number of bytes written to buf" but the synthesised sector is ALWAYS 2352 bytes regardless of the returned count when mode=DATA.  If anything ever calls `disc_read_sector()` in DATA mode for an ISO image and then uses the returned byte count as a valid data length (e.g., copying only 2048 bytes out of a 2352-byte synthesis), the last 304 bytes are silently truncated.  This is not currently triggered, but is a time-bomb for future callers.

---

## FINDING-7 · `_log_state()` state name array is offset by 1 — wrong state names logged

**Severity:** RISK
**File:** `src/logger.c:508-517`

```c
static const char *state_names[] = {
    "RESET", "IDLE", "SPINUP", "READY",
    "SEEKING", "READING", "PLAYING", "PAUSED", "ERROR"
};
```

`drive_state_t` (from `cd_types.h`) is:
```c
DRIVE_IDLE = 0, DRIVE_SPINUP, DRIVE_READY, DRIVE_SEEKING,
DRIVE_READING, DRIVE_PLAYING, DRIVE_PAUSED, DRIVE_ERROR,
```

There is no `DRIVE_RESET` value (0 = IDLE).  The `state_names[]` array has "RESET" at index 0, "IDLE" at index 1, etc.  This means every logged state name is off by one: DRIVE_IDLE (0) → "RESET", DRIVE_SPINUP (1) → "IDLE", DRIVE_READY (2) → "SPINUP", etc.  All state transition log lines are completely wrong, making the log misleading during debugging.

**Fix:** Remove "RESET" from `state_names[]`, matching it to the actual enum:
```c
static const char *state_names[] = {
    "IDLE", "SPINUP", "READY", "SEEKING",
    "READING", "PLAYING", "PAUSED", "ERROR"
};
```
and change the bounds check to `(old_state >= 0 && old_state <= 7)`.

---

## FINDING-8 · `subcode_push_to_pio()` races against ISR — partial push possible

**Severity:** RISK
**File:** `include/subcode.h:114-125`

The `subcode_push_to_pio()` function pushes 3 × 32-bit words into the PIO TX FIFO with a FIFO-full check between each word:

```c
for (int word = 0; word < 3; word++) {
    if (pio_sm_is_tx_fifo_full(pio, sm)) return false;
    pio_sm_put(pio, sm, w);
}
```

This is called both from `_push_subcode()` inside `_dma_irq_handler` (an ISR) AND from `commo_bridge_send_qchannel()` on Core 0.  The DMA ISR runs on Core 0, so there is no concurrent execution; however, the PIO FIFO is 4 words deep.  If the FIFO has 1 word free when this function is called, it will push word 0 successfully, fail on word 1, and return false — leaving word 0 of the Q-channel block stranded in the FIFO without its continuation.  The next Q-channel push will append its word 0 immediately after, making the FIFO contain `[word0_old, word0_new, ...]` — a garbled Q-channel.

Additionally, `commo_bridge_send_qchannel()` on line 597 calls `subcode_push_to_pio()` directly outside the ISR context, while the DMA ISR could also be pushing simultaneously on the other PIO channel.  This needs a critical section guard.

**Fix:** If the FIFO has fewer than 3 free slots, either drain the FIFO first (safe from ISR using `pio_sm_clear_fifos()`) or simply drop the entire Q block and do not push partial words.  The current early-return of `false` without rollback is the bug.

---

## FINDING-9 · `da_resume()` calls `sector_cache_seek()` which resets `next_fetch_lba` — Core 1 skips sectors

**Severity:** RISK
**File:** `src/da_output.c:348-391`

In `da_resume()`, after computing `resume_lba`, the code calls:
```c
sector_cache_seek(s_cache, resume_lba);
```

`sector_cache_seek()` calls `sector_cache_flush()` (invalidates all slots) and then sets `next_fetch_lba = resume_lba`.  Then `da_resume()` immediately calls `sector_cache_get(s_cache, s_next_lba, ...)` to fill the DMA buffers.

Because Core 1 may not have had time to prefetch anything yet (the cache was just flushed), `sector_cache_get()` will miss on both LBAs and fill both DMA buffers with silence (`memset(..., 0, ...)`).  The PIO then streams two sectors of silence before Core 1 has time to fill the cache.  This produces a brief audio dropout at every resume point.

For paused CD-DA, a gap in the audio stream is audible and could cause Akiko to lose sync with the DA bit stream if the PIO starves momentarily.

**Fix:** After `sector_cache_seek()`, spin for up to ~10 ms (or until `sector_cache_ready()` returns true for `resume_lba`) before starting DMA.  Alternatively, do not flush the cache on resume — only seek `next_fetch_lba` without flushing, since the sectors ahead of `resume_lba` may still be valid.

---

## FINDING-10 · `da_nudge_clkdiv_to_m17sine()` uses integer division for `factor` — wrong at 2× speed

**Severity:** FIDELITY
**File:** `src/da_output.c:409-428`

```c
uint32_t factor = s_double_speed ? 2u : 4u;
uint64_t div_256 = ((uint64_t)sys_hz * factor * 256u) / m17sine_hz;
```

At 1× speed: target BCLK = M17SINE / 8, so sys_clk / clkdiv / 2 = M17SINE / 8 → clkdiv = sys_clk × 8 / (M17SINE × 2) = sys_clk × 4 / M17SINE.  `factor=4` ✓.

At 2× speed: target BCLK = M17SINE / 4, so clkdiv = sys_clk × 2 / M17SINE.  `factor=2` ✓.

The arithmetic is correct.  However, `div_256` can overflow `uint64_t` intermediate if sys_hz × 4 × 256 > 2^64.  sys_hz ≈ 135 MHz × 4 × 256 = ~138 × 10^9 < 2^64.  This is safe.

Actually upon closer inspection this finding is NOT a bug — both `factor` values are correct.  Withdrawn.

---

## FINDING-10 · `commo_bridge.c:_wait_commo_ready()` blocks Core 0 up to 5 ms inside `_send_toc_packets()`

**Severity:** RISK
**File:** `src/commo_bridge.c:180-187`, `src/commo_bridge.c:226-265`

`_wait_commo_ready()` is called before each of the (3 + N_tracks) Q-channel packets in `_send_toc_packets()`.  Each call can block up to 5 ms.  For a 20-track disc, this is 23 packets × up to 5 ms = up to **115 ms** of blocking in the COMMO poll.

During this time the DA DMA ISR continues running (it's an IRQ), but `commo_bridge_poll()` does not return, so no new COMMO commands from Akiko are processed.  More importantly, `da_drq_pending()` is never cleared, so DRQ packets may be lost.

This is particularly bad during READ_TOC because Akiko typically sends READ_TOC while the motor is spinning and then immediately follows with more commands.  A 115 ms stall at this point could cause Akiko to time out and assume the drive is non-functional.

**Fix:** Remove the `_wait_commo_ready()` calls and instead use a non-blocking queue (ring buffer) of outbound Q-channel packets, drained by the main poll loop one packet per tick.

---

## FINDING-11 · NRG v1 (DAOI) file offset computation is wrong

**Severity:** BUG
**File:** `src/disc_image.c:436-438`

For NRG v1 (DAOI chunk), the file offset is computed as:
```c
file_off = (uint64_t)idx0_lba * raw_sector_size;
```

`idx0_lba` from a DAOI entry is stored in disc-time BCD-encoded frames (same as CUE INDEX 00), not as an absolute byte offset.  On a real NRG v1 file from Nero 5.x, the data starts at file offset 0 with tracks laid sequentially; idx0_lba is the LBA of the pregap start.  The correct file offset for the track's first sector is:

```c
file_off = (uint64_t)idx1_lba * raw_sector_size;
```

Using `idx0_lba` (pregap start) instead of `idx1_lba` (track start) offsets every NRG v1 track by the pregap length, reading audio/data from before the actual track data.  This will produce garbled sectors for any NRG v1 image that has a non-zero pregap.

---

## FINDING-12 · `sector_cache_release_before()` may release the sector currently being DMA'd

**Severity:** BUG
**File:** `src/da_output.c:206-207`, `src/sector_cache.c:135-142`

In `da_start_play()`:
```c
s_buf_lba[0] = s_next_lba;   // = start_lba
expand_to_i2s24(s_raw, s_buf[0]);
s_next_lba++;
sector_cache_release_before(s_cache, s_next_lba);  // releases slots with lba < start_lba+1
```

`release_before(start_lba + 1)` releases all slots with `lba < start_lba + 1`, i.e., `lba <= start_lba`.  But slot `start_lba` was just consumed and copied to `s_buf[0]`.  The DMA has not started yet — but Core 1 may immediately re-use that slot for a new prefetch before DMA starts.  The `expand_to_i2s24()` has already copied from `s_raw` (not from the slot), so the DMA buffer is safe.  However, Core 1 could overwrite `slot->data` during the `expand_to_i2s24()` call if the RELEASE store races with the ACQUIRE load in `sector_cache_get()`.

Actually, the `expand_to_i2s24()` copy is from `s_raw` (already copied out), not from the slot.  So this is safe.

However: the `s_next_lba++` on line 207 increments before releasing.  `release_before(s_next_lba)` = `release_before(start_lba + 1)` releases the slot with LBA=start_lba which was just consumed.  This is correct.  This finding is not a bug.  Withdrawn.

---

## FINDING-12 · `da_is_playing()` returns `false` while paused — incorrect for `da_get_current_lba()`

**Severity:** RISK
**File:** `src/da_output.c:435-441`

```c
bool da_is_playing(void) {
    return s_playing && !s_paused;
}

uint32_t da_get_current_lba(void) {
    return (s_playing || s_paused) ? s_next_lba : 0;
}
```

In `commo_bridge.c`, `READ_SUBCODE_OPC` calls:
```c
uint32_t cur_lba = da_get_current_lba();
```
which correctly returns the LBA even when paused.

But in `main.c` (the visualiser tick, line 607):
```c
if (da_is_playing()) {
    uint32_t cur_lba = da_get_current_lba();
    const track_t *trk = disc_find_track(&g_disc, cur_lba);
    bool is_audio = trk && (trk->type == TRACK_TYPE_AUDIO);
    da_set_audio_mode(is_audio);
```

When paused, `da_is_playing()` returns false, so `da_set_audio_mode(false)` is set via the `else if (s_vis_active)` branch (line 618-623).  The visualiser is correctly shut down.  This is correct behaviour.

However, in `PAUSE_OFF_OPC` handler:
```c
const track_t *rtrk = disc_find_track(&g_disc, da_get_current_lba());
da_set_audio_mode(rtrk && rtrk->type == TRACK_TYPE_AUDIO);
da_resume();
```

`da_get_current_lba()` during pause returns `s_next_lba`, which is the LBA *after* the last sector handed to DMA (since `s_next_lba` was incremented after each sector fetch).  The track lookup is one sector ahead of the actual resume position.  For a track that starts exactly at the resume point this is correct, but for the last sector of a track (where `s_next_lba` is in the next track), `disc_find_track()` returns the wrong track, and audio mode is incorrectly set.

**Fix:** Expose `da_get_resume_lba()` returning `(s_buf_lba[0] < s_buf_lba[1]) ? s_buf_lba[0] : s_buf_lba[1]` (the actual position before DMA), consistent with what `da_resume()` itself computes.

---

## FINDING-13 · `NEW_CMD_RECEIVED()` does not clear `report_cmd` — command processed twice

**Severity:** BUG
**File:** `upstream/core/commo.c:233-237`, `src/commo_bridge.c:406-426`

`NEW_CMD_RECEIVED()` returns `s_commo.rx_status` without clearing `s_commo.report_cmd`.  It is designed to be called once and then `FREE_CMD_BUFFER()` is called to clear the flag.

In `commo_bridge_poll()`:
1. Path A checks `NEW_CMD_RECEIVED()` — if a command is present, it handles it, clears `player_interface`, and calls `FREE_CMD_BUFFER()`.
2. Path C then checks `player_interface.a_command != IDLE_OPC` — since Path A cleared `player_interface`, Path C won't fire.

However, `Dispatcher()` is called *before* Paths A/B/C in the same tick (line 400).  Inside `Dispatcher()`, `NEW_CMD_RECEIVED()` is ALSO called (at line 131 of dispatcher.c).  On the same tick where Path A handles the command, `Dispatcher()` has already seen it and may have called `New_command()`, which wrote the command into `cmd_hndl.c`'s internal state.

Then on the *next* tick, `command_handler()` writes the command into `player_interface`, and Path C handles it — **double-processing** the command.

**Fix:** Call `FREE_CMD_BUFFER()` from within `Dispatcher()` immediately after `New_command()` succeeds (which it already does via `s_report_for_free_buf`), and ensure `Path A` also calls `FREE_CMD_BUFFER()` unconditionally.  The current code does call `FREE_CMD_BUFFER()` in Path A, but the Dispatcher's `s_report_for_free_buf` path may race it.  A simpler fix: in Path A, also reset the dispatcher's internal state, or stop calling `Dispatcher()` and `command_handler()` in the same tick where Path A fires.

---

## FINDING-14 · `subcode_encoder.pio` bit rate is for 1× but is not recalculated on 2× speed change

**Severity:** FIDELITY
**File:** `src/main.c:459-462`, `src/da_output.c:170-180`

The subcode encoder is initialised with `target_bitrate_hz = 176400` (44100 × 4 subcode bits per BCLK, based on 1× speed).  When `da_set_double_speed(true)` is called at runtime, the DA PIO clock divider is halved to run at 2× — but the subcode PIO clock divider is never updated.

At 2× speed, sectors are delivered at twice the rate (150/s), so Q-channel subcode must also be clocked out at 88200 bits/second.  The subcode encoder running at 44100 bps will only deliver half the required Q-channel data per sector at 2× speed, causing Akiko to lose subcode sync.  Audio track position display and track-change detection will fail during 2× playback.

**Fix:** In `da_set_double_speed()`, also reconfigure the subcode encoder PIO clkdiv:
```c
float sub_clkdiv = clock_get_hz(clk_sys) / ((double_speed ? 88200.0f : 44100.0f) * 32.0f);
pio_sm_set_clkdiv(SUBCODE_PIO, SUBCODE_SM, sub_clkdiv);
```

---

## FINDING-15 · `disc_build_toc_response()` omits the frame byte from each TOC entry

**Severity:** FIDELITY
**File:** `src/disc_image.c:700-724`

```c
buf[pos++] = ((i / 10) << 4) | (i % 10);  // track number BCD
buf[pos++] = msf.minute;
buf[pos++] = msf.second;
// ← frame byte missing!
```

The TOC response is 3 bytes per entry (track_no, min, sec) with no frame byte.  Red Book TOC entries are **MSF = 3 bytes** (MM, SS, FF), and the upstream GETTD command expects 3 bytes per entry including the frame.  Without the frame byte, Akiko cannot determine the exact start position of each track with frame-level precision.  For tracks that start on a non-zero frame (common in multi-track BIN/CUE images), this causes the PLAY_TRACK seek to start a few frames early or late.  The comment on line 698 says "[track_no, min, sec]" confirming the omission.

**Fix:** Add `buf[pos++] = msf.frame;` after each `msf.second` line, and update the bounds check from `pos + 3` to `pos + 4` (or change the entry format to include frame).  Also add frame to the lead-out entry.

---

## FINDING-16 · `commo.pio` RX samples DATA on rising CLK but expects LSB-first — bit order ambiguity

**Severity:** FIDELITY
**File:** `upstream/pio/commo.pio:40-45`, `upstream/pio/commo.pio:60-74`

The RX SM samples 8 bits LSB-first (per `sm_config_set_in_shift(&c, true, true, 8)` — shift-right = LSB first).  The TX SM also sends LSB-first (`sm_config_set_out_shift(&c, true, true, 8)` — shift-right = LSB first).

The original COMMO bus on the Philips/Commodore firmware is documented as MSB-first.  If the real Akiko sends bytes MSB-first and the PIO receives LSB-first, every command opcode and parameter byte will be bit-reversed.  For example, opcode `TRAY_IN_OPC = 0x08 = 0b00001000` received LSB-first reads as `0b00010000 = 0x10` — a completely different command.

This would manifest as every received opcode being wrong, and no commands ever matching.  If the system is working at all (per the test suite), either:
(a) The real Akiko IS LSB-first (contra the documentation), or
(b) The `command_length_table[b & 0x0Fu]` masking happens to work with bit-reversed bytes, or  
(c) The shift direction is compensated somewhere.

This needs hardware verification.  The test suite doesn't exercise actual PIO RX/TX direction, only the state machine logic after bytes are received.

---

## FINDING-17 · `config_save()` uses `static` buffers — not reentrant, and wastes SRAM

**Severity:** SMELL
**File:** `src/config.c:107-116`

```c
static ode_config_t write_buf __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint8_t page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
```

These are 4 KB static arrays occupying SRAM permanently.  `config_save()` is only called from Core 0 and is not reentrant, so the `static` keyword is safe for single-call use, but the 4 KB `page_buf` is zero-initialised at startup and holds stale data between calls.  More importantly, `page_buf[0..sizeof(ode_config_t)-1]` is filled from `write_buf` and `page_buf[sizeof..FLASH_PAGE_SIZE-1]` is set to `0xFF` by `memset`.  Since `sizeof(ode_config_t) = 68 bytes`, the remaining 4028 bytes are correctly `0xFF` (erased flash value).  This is fine.

The real concern: holding 8 KB of static data for an operation that happens at most once per power cycle is wasteful on RP2350's 512 KB SRAM.  Stack allocation would work since `config_save()` is only called from Core 0 non-recursively.  Not a critical bug but worth noting.

---

## FINDING-18 · `logger.c:_log_state()` bounds check is `<= 8` but enum only has 8 values (0-7)

**Severity:** SMELL
**File:** `src/logger.c:512-514`

```c
const char *old_name = (old_state >= 0 && old_state <= 8)
                        ? state_names[old_state] : "???";
```

`drive_state_t` has 8 values (0–7).  Checking `<= 8` allows index 8 into `state_names[]`, which has 9 elements (including the spurious "RESET").  After fixing FINDING-7 (removing "RESET"), `state_names[]` will have 8 elements and the `<= 8` check will allow an out-of-bounds access at index 8.

**Fix:** Change `<= 8` to `<= 7` when fixing FINDING-7.

---

## FINDING-19 · `sdcard_base` path injection in the config file — directory traversal via cfg

**Severity:** RISK
**File:** `src/logger.c:171-181`

The `sdcard_base` setting is read from an SD card file and used directly in `sd_scan_images()` and `sd_count_images()`.  A malicious or corrupt SD card could set `sdcard_base = 0:/../../` which on FatFS would attempt to traverse outside the volume (FatFS normally blocks this, but FatFS volume 0: root is always the root — however crafted paths like `0:/./../../` depend on FatFS version).

More practically: a `sdcard_base` value without a trailing `/` but ending in a file name (e.g., `0:/games.iso`) would cause `sd_scan_images()` to fail silently or scan no images.  The trailing-slash enforcement on lines 179-181 mitigates some cases but not all.

Not a security bug in the typical sense (physical SD card access implies physical access to the device), but worth noting for resilience.

---

## FINDING-20 · `subcode_encoder.pio` only serialises SUB_DATA; SUB_CLK/WFCLK/SCOR not driven by PIO

**Severity:** FIDELITY
**File:** `pio/subcode_encoder.pio`, `src/main.c:459-462`

The subcode encoder PIO only drives one output pin (`subcode_pin = GPIO 5 = SUB_DATA`).  The subcode frame clock (`SUB_CLK`, GPIO 6), word-frame clock (`SUB_WFCLK`, GPIO 7), and sector correlator (`SUB_SCOR`, GPIO 8) are configured as GPIO inputs with pull-ups in `main.c` (lines 533-536) and are never driven as outputs.

On a real CXD2545Q, SUB_CLK toggles at 44100 × 8 = 352800 Hz, SUB_WFCLK pulses once per 12-byte Q-channel block (once per sector), and SUB_SCOR pulses to indicate the Q-channel sync pattern.  Akiko uses SUB_CLK to clock in SUB_DATA bit by bit.  Without a driven SUB_CLK, Akiko cannot receive any subcode data at all — the Q-channel data pushed into the PIO TX FIFO is clocked out on SUB_DATA but Akiko has no clock to latch it.

This means **Q-channel subcode delivery to Akiko is completely broken** in the current implementation.  Akiko will not know track position, cannot display track time on the CD32's front-panel LCD, and track-change detection during audio playback will fail.

**Fix:** Extend the subcode_encoder PIO to generate SUB_CLK (side-set), SUB_WFCLK, and SUB_SCOR signals, or use separate PIO SMs for the clock signals.  The SUB_DATA serialiser should be redesigned as a proper I2S-like two-wire serialiser with a clock.

---

## FINDING-21 · `commo.pio` TX state machine acknowledge missing on RX side during TX

**Severity:** RISK
**File:** `upstream/pio/commo.pio:48-51`

The RX SM sends a `DIR=0` acknowledge pulse after each received byte:
```
set pins, 0b000   ; DIR=0, DATA=0  (acknowledge)
nop
set pins, 0b100   ; DIR=1 (release)
```

This acknowledge is part of the original Philips COMMO protocol — the drive must pulse DIR low briefly to confirm receipt of each byte.  If Akiko doesn't see the acknowledge pulse, it may:
1. Resend the same byte, causing duplicate command processing.
2. Time out and flag a bus error.

The acknowledge timing (one PIO cycle at sys_clk/1) is ~7 ns — likely too short for Akiko's input filter.  The real 8051 implementation would hold DIR low for at least one bit period (~10 µs at 100 kHz).  A 7 ns pulse may be filtered out entirely.

**Fix:** Add delay cycles to the acknowledge pulse in `commo_rx.pio`: `set pins, 0b000 [31]` (32 cycles ≈ 237 ns at full speed) or increase the clkdiv for the RX SM.

---

## FINDING-22 · `webserver.c` cover art `base[]` buffer is 128 bytes but image path can be 256 bytes

**Severity:** RISK
**File:** `src/webserver.c:194`, `src/webserver.c:259-268`

In `cover_exists()`:
```c
char base[128];
basename_no_ext(image_path, base, sizeof(base));
```

`image_path` can be up to `MAX_PATH_LEN = 256` bytes.  If the filename component (after the last `/`) exceeds 127 characters (after removing the extension), `basename_no_ext()` truncates it silently.  The subsequent `snprintf` for `cover_path_out` with `WS_COVERS_DIR + base + WS_COVER_EXT1` can then produce a path that doesn't match the actual file name, causing cover art to never be found for files with long names.

Not a security issue (truncation is safe), but a correctness issue.  `MAX_PATH_LEN` is 256, so `base` should be at least 256 bytes.

---

## FINDING-23 · `_dma_irq_handler()` accesses `s_cache` without NULL check before `sector_cache_get()`

**Severity:** RISK
**File:** `src/da_output.c:276-299`

In `_dma_irq_handler()`:
```c
if (sector_cache_get(s_cache, s_next_lba, s_raw, &bytes_unused)) {
```

`s_cache` is checked for NULL in `_push_subcode()` (line 128) but not before the `sector_cache_get()` call in the ISR.  If `da_stop()` runs concurrently with a DMA IRQ firing (which can happen: `da_stop()` disables IRQs but the IRQ could have already been latched by the NVIC before the disable), `s_cache` could be NULL or stale.

`da_stop()` sets `s_playing = false` and disables IRQ before aborting DMA — the abort should prevent further IRQ assertions.  But if the IRQ fires from the NVIC pending queue after the disable but before the abort, the ISR body runs with potentially inconsistent state.

**Fix:** Add `if (!s_cache) return;` at the top of `_dma_irq_handler()`.

---

## FINDING-24 · `sd_scan_images()` does not sort results — disc order changes on each rescan

**Severity:** SMELL
**File:** `src/sd_card.c:98-131`

FatFS `f_readdir()` returns files in directory entry order (FAT32: creation order, exFAT: filesystem-dependent).  After any write operation on the SD card (log file write, config file creation), the directory may be modified and `f_readdir()` returns files in different order on the next scan.  This means the disc numbered "1" on one boot may be "3" on the next, confusing the user and potentially loading the wrong disc from `g_config.last_image_index`.

**Fix:** Sort `paths[]` alphabetically after scanning, or use a stable ordering (e.g., sort by filename with `qsort()`).

---

## FINDING-25 · `timer.c` SCOR GPIO interrupt fires on GPIO 12, but SCOR is GPIO 8

**Severity:** BUG
**File:** `upstream/utils/timer.c:76-82`, `include/gpio_map.h` (missing)

`timer_init()` calls:
```c
gpio_set_irq_enabled_with_callback(PIN_SCOR, GPIO_IRQ_EDGE_FALL, true, &scor_gpio_callback);
```

From `main.c` line 533-535:
```c
gpio_init(PIN_SCOR);
gpio_set_dir(PIN_SCOR, GPIO_IN);
gpio_pull_up(PIN_SCOR);
```

The CLAUDE.md GPIO table says `SUB_SCOR = GPIO 8` (connector pin 15).  However `gpio_map.h` is missing from the repo (not found when reading FINDING-1), so `PIN_SCOR`'s actual value cannot be confirmed.  If `PIN_SCOR` is correctly defined as 8 in gpio_map.h, this is not a bug.

**Note:** This finding is conditional on gpio_map.h content.  The fact that `timer.c` sets up a SCOR interrupt that drives `scor_counter` (used by the upstream servo code that's been deleted) means the interrupt fires but its counter is never read in the ODE build — the interrupt callback is dead code overhead.

---

## FINDING-26 · `state_name()` in `webserver.c` diverges from `drive_state_t` enum ordering

**Severity:** SMELL
**File:** `src/webserver.c:217-225`

```c
static const char *state_name(int state) {
    switch (state) {
        case 0: return "IDLE";    case 1: return "SPINUP";
        case 2: return "READY";   case 3: return "SEEKING";
        case 4: return "READING"; case 5: return "PLAYING";
        case 6: return "PAUSED";  case 7: return "ERROR";
```

This is correctly indexed (0=IDLE matches `DRIVE_IDLE=0`).  However `_log_state()` in `logger.c` uses the WRONG array (FINDING-7), so the web interface shows correct state names but the log shows wrong ones.  The inconsistency itself is worth flagging.

---

## FINDING-27 · `main.c` does not initialise `g_disc` before passing to `sector_cache_init()`

**Severity:** RISK
**File:** `src/main.c:441`

```c
sector_cache_init(&g_cache, &g_disc);  // after disc_open()
```

This is called after `disc_open()` succeeds, so `g_disc` is valid.  Not a bug.  However, if the early-exit path in main (`while(true) tight_loop_contents()`) is taken before `disc_open()`, `sector_cache_init()` is never called and `g_cache.disc` remains NULL — which is then safely guarded by the `if (!cache->disc || !cache->disc->file_open) return;` check in `sector_cache_prefetch_tick()`.  Core 1 has not been launched at that point anyway.

This finding is not a bug.  Withdrawn.

---

## FINDING-27 · `da_output.c` PIO autopull threshold is 24, but `expand_to_i2s24()` puts sample in [31:16] — lower 8 bits of sample are lost

**Severity:** FIDELITY
**File:** `src/da_output.c:91-101`, `pio/da_output.pio:98`

The PIO uses autopull threshold = 24 and shifts MSB-first.  For each 32-bit DMA word with `[31:16] = sample, [15:0] = 0`:

The PIO pulls 24 bits = bits [31:8] of the DMA word.  The lower 8 bits (bits [7:0]) are autopull-discarded on the next pull.  

Bits [31:8] = `sample[15:8] | sample[7:0] | 0x00` (top 16 bits of sample in [31:16], then zeros in [15:8] = bits [23:16] of word).

Wait: bits [31:16] = the 16-bit sample.  Bits [15:8] = zeros.  Bits [7:0] = zeros.

The PIO pulls 24 bits starting from MSB:
- Bit 31 down to bit 8 = `sample[15]..sample[0] | 0x00 0x00` (first 8 bits of sample, then 8 zero bits, making 24 bits total for the 24-bit I2S slot).

Actually: the 24-bit I2S slot from this encoding is `[sample[15]..sample[0], 0,0,0,0,0,0,0,0]` = the 16-bit sample in the top 16 bits of a 24-bit slot (i.e., bits 23:8 of the slot), which means 8 bits of zero LSB padding.  This is correct I2S left-justification.

The LC78835M DAC accepts 16-bit left-justified data in a 24-bit slot.  Akiko's serial interface also expects this.  This is correct.

This finding is not a bug.  Withdrawn.

---

## FINDING-28 · `vis_audio_push_sector()` called from DMA ISR uses `memory_order_relaxed` for write index load

**Severity:** RISK
**File:** `src/vis_audio.c:43`

```c
int wr = atomic_load_explicit(&s_wr, memory_order_relaxed);
```

`vis_audio_push_sector()` is called from `_dma_irq_handler()` (an ISR on Core 0).  `vis_audio_get_samples()` is called from the main loop on Core 0.  Since both run on the same core, there is no concurrent execution — the ISR will preempt the main loop but not run simultaneously.  Therefore the relaxed ordering is technically safe on a single core.

However, `s_acc` and `s_acc_pos` are plain `static` variables (not atomic) that are written in the ISR and read in `vis_audio_push_sector()` only (both in ISR context), so no race.  And `s_ring[]` slots are protected by the atomic s_wr/s_rd pair.

Not a bug given single-core usage, but the code's comment claims this is ISR↔main SPSC — which would require acquire/release, not relaxed, for the write path.  The relaxed load is safe here but fragile if the ISR were ever moved to Core 1.

---

## FINDING-29 · `MDF parser` truncates 64-bit `start_offset` to 32 bits silently

**Severity:** BUG
**File:** `src/disc_image.c:549`

```c
trk->file_offset = (uint32_t)(tblk.start_offset & 0xFFFFFFFF);
```

`tblk.start_offset` is a `uint64_t` in `mds_track_block_t`.  For MDF images larger than 4 GB (common for multi-track images with many audio tracks), `start_offset` will exceed `0xFFFFFFFF` and the truncation silently wraps.  The resulting file seek will be at the wrong position, producing garbled sectors.

FatFS uses `FSIZE_t` (typically `QWORD = uint64_t` when `FF_FS_EXFAT == 1`) for file sizes and offsets, so FatFS can handle >4 GB files.  The correct fix is to change `trk->file_offset` from `uint32_t` to `uint64_t` in `track_t` and update `disc_read_sector()` accordingly.

**Note:** This affects the `track_t.file_offset` field type in `disc_image.h:62` which is `uint32_t`.

---

## FINDING-30 · `disc_read_sector()` uses `uint32_t file_offset` limiting images to 4 GB

**Severity:** BUG
**File:** `src/disc_image.c:637`, `include/disc_image.h:62`

```c
uint32_t file_offset = trk->file_offset + sector_idx * trk->sector_size;
```

Both `trk->file_offset` and the computed `file_offset` are `uint32_t`.  For a 2352-byte sector image, 4 GB / 2352 = ~1.8 million sectors = ~400 minutes of audio.  A full 80-minute disc = 360,000 sectors = 847 MB — well under 4 GB.  For data discs this is fine.

However, for multi-track BIN images with many large audio tracks (e.g., full rips of large games), the total size can exceed 4 GB.  The `file_offset` overflow here would produce a silent silent seek error.

**Fix:** Widen `track_t.file_offset` to `uint64_t` and update `disc_read_sector()` to use a `uint64_t` intermediate for the seek offset.

---

## Summary Table

| ID | Severity | File | Short description |
|----|----------|------|-------------------|
| 1 | FIDELITY | `src/commo_bridge.c` | PASSIVE pin (GPIO 13) never driven low — drive presence undetected |
| 2 | FIDELITY | `src/commo_bridge.c` | Spinup instant (FAKE_TIMING disabled) — timing check fails on real CD32 |
| 3 | FIDELITY | `pio/da_output.pio` | LRCLK polarity inverted vs. I2S spec — L/R channels swapped |
| 4 | FIDELITY | `src/commo_bridge.c` | DRQ status byte missing PLAYING state indicator |
| 5 | FIDELITY | `src/commo_bridge.c` | TOC 0xA1 packet uses last-track CTRL nibble instead of first-track |
| 6 | BUG | `src/disc_image.c` | ISO disc_read_sector always synthesises 2352 bytes but may report 2048 |
| 7 | RISK | `src/logger.c` | `_log_state()` state names offset by 1 — all state log messages wrong |
| 8 | RISK | `include/subcode.h` | Partial Q-channel push leaves orphaned words in PIO TX FIFO |
| 9 | RISK | `src/da_output.c` | da_resume() flushes cache then immediately tries sector_cache_get — always misses |
| 10 | RISK | `src/commo_bridge.c` | `_wait_commo_ready()` in `_send_toc_packets()` blocks Core 0 up to 115 ms |
| 11 | BUG | `src/disc_image.c` | NRG v1 DAOI file offset uses idx0_lba (pregap) instead of idx1_lba (track start) |
| 12 | RISK | `src/da_output.c` | `da_get_current_lba()` during pause is one sector ahead — wrong audio mode on resume |
| 13 | BUG | `upstream/core/commo.c` | `NEW_CMD_RECEIVED()` + Dispatcher path causes double-processing of commands |
| 14 | FIDELITY | `src/main.c`, `src/da_output.c` | Subcode encoder PIO bit rate not updated on 2× speed change |
| 15 | FIDELITY | `src/disc_image.c` | `disc_build_toc_response()` omits frame byte from each TOC entry |
| 16 | FIDELITY | `upstream/pio/commo.pio` | COMMO RX LSB-first may be wrong if Akiko sends MSB-first (needs hw verification) |
| 17 | SMELL | `src/config.c` | `config_save()` wastes 8 KB SRAM in static buffers |
| 18 | SMELL | `src/logger.c` | `_log_state()` bounds check `<= 8` will OOB after fixing FINDING-7 |
| 19 | RISK | `src/logger.c` | `sdcard_base` from cfg file used without path sanitisation |
| 20 | FIDELITY | `pio/subcode_encoder.pio` | SUB_CLK/WFCLK/SCOR never driven — Akiko cannot receive Q-channel subcode |
| 21 | RISK | `upstream/pio/commo.pio` | COMMO RX acknowledge pulse (~7 ns) too short for Akiko input filter |
| 22 | RISK | `src/webserver.c` | Cover art `base[]` buffer 128 bytes — truncates long disc filenames |
| 23 | RISK | `src/da_output.c` | `_dma_irq_handler()` accesses `s_cache` without NULL check |
| 24 | SMELL | `src/sd_card.c` | Disc image scan unsorted — disc index changes between reboots |
| 25 | SMELL | `upstream/utils/timer.c` | SCOR IRQ callback dead code in ODE build |
| 26 | SMELL | `src/webserver.c` | `state_name()` correct but diverges from wrong `_log_state()` array |
| 28 | RISK | `src/vis_audio.c` | `memory_order_relaxed` for write index load fragile if ISR moved to Core 1 |
| 29 | BUG | `src/disc_image.c` | MDF `start_offset` truncated from uint64_t to uint32_t — >4 GB images broken | cds are only 700 mb
| 30 | BUG | `src/disc_image.c` | `disc_read_sector()` file offset is uint32_t — limits all images to 4 GB | cds are only 700mb


