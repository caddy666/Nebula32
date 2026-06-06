# Second Pass Code Review
## CD32 ODE — Nebula32 (post first-pass fixes)
_Reviewed 2026-05-17 — all 17 findings fixed 2026-05-17_

All 28 first-pass findings are confirmed fixed.  This review goes deeper on
correctness, concurrency, and robustness that the first pass didn't surface.

Severity key: **BUG** (incorrect behaviour), **RISK** (likely to cause failures
under real-world load), **SMELL** (maintenance hazard or latent confusion).

---

## BUG-1 · `config_save()` stalls Core 1 mid-instruction during flash erase
**File:** `src/config.c:104–139`

`config_save()` calls `flash_range_erase()` + `flash_range_program()` which
disable the RP2350 XIP cache for ~50 ms so the QSPI bus is free for flash
programming.  During that window **Core 1 cannot fetch instructions from flash**
— it will stall (not crash) mid-execution as soon as it needs a new cache line.
If Core 1 is inside `disc_read_sector` → FatFS `f_read` → SDIO DMA at that
moment, the SDIO transaction is left in an indeterminate state and the SD card
may need a full power cycle to recover.

The RP2350 datasheet (section 2.8.3) and Pico SDK explicitly document that
multicore builds must call `multicore_lockout_start_blocking()` before flash
writes to park Core 1 in a SRAM trampoline, then `multicore_lockout_end_blocking()`
afterward.  Neither is present.

`config_save()` is called from `load_disc_image()` in `src/main.c:155`, which
can be triggered mid-playback by the web interface or rotary encoder while Core 1
is actively prefetching.

**Fix:** wrap the erase/program block in `multicore_lockout_start_blocking()` /
`multicore_lockout_end_blocking()`.

---

## BUG-2 · End-of-disc: DMA channels keep chaining and firing IRQs indefinitely
**File:** `src/da_output.c:259–294`

When `sector_cache_get()` returns false for `s_next_lba` (end of disc), the ISR
sets `s_playing = false` and returns.  However:

1. `chain_to` has already auto-started the **other** DMA channel.
2. That channel completes → fires `_dma_irq_handler` again → `!s_playing` → returns.
3. That channel's `chain_to` starts the first channel again → infinite loop.

Both DMA channels are still running indefinitely in the background, re-reading
stale buffer data and firing an IRQ every ~13 ms (1× speed).  The comment says
"Option A: stop", but it doesn't actually stop the DMA.  `da_stop()` correctly
calls `dma_channel_abort()` + disables IRQ; the end-of-disc path must do the same.

**Fix:** in the `else` branch at line 289, add:
```c
dma_channel_set_irq0_enabled(s_dma_ch,  false);
dma_channel_set_irq0_enabled(s_dma_ch2, false);
dma_channel_abort(s_dma_ch);
dma_channel_abort(s_dma_ch2);
irq_set_enabled(DMA_IRQ_0, false);
```

---

## BUG-3 · `tcp_recved()` called after `tcp_close()` — undefined lwIP behaviour
**File:** `src/webserver.c:660–667`

```c
handle_request(pcb, conn);   // may call tcp_write + tcp_close internally
tcp_close(pcb);              // PCB transitions to CLOSE_WAIT; pcb still valid...
conn_free(conn);
tcp_arg(pcb, NULL);
// ...
tcp_recved(pcb, tot);        // ...but using it here is UB after close
```

In lwIP (NO_SYS=1 poll mode) `tcp_close()` sends FIN and transitions the PCB
but does not immediately free it.  However, the lwIP docs explicitly state that
after `tcp_close()` the application must not call any further tcp_* functions on
that PCB.  `tcp_recved(pcb, tot)` violates this contract and may trigger an
`LWIP_ASSERT` panic on some lwIP builds or silently corrupt TCP state.

**Fix:** call `tcp_recved(pcb, tot)` **before** `handle_request()` (or at the
start of `handle_request`), then close.

---

## RISK-1 · M17SINE unconditionally slaved to GPIO 9 at boot — bench use breaks UART/I2C
**File:** `src/main.c:424–428`

```c
clock_configure(clk_peri, 0,
    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_GPIN0,
    16934400, 16934400);
```

This is executed unconditionally regardless of whether M17SINE is actually
present on GPIO 9.  On a bench (no CD32 connected), GPIO 9 floats at 0 Hz,
and `clk_peri` is configured to a dead clock source.  UART CDC, I2C (MCP23017),
and SPI (ST7789) all derive from `clk_peri` — they stop working, making USB
serial invisible and the display blank with no error output.

**Fix:** measure with `frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLKSRC_GPIN0)`
first; only slave `clk_peri` if the result is within the expected 16–17 MHz range.

---

## RISK-2 · `_wait_commo_ready()` busy-loops 20 ms × N tracks in Core 0 main loop
**File:** `src/commo_bridge.c:180–187`, called from `_send_toc_packets()`

`_send_toc_packets()` calls `_wait_commo_ready()` before every TOC packet.  For
a disc with 50 tracks, that is 53 packets × up to 20 ms each = up to **1 060 ms**
blocked in the main loop.  During this time:
- COMMO bus is not being polled for new commands
- `webserver_poll()` stalls (TCP connections time out)
- `/RESET` pin is not checked

The fix is to make TOC transmission asynchronous (queue packets, send one per
`commo_bridge_poll` call) rather than sending all in a single synchronous burst.
A simpler short-term fix: reduce the timeout to 2 ms and continue on timeout
rather than blocking.

---

## RISK-3 · Cover-art JPEG streaming drops data when TCP send buffer is full
**File:** `src/webserver.c:596–600`

```c
while (f_read(&cover_file, chunk, sizeof(chunk), &br) == FR_OK && br > 0) {
    err_t e = tcp_write(pcb, chunk, br, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) break;  // ← silently exits, leaving truncated JPEG
}
```

When lwIP's TCP send buffer is full, `tcp_write()` returns `ERR_MEM`.  The loop
`break`s and `tcp_output()` is called with whatever bytes were successfully
queued.  The browser receives a partial JPEG with `Content-Length` already sent
— browsers will show a broken image rather than retrying.

**Fix:** on `ERR_MEM`, call `cyw43_arch_poll()` to drain the send queue, then
retry `tcp_write()`.  Cap retries to avoid blocking indefinitely.

---

## RISK-4 · `atoi()` for `log_max_kb` silently accepts negative values
**File:** `src/logger.c:199`

```c
s_cfg.log_max_kb = (uint32_t)atoi(val);
```

`atoi("0")` → 0 (rotation disabled — correct).
`atoi("-1")` → -1 → cast to `uint32_t` = 4 294 967 295 KB = effectively unlimited.
A typo in `nebula32.cfg` (`log_max_kb = -1`) disables rotation silently and
could fill the SD card.

**Fix:** use `strtol()` + range check, or clamp: `s_cfg.log_max_kb = (val[0]=='-') ? 0 : (uint32_t)atoi(val);`

---

## RISK-5 · `s_audio_mode` write is not atomic — ISR reads while Core 0 writes
**File:** `src/da_output.c:69, 278, 388`

```c
static volatile bool s_audio_mode = false;   // written by Core 0 via da_set_audio_mode()
// ...
if (s_audio_mode) {                           // read from DMA ISR on same core
```

The DMA ISR executes on Core 0 and can interrupt `da_set_audio_mode()` between
the load and the store of `s_audio_mode`.  On Cortex-M33 a `volatile bool` write
is a single instruction so the torn-write risk is low, but the ordering is
unspecified.  More importantly, it's inconsistent with `s_drq_pending` which
correctly uses `__atomic_store_n`/`__atomic_load_n`.  Using atomics here makes
the intent explicit and keeps the whole module consistent.

**Fix:** replace `volatile bool` with plain `bool`; use `__atomic_store_n(&s_audio_mode, is_audio, __ATOMIC_RELAXED)` and `__atomic_load_n(&s_audio_mode, __ATOMIC_RELAXED)` (relaxed is sufficient for a flag that controls a non-critical visualiser path).

---

## SMELL-1 · `commo_bridge_send_qchannel()` still uses `static` packet buffer
**File:** `src/commo_bridge.c:685`

```c
static uint8_t pkt[Q_PACKET_LENGTH];
```

`commo_bridge_send_status()` was correctly converted to a stack-allocated buffer
in the first-pass fix (finding 25), but the parallel `send_qchannel()` function
was missed.  The `static` buffer is fine for single-Core-0 use, but it's an
inconsistency that will confuse anyone comparing the two functions.

**Fix:** change to stack allocation: `uint8_t pkt[Q_PACKET_LENGTH];`

---

## SMELL-2 · `_log_cmd`, `_log_cmd_resp`, `_log_irq` are dead code
**File:** `src/logger.c:523–616`

These three functions are defined in `logger.c` and declared in `logger.h`, but
**no callsite exists** in any source file.  `commo_bridge.c` logs commands via
`LOG_INFO_MSG` directly rather than through the structured `LOG_CMD` / `LOG_CMD_RESP`
macros.  `_log_irq` is not even declared in `logger.h`.

Additionally, `cmd_name()` in `logger.c:487–516` maps Sony CDP protocol opcodes
(GETSTAT, SETLOC, PLAY, etc.) — not the COMMO bus opcodes the bridge actually
handles.  If `_log_cmd` were ever used, the command names would be wrong.

**Fix:** delete `_log_cmd`, `_log_cmd_resp`, `_log_irq`, and `cmd_name()`.  Remove
the corresponding declarations from `logger.h`.  Replace `LOG_CMD` / `LOG_CMD_RESP`
macros with `LOG_INFO_MSG` calls if structured command logging is ever desired.

---

## SMELL-3 · Console disc selector only accepts indices 1–9; discs 10–32 unreachable
**File:** `src/main.c:220`

```c
if (c >= '1' && c <= '9') {
```

The system supports `MAX_IMAGES = 32` but the serial console silently ignores any
keypress for images 10+.  Users with more than 9 disc images must use the rotary
encoder or web interface.  There is no error message explaining the limitation.

**Fix:** accept multi-digit input (accumulate a number until `\r`/`\n`) or at
minimum print a warning when `s_image_count > 9` at boot.

---

## SMELL-4 · `FAKE_TIMING` partially controls `TRAY_IN_OPC` — latent half-implementation
**File:** `src/commo_bridge.c:474–479`

`_maybe_advance_state()` is gated correctly on `#ifdef FAKE_TIMING`, but in
`TRAY_IN_OPC`, the `s_state_deadline_us` assignment that would trigger the delay
is commented out with `//` rather than `#ifdef FAKE_TIMING`:

```c
// #ifdef FAKE_TIMING
//     s_state_deadline_us = time_us_32() + SPINUP_DELAY_US;
// #else
s_state_deadline_us = 0;  // instant — uncomment FAKE_TIMING to delay
// #endif
```

If someone defines `FAKE_TIMING`, `_maybe_advance_state()` will check the
deadline, but `s_state_deadline_us` will always be 0 (already expired), so the
spin-up delay will not actually fire.  The feature silently stops working.

**Fix:** replace the `//`-commented `#ifdef` block with the real preprocessor
guards, or remove the code entirely and document that FAKE_TIMING is not yet
wired for TRAY_IN.

---

## SMELL-5 · `da_resume()` always rewinds 2 sectors regardless of pause duration
**File:** `src/da_output.c:347–349`

```c
uint32_t resume_lba = (s_next_lba > 2) ? s_next_lba - 2 : 0;
```

The 2-sector rewind is intended to compensate for sectors loaded in the ping-pong
buffers at pause time.  But those sectors were already DMA'd to the PIO FIFO and
partially clocked out to Akiko before the pause; the audio for them was already
delivered.  Rewinding 2 sectors replays audio that was already sent, causing a
brief audible backward stutter on every resume.

The correct offset to rewind is the number of sectors that were loaded but **not
yet delivered** — typically 0 to 1.  Since `da_pause()` aborts DMA mid-transfer,
the partial current sector is lost; rewinding by 1 (the partial sector) would be
more accurate.

**Fix:** store the LBA of the sector that was actively streaming when `da_pause()`
was called, and resume from there.

---

## SMELL-6 · SD read errors in `sector_cache_prefetch_tick()` are not propagated to COMMO
**File:** `src/sector_cache.c:203–208`

When both SD read attempts fail, the code logs the error and advances
`next_fetch_lba` to skip the bad sector.  Core 0 subsequently calls
`sector_cache_get()` for that LBA, gets a miss, and times out.  There is no
mechanism for Core 0 to detect the difference between "not yet prefetched" and
"permanent read error".

The result: on a bad SD sector the drive will silently stall (spinning with DRQ
never asserted) rather than returning a meaningful error status packet to the
CD32.  Akiko will eventually time out, but the user sees a frozen game instead of
an error screen.

**Fix:** set `slot->error = true` when the retry also fails, and have
`sector_cache_get()` return `true` with `bytes_out = 0` for error slots (or a
separate boolean out-parameter).  `commo_bridge` can then detect the error and
send `DRIVE_STATUS_ERROR`.

---

## SMELL-7 · `build_html_page()` uses a 32 KB `static` local buffer — not re-entrant
**File:** `src/webserver.c:223–225`

```c
static char html[32768];
```

Since the webserver is poll-mode and connections are handled one at a time, this
is safe in practice.  However, the function returns a pointer to this static
buffer, and the caller immediately passes it to `tcp_write(..., TCP_WRITE_FLAG_COPY)`.
The `TCP_WRITE_FLAG_COPY` flag means lwIP copies the data before `tcp_write`
returns, so there is no lifetime issue.

The risk: if `build_html_page()` is ever called from a second path (e.g., a JSON
endpoint or a test fixture) before the copy completes, the buffer is silently
overwritten.  The test build (`WEBSERVER_TEST_BUILD`) already exposes this via
`webserver_get_page_for_test()`.  The 32 KB static also permanently occupies
`SRAM` even when WiFi is disabled.

**Fix:** use a caller-supplied buffer (pass `char *buf, size_t bufsz` and fill in
place) to make ownership explicit and avoid the static.

---

## SMELL-8 · `build_json_status` and `/api/images` endpoint don't JSON-escape filenames
**File:** `src/webserver.c:437–529`

`build_html_page()` correctly calls `html_escape()` before inserting disc names
into HTML, but `build_json_status()` and the `/api/images` JSON response insert
`loaded_name` and `base` directly into JSON strings without escaping:

```c
snprintf(buf + pos, ..., "\"loaded_name\":\"%s\"", loaded_name);
// and:
snprintf(..., "\"name\":\"%s\"", base);
```

A disc filename containing a double-quote, backslash, or control character
(e.g. `My "Best" Games.iso`) will produce malformed JSON that breaks the API
clients.  FatFS filenames can contain these characters on FAT32.

**Fix:** add a `json_escape()` helper (similar to `html_escape()`) and call it
before inserting filenames into JSON string contexts.

---

## SMELL-9 · `cover_exists()` calls `f_stat()` three times per disc entry during HTML generation
**File:** `src/webserver.c:162–184`, called from `src/webserver.c:342`

For each of the 32 disc images, `build_html_page()` calls `cover_exists()` which
may call `f_stat()` up to three times (tries `covers/name.jpg`, `covers/name.jpeg`,
then same-dir).  With 32 discs, that is up to **96 SD card `f_stat` calls** per
page render.  Each `f_stat` on FatFS over SDIO takes ~1–3 ms, so page generation
can block the main loop for up to ~300 ms — longer than a COMMO command timeout.

**Fix:** cache the cover-exists flag per disc image in `s_image_paths` metadata
(or a parallel `bool s_has_cover[MAX_IMAGES]` array) and refresh it only when
the disc list changes.

---

## Summary

| # | Severity | File | Issue |
|---|----------|------|-------|
| BUG-1 | **BUG** | `config.c` | `config_save()` stalls Core 1 via XIP blackout — needs `multicore_lockout` |
| BUG-2 | **BUG** | `da_output.c` | End-of-disc doesn't abort DMA channels — IRQs fire forever |
| BUG-3 | **BUG** | `webserver.c` | `tcp_recved()` called after `tcp_close()` — UB in lwIP |
| RISK-1 | **RISK** | `main.c` | M17SINE slaved unconditionally — breaks UART/I2C/SPI on bench |
| RISK-2 | **RISK** | `commo_bridge.c` | `_wait_commo_ready()` busy-loops up to 1 s per TOC response |
| RISK-3 | **RISK** | `webserver.c` | JPEG streaming silently truncates on TCP back-pressure |
| RISK-4 | **RISK** | `logger.c` | `atoi()` for `log_max_kb` wraps on negative input |
| RISK-5 | **RISK** | `da_output.c` | `s_audio_mode` inconsistently protected vs `s_drq_pending` |
| SMELL-1 | **SMELL** | `commo_bridge.c` | `send_qchannel` still uses `static` buffer unlike `send_status` |
| SMELL-2 | **SMELL** | `logger.c` | `_log_cmd`, `_log_cmd_resp`, `_log_irq`, `cmd_name` are dead code |
| SMELL-3 | **SMELL** | `main.c` | Console only accepts images 1–9; 10–32 silently unreachable |
| SMELL-4 | **SMELL** | `commo_bridge.c` | `FAKE_TIMING` half-wired — `TRAY_IN` delay never fires if enabled |
| SMELL-5 | **SMELL** | `da_output.c` | `da_resume()` rewinds 2 sectors causing audible backward stutter |
| SMELL-6 | **SMELL** | `sector_cache.c` | SD read errors not propagated — host sees silent stall |
| SMELL-7 | **SMELL** | `webserver.c` | `build_html_page()` 32 KB static buffer: not re-entrant |
| SMELL-8 | **SMELL** | `webserver.c` | JSON API endpoints don't escape disc filenames |
| SMELL-9 | **SMELL** | `webserver.c` | Up to 96 `f_stat` calls per HTML render — stalls main loop |

**Priority order for fixing:**
1. BUG-1 (data corruption risk on SD card)
2. BUG-2 (CPU burn + broken DMA state after disc end)
3. BUG-3 (lwIP assertion / connection corruption)
4. RISK-1 (completely breaks bench development)
5. RISK-2 (COMMO timeout during TOC read on multi-track discs)
