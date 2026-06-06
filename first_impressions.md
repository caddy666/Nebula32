# First Impressions — Code Review Notes

New-joiner review of `include/`, `src/`, and `upstream/`.  Severity: **BUG** (wrong behaviour, data corruption, UB), **RISK** (latent / conditional breakage), **SMELL** (quality / maintainability).  Ordered roughly from most to least severe.

---

## BUGS

### 1. `webserver.c:630` — use-after-free on `p->tot_len`

```c
pbuf_free(p);
// ...
tcp_recved(pcb, p ? p->tot_len : 0);  // p was just freed
```

`pbuf_free` may deallocate `p`.  Reading `p->tot_len` afterwards is undefined behaviour.  Save `tot_len` before the free.

---

### 2. `webserver.c:199` — HTML buffer silent overflow (potential heap stomp)

`build_html_page` builds into a 32 KB `static char html[]` via the `HCAT` macro:

```c
pos += snprintf(html + pos, sizeof(html) - pos, ...);
```

When `pos >= 32768`, `sizeof(html) - pos` is a `size_t` subtraction that **underflows** to a huge value.  Subsequent `snprintf` calls will write well past the end of the array.  With the maximum 99-disc grid at ~400 bytes per entry the HTML can easily exceed 32 KB.  The `WebserverHtml` test only passes with a small fixture image list — it will not catch this in production.

---

### 3. `da_output.c:69-71, 287, 431-433` — non-atomic `s_drq_pending`

```c
static volatile bool s_drq_pending = false;  // set by DMA ISR
```

`volatile` is not sufficient for inter-core visibility on Cortex-M33.  The ISR (which can run on Core 0's NVIC but interrupts it) sets the flag; `commo_bridge_poll` reads and clears it without any memory barrier.  A compiler or CPU reorder can cause the read to see a stale value.  Replace with `__atomic_*` (ACQUIRE load, RELEASE store) the same way `sector_cache.c` handles `slot->valid`.

---

### 4. `commo_bridge.c:207` — `_send_toc_packets` unguarded access to `g_disc.tracks`

```c
const track_t *trk1 = &g_disc.tracks[g_disc.first_track - 1];
```

If `g_disc.first_track == 0` (disc not open, or a malformed image where the first-track field was never set), this is `tracks[-1]` — out-of-bounds UB.  `READ_TOC_OPC` can arrive at any time; the handler should guard with `if (!g_disc.file_open) return _build_status()`.

---

### 5. `disc_image.c:422` — unaligned 32-bit reads in NRG parser

```c
uint32_t idx0_lba = be32(*(uint32_t*)(entry + 16));
uint32_t idx1_lba = be32(*(uint32_t*)(entry + 20));
uint32_t end_lba  = be32(*(uint32_t*)(entry + 24));
```

`entry` is a `uint8_t[42]`.  Casting to `uint32_t *` at unaligned offsets violates strict aliasing and may trap on strict-alignment platforms.  The v2 path correctly uses `memcpy` for the 64-bit field — apply the same pattern here.  On the RP2350 these silently mis-read under `-fsanitize=undefined` (UBSan catches the alignment violation at runtime).

---

### 6. `webserver.c:322` — HTML injection (XSS) in disc name output

```c
HCAT("<div class='disc-name'>%s</div>", base);
```

`base` is a filename extracted directly from the FAT volume.  A filename containing `</div><script>alert(1)</script>` renders as executable script in the browser.  The attacker just needs write access to the SD card.  The same issue exists in the `alt` attribute and the JSON `/api/images` response.  HTML-encode `<`, `>`, `&`, `"` before inserting into HTML context.

---

## RISKS

### 7. `main.c:153-154` — `config_save` on every disc load (flash wear + COMMO blackout)

```c
g_config.last_image_index = (uint8_t)(index & 0xFF);
config_save(&g_config);   // erases 4 KB flash, IRQs disabled ~50 ms
```

`load_disc_image` is called at boot (auto-load), and on every rotary-encoder or web selection.  Each call erases the config flash sector even when the index hasn't changed.  At one disc load per power cycle that is ~1 flash erase/day.  More importantly, IRQs are disabled for ~50 ms — the COMMO bus is completely dead during that window.  If Akiko is mid-command, the timing contract breaks.

Fix: only call `config_save` when `index != g_config.last_image_index`.

---

### 8. `sector_cache.c:189-192` — SD read errors advance `next_fetch_lba` permanently

```c
slot->error = true;
__atomic_store_n(&cache->next_fetch_lba, fetch_lba + 1, __ATOMIC_RELEASE);
```

A transient SD error (card wake-up latency, CRC error) permanently skips the sector.  The slot is not marked valid so Core 0 will miss it, leading to audio silence or a hang in `da_start_play`'s `sector_cache_get` poll.  Real SD cards have ~1% transient error rates during multi-session gaming.  Should retry the sector at least once (a second `disc_read_sector` call) before giving up.

---

### 9. `da_output.c:323-331` — `da_pause` does not clear PIO FIFO

`da_stop` explicitly drains the PIO TX FIFO and restarts the SM.  `da_pause` only aborts the DMA channels but leaves whatever partial words are already in the PIO FIFO in place.  On resume, those stale words clock out first, corrupting the first few I2S frames.  The PIO should be stopped and restarted on pause just as on stop, or at minimum the FIFO should be drained.

---

### 10. `sector_cache.h:39-40` — dead fields in `sector_cache_t`

```c
int head;  // reserved; implementation uses linear scan
int tail;  // reserved; implementation uses linear scan
```

These fields are zeroed by `sector_cache_init` (via `memset`) and never read or written by any code path.  They consume 8 bytes and mislead readers into thinking there is a classic ring-buffer head/tail scheme.  Remove them.

---

### 11. `sector_cache.h:18` — dead include

```c
#include "pico/util/queue.h"
```

`queue_t` is not used anywhere in `sector_cache.h` or `sector_cache.c`.  Dead include adds compile-time overhead and false dependencies.

---

### 12. `webserver.c:637-648` — unbounded heap use (`malloc` per connection)

```c
http_conn_t *conn = (http_conn_t *)malloc(sizeof(http_conn_t));
```

On an RP2350 with a small heap, repeated connections (the web page auto-refreshes every 5 seconds) can fragment the heap.  `malloc` failure is handled (drops the connection) but not gracefully recovered.  A static pool of two or three `http_conn_t` slots is more appropriate for a no-RTOS embedded target.

---

### 13. `main.c:541` — `sleep_us(200)` caps COMMO response latency

The Core 0 main loop sleeps 200 µs unconditionally at the end of every tick.  COMMO command processing happens inside `commo_bridge_poll()` which is called just above.  If Akiko sends a back-to-back command 1 µs after the poll returns, the response is delayed up to 200 µs.  The protocol spec does not document the host's patience; some Amiga CD32 games have been observed to retry within 100 µs.  Remove the sleep or gate it on whether `commo_bridge_poll()` returned false (idle).

---

### 14. `main.c:173-178` — Core 1 busy-spins when cache is full

```c
static void core1_main(void) {
    while (true) {
        sector_cache_prefetch_tick(&g_cache);
        tight_loop_contents();
    }
}
```

`sector_cache_prefetch_tick` returns immediately when all 8 slots are valid.  Core 1 then spins at maximum frequency, burning power and adding noise on the shared SRAM bus.  A `sleep_us(100)` (one tenth of the minimum sector period at 2× speed) when the first pass finds no free slots would reduce Core 1 load from ~100% to under 1% during steady-state playback with no correctness loss.

---

### 15. `main.c:481-482` — 1 ms timer for a 2 s flush interval is wasteful

```c
add_repeating_timer_us(-1000, periodic_update_cb, NULL, &update_timer);
```

The callback calls `logger_flush_if_due()` which only does work every 2000 ms.  The timer fires 2000 times per useful work unit.  The extra overhead is small on a 135 MHz core but the mismatch is confusing.  Change the period to 500 ms, or call `logger_flush_if_due` directly from the main loop without a timer.

---

### 16. `GPIO 14 / RESET pin` — never configured

The hardware table in `CLAUDE.md` lists GPIO 14 as `RESET` (active-low from CD32) with direction IN.  Neither `main.c` nor `commo_bridge_init` calls `gpio_init(14)` or sets up an IRQ on it.  The `HostReset` test suite validates reset behaviour, but those tests use fake GPIO harnesses — the physical pin is floating in firmware.  If the CD32 asserts RESET during gameplay the drive state is not cleared.

---

### 17. `webserver.c:161-169` — `state_name()` enum offset mismatch

```c
static const char *state_name(int state) {
    switch (state) {
        case 0: return "RESET";   case 1: return "IDLE";
        case 2: return "SPINUP";  ...
```

`drive_state_t` starts at `DRIVE_IDLE = 0`.  `state_name(0)` returns `"RESET"` but `DRIVE_IDLE` = 0 should return `"IDLE"`.  Every state label in the web UI is shifted by one.  The logger's `_log_state` in `logger.c:580-588` has a separate correct copy of state names.  Unify them.

---

### 18. `disc_image.c` / `webserver.c` — settings file parsed twice independently

`logger.c:parse_settings_file` reads `0:/nebula32.cfg` for logging settings.  `webserver.c:read_wifi_config` re-opens and re-reads the **same file** for WiFi credentials.  Any new config key must be added in two places, and both parsers can diverge in their handling of whitespace, comments, and encoding.  One parser should extract all known keys in a single pass.

---

### 19. `subcode.c:204-234` — production source contains large dead-code comment

The original buggy `subcode_build_q_isrc_ORIG` implementation is preserved as a 30-line block comment.  This is documentation clutter that belongs in a commit message or CHANGELOG, not in the source tree.  Delete it.

---

### 20. `.o` object files committed to the repository

```
src/ecc.o  src/fft.o  src/sector_cache.o  src/subcode.o  src/vis_audio.o
upstream/utils/maths.o
```

Build artefacts are checked in.  They force unnecessary re-links, cause spurious diffs, and can introduce ABI confusion if the source changes but the `.o` is not rebuilt.  Add `*.o` to `.gitignore` and remove from the tree.

---

### 21. `ecc.c` — P/Q ECC interleave is simplified and non-conformant

The ECMA-130 P-parity encoder uses a 43 × 24 matrix interleave; the implementation uses a flat stride-86 walk over 22 bytes which skips the EDC and zero-pad region entirely.  The Q-parity reuses `rs_p_encode` (same generator polynomial, different codeword length) which is incorrect — Q uses a 43-element codeword with a diagonal interleave, not the same P generator.  Real CD32 firmware does not verify P/Q parity (the CXD2545Q handled that transparently), so this bug is latent, but software using `READS` mode that does verify parity will see errors.  Reference: ECMA-130 Annex C.

---

### 22. `upstream/commo.c:172-183` — `COMMO_SM_TXD_DATA` checks `commo_data_is_low` during transmit

```c
case COMMO_SM_TXD_DATA:
    if (commo_data_is_low()) break;
```

`commo_data_is_low` calls `gpio_get(PIN_COMMO_DATA)`.  During transmit the pin has been switched to output by `commo_tx_send`.  `gpio_get` on an output pin returns the driven level, not the host's intention.  This check is a vestige from the original bit-bang code where the host could abort a transfer.  It is effectively dead — the GPIO output is held high by the drive so `commo_data_is_low()` always returns false and the SM advances immediately.  This is benign but confusing; document or remove the check.

---

### 23. `logger.c:318-321` — `logger_write` can block (violates non-blocking contract)

The header says "Non-blocking — log writes never stall the bus handler."  But:

```c
if (s_ring_used >= LOG_FLUSH_WATERMARK) {
    logger_flush();   // SD card write: 2-5 ms
}
```

When the ring hits 75% capacity, `logger_write` triggers a synchronous SD card write inline.  If the COMMO bus sends a burst of commands the ring can fill quickly.  A 5 ms stall inside `LOG_CMD(...)` during a PLAY_TRACK command would violate the bus timing contract.  The flush should remain deferred to the main loop; `logger_write` should only append to the ring and set a flag.

---

## SMELLS

### 24. `sector_cache_get` contract: consumed slots not immediately freed

After `sector_cache_get` the slot remains `valid=true` until the next `sector_cache_release_before` call.  At any instant there can be up to 3 consumed-but-not-freed slots (the two ping-pong buffers plus the one being fetched).  With only 8 slots, steady-state headroom is 5 prefetch positions.  At 2× speed with a slow SD card this gets tight.  The slot should be freed immediately after the DMA expand step in the ISR, not left for `release_before` to sweep up later.

---

### 25. `commo_bridge.c:621` — `static uint8_t pkt[]` in non-reentrant function

```c
static uint8_t pkt[STATUS_PACKET_LENGTH];
```

Declaring the TX packet buffer `static` saves 15 bytes of stack but makes the function non-reentrant by contract (silently wrong if ever called from two places).  At the call sites it is fine today, but the next person who adds a second call path from an IRQ context will corrupt the previous packet mid-send.  Prefer stack allocation — 15 bytes is not a meaningful saving.

---

### 26. `da_output.c` — `expand_to_i2s24` always expands full 2352 bytes regardless of sector type

For data-only sessions, `expand_to_i2s24` blindly interprets the sector's raw bytes as PCM pairs.  The resulting garbage audio is silently pushed to the DA PIO and (if `s_audio_mode` is false) discarded before the vis_audio snoop.  The PIO still clocks it out to Akiko.  The DA output for data tracks should send silence (zero-filled buffers), not random data bytes.  This is only an issue if Akiko's audio hardware is active for a data session, but it is a correctness gap.

---

### 27. `disc_find_track` — does not handle pregap LBAs

Sectors in the pregap range (`trk->pregap_lba <= lba < trk->start_lba`) are not matched by any track entry.  `disc_find_track` returns NULL for those LBAs.  `_push_subcode` silently skips subcode generation for them.  The Q-channel stream has a gap during disc lead-in and inter-track pauses which Akiko may misinterpret.

---

### 28. `config.h:65` — `_Static_assert` instead of `CD32_SASSERT`

All other compile-time assertions in the project use the `CD32_SASSERT` wrapper (defined in `cd_types.h`) for C/C++ portability.  `config.h` uses the raw `_Static_assert` keyword, which is invalid in C++.  Use `CD32_SASSERT` for consistency.

---

*Review date: 2026-05-17.  Files covered: `include/` (20 headers), `src/` (18 source files), `upstream/` (7 source files, 10 upstream headers).*
