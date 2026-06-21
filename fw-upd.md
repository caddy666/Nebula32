# Nebula32 — SD-card UF2 Firmware Self-Update

## Status: IMPLEMENTED

## Architecture

Two-stage dual-protection write with four phases:

```
Phase 1 (validate)    — SD reads only, no flash touched
Phase 2 (write Bank1) — Core 1 locked out; write UF2 → Bank 1 (offset 0x100000)
Phase 3 (verify)      — Bank 1 XIP readback vs UF2 data; if fail → Bank 0 untouched
Phase 4 (commit)      — Copy Bank 1 → Bank 0 sector-by-sector; watchdog_reboot
```

**Fault tolerance:** Power failure in Phase 1–3 → Bank 0 (running firmware) is untouched.
Power failure in Phase 4 → USB drag-and-drop recovery required (~4 seconds window).

## Flash layout

Banks are fixed 1 MB regions (`FW_BANK1_OFFSET = 0x100000`); the config page is the
**last 4 KB of flash** (`PICO_FLASH_SIZE_BYTES − 0x1000`), so its address scales with
the board. Firmware is ≈350 KB, so a 1 MB bank is ample.

```
Bank 0 (0x000000 – 0x0FFFFF): current firmware   ← never touched until Phase 4
Bank 1 (0x100000 – 0x1FFFFF): staging area        ← written in Phase 2, copied in Phase 4
Config (last 4 KB of flash):  ode_config_t         ← always preserved
   • Core2350B0 (16 MB): 0xFFF000 – 0xFFFFFF
   • Pico 2 / 2 W (2 MB): 0x1FF000 – 0x1FFFFF
```

## Trigger mechanisms

### Web UI
- `GET /api/fw/list` → JSON array of `.uf2` files on SD root
- `POST /api/fw/flash/{filename}` → triggers flash + reboot
- "Firmware Update" section in web UI (collapsible `<details>` panel)

### Boot-time sentinel
Place `NEBULA32.UF2` in SD card root.
On next power-on: auto-flashes → renames to `NEBULA32.OLD` → reboots.

## Validation checks

`fw_validate()` verifies (in order):

| Check | Error |
|-------|-------|
| UF2 magic numbers (start0/start1/end) | `FW_ERR_BAD_MAGIC` |
| RP2350 family ID (if FLAG_FAMILY_ID set) | `FW_ERR_FAMILY` |
| Sequential block_no (0, 1, 2, …) | `FW_ERR_BAD_SEQUENCE` |
| bank0_off < 1 MB (fits in one bank) | `FW_ERR_TOO_LARGE` |
| num_blocks consistent across all blocks | `FW_ERR_BLOCK_COUNT` |
| At least one data block | `FW_ERR_NO_BLOCKS` |
| Block at flash offset 0 (reset vector present) | `FW_ERR_NO_ORIGIN` |
| Bank 1 XIP readback matches UF2 data | `FW_ERR_VERIFY` |

## Files

| File | Role |
|------|------|
| `include/fw_update.h` | Public API (`fw_result_t`, `fw_validate`, `fw_flash_and_reboot`) |
| `src/fw_update.c` | Implementation |
| `include/sd_card_api.h` | Added `sd_scan_uf2_files` / `sd_count_uf2_files` |
| `src/sd_card.c` | Implements UF2 scan functions |
| `src/webserver.c` | `/api/fw/list`, `/api/fw/flash/*`, HTML firmware section |
| `src/main.c` | Boot-time sentinel check |
| `CMakeLists.txt` | Added `src/fw_update.c`, `hardware_watchdog` |
| `tests/host/test_fw_update.cpp` | 19 host tests (Uf2Parse × 13, Uf2Flash × 6) |

## Note on `flash_select_app_bank()`

The `flash_select_app_bank(1)` function seen in early reference code does **not exist** in
Pico SDK 2.2.0. The RP2350's actual A/B boot mechanism uses `rom_pick_ab_partition()` and
requires partition tables embedded in the firmware binary — significant added complexity.

The implemented approach achieves comparable fault tolerance through the two-stage
(Bank 1 staging → verify → copy to Bank 0) sequence without requiring partition tables.

## Test results

```
$ make && ./cd32_tests -v | tail -3
OK (557 tests, 557 ran, 1807016 checks, 0 ignored, 0 filtered out)
```

---

## Completed — Bugs, Security Holes, and Optimisations (2026-05-20)

### Bugs

**B1 — `target_addr - XIP_BASE` unsigned underflow** (`src/fw_update.c:153`)  
If `blk.target_addr < XIP_BASE` (e.g. a SRAM address like `0x20000000`), the subtraction wraps: `0x20000000 - 0x10000000 = 0x10000000 = FW_BANK1_OFFSET`. The subsequent `bank0_off >= FW_BANK1_OFFSET` check produces false (equal, not greater), so validation continues. The block is only silently discarded by the config-page skip — not rejected with a clean error.  
**Fix:** Add explicit guard before the subtraction:
```c
if (blk.target_addr < XIP_BASE) { result = FW_ERR_TOO_LARGE; break; }
```

**B2 — `payload_size` not validated — buffer overread + catastrophic flash write** (`src/fw_update.c:163, 249, 271`)  
A UF2 block with `payload_size = 0xFFFFFFFF` passes the config-page overflow check (the addition wraps to a small value). `flash_range_program(bank1_off, blk.data, 0xFFFFFFFF)` then reads ~4 GB past the 476-byte `data[]` array, corrupting flash with arbitrary SRAM content. The verify pass has the same bug via `memcmp`.  
**Fix:** Reject any block where `payload_size == 0 || payload_size > 476 || payload_size % FLASH_PAGE_SIZE != 0`.

**B3 — `flash_range_program` requires page-aligned count** (`src/fw_update.c:249`)  
`flash_range_program` (Pico SDK) asserts `count % 256 == 0`. Non-standard UF2 producers can emit 128-byte payloads, which currently reach the call unchecked and trigger a hard assert in the SDK.  
**Fix:** Covered by the `payload_size % FLASH_PAGE_SIZE != 0` check in B2.

**B4 — NOFLASH blocks break the sequential `block_no` check** (`src/fw_update.c:136–150`)  
NOFLASH blocks `continue` before `expected_next_block_no++`, so a file laid out as: block_no=0 (NOFLASH), block_no=1 (data), block_no=2 (data) fails with `FW_ERR_BAD_SEQUENCE` because the data block at position 1 is compared against `expected_next_block_no=0`.  
**Fix:** Increment `expected_next_block_no` *before* the `UF2_FLAG_NOFLASH continue`, or skip the sequence check entirely for NOFLASH blocks.

**B5 — `f_rename` destination missing volume prefix** (`src/fw_update.c:317`)  
`f_rename("0:/NEBULA32.UF2", "NEBULA32.OLD")` — the source has a FatFS volume prefix `0:/` but the destination does not. On multi-partition FatFS configurations (FF_MULTI_PARTITION=1, which this project uses), some FatFS builds reject cross-volume or prefix-inconsistent renames.  
**Fix:** Use `f_rename(path, "0:/NEBULA32.OLD")`.

**B6 — `f_rename` return value not checked — infinite re-flash loop** (`src/fw_update.c:317`)  
If the rename fails silently, `NEBULA32.UF2` remains on the SD card. On next power-on, the boot-time sentinel fires again, re-flashing the same firmware in an infinite loop. The device never reaches normal operation.  
**Fix:** Log `f_rename` failures and continue to reboot regardless — the rename is best-effort, but the failure must be surfaced.

**B7 — IRQs held disabled across erase + 16 page-writes in Stage 3** (`src/fw_update.c:302–309`)  
A single `save_and_disable_interrupts()` wraps `flash_range_erase` (~50 ms) + 16 × `flash_range_program` (~7.2 ms) = ~57 ms per sector. For a 150 KB firmware (~38 sectors) total IRQ hold time is ~2.2 seconds. All SDIO, USB, and watchdog interrupts are suppressed for this duration.  
**Fix:** Re-enable IRQs between each page-write operation in Stage 3, mirroring the per-call pattern already used in Stage 1b.

---

### Security Holes

**S1 — Path traversal: backslash not blocked** (`src/webserver.c`, `/api/fw/flash/` handler)  
The path traversal guard checks for `..` and `/` but not `\`. FatFS on Windows-adjacent toolchains treats `\` as a directory separator. A request to `/api/fw/flash/\subdir\evil.uf2` bypasses the check and reaches `fw_flash_and_reboot`.  
**Fix:** Add `strstr(fname, "\\")` to the traversal guard.

**S2 — Filename length not bounded before `snprintf` path construction** (`src/webserver.c`)  
If `fname` (from URL) is longer than `MAX_PATH_LEN - 3`, `snprintf(full, sizeof(full), "0:/%s", fname)` silently truncates. The truncated path may accidentally match a different, shorter filename on the SD card.  
**Fix:** Check `strlen(fname) < MAX_PATH_LEN - 3` before constructing the path; return 400 if too long.

**S3 — `payload_size` overread allows arbitrary SRAM disclosure via XIP compare** (`src/fw_update.c:271`)  
In the verify pass, `memcmp(xip, blk.data, blk.payload_size)` with an oversized `payload_size` reads beyond `blk.data[476]`. While the result only affects a boolean, it reads stack/BSS memory — a non-issue on bare-metal but worth fixing to prevent exploits if the validate/flash logic is ever reused in a networked context.  
**Fix:** Same payload_size validation as B2.

---

### Optimisations

**O1 — `fw_paths[8][MAX_PATH_LEN]` stack-allocated in request handler** (`src/webserver.c`)  
`MAX_PATH_LEN = 256`, so `fw_paths` is 2 KB on the lwIP callback stack. The lwIP stack is already shallow by design.  
**Fix:** Declare `static char fw_paths[8][MAX_PATH_LEN]`.

**O2 — Three full SD reads of the UF2 file** (`src/fw_update.c`)  
`fw_validate` + write pass + verify pass = 3 sequential reads of the entire file. For a 1 MB file this is ~3× the minimum I/O.  
**Possible fix:** Combine write + verify: immediately after each `flash_range_program` call (while still in the IRQ-disabled window), XIP-read the just-programmed page and compare with `blk.data`. Reduces SD reads to 2 and eliminates Stage 2 entirely. Trade-off: XIP reads during a single IRQ-disabled window are safe (cache is coherent), but the approach needs careful validation.

**O4 — Display feedback: "Flash in progress" + progress bar** (`src/fw_update.c`, `src/display.cpp`)  
The flash operation takes several seconds with no visual feedback — the display shows whatever was last rendered (cover art, disc grid, etc.). The user has no indication the device is doing anything until it reboots.  
**Fix:** Before `multicore_lockout_start_blocking()`, call into `display.cpp` to render a full-screen "Firmware Update" overlay. During the Stage 1b write loop and Stage 3 copy loop, update a progress bar keyed off `block_no / num_blocks` (available from `blk.block_no` and `blk.num_blocks` already in the loop). The progress bar needs to survive Core 1 lockout — write directly to the display via SPI on Core 0 only; do not call any display function that touches shared state. Candidate approach: a new `display_fw_progress(uint8_t pct)` function in `display.cpp` that draws a fixed-layout bar (e.g. white filled rect on black background, centred, 200×16 px) using raw SPI writes with IRQs enabled between flash calls.  
Display stages to show:  
- Before Stage 1a erase: `"Flashing firmware…"` + 0%  
- During Stage 1b write: progress 0–70% (proportional to blocks written)  
- During Stage 2 verify: `"Verifying…"` + 70–85%  
- During Stage 3 copy: `"Committing…"` + 85–99%  
- Before reboot: `"Rebooting…"` + 100%

**O5 — Firmware update logging and error persistence** (`src/fw_update.c`, `src/logger.c`)  
The current implementation uses `printf` only — output goes to UART and is lost on reboot. If flashing fails (or corrupts flash and the device bootloops), there is no post-mortem record.  
**Fix:** Integrate with the existing SD logger (`src/logger.c`) to write structured log entries at each phase transition and on error:

| Event | Log entry |
|-------|-----------|
| Validation started | `[FW] validate start: <filename>` |
| Validation passed | `[FW] validate ok: <num_blocks> blocks` |
| Validation failed | `[FW] validate FAIL: <fw_result_str(r)>` |
| Erase started | `[FW] erase bank1: <n> sectors` |
| Write started | `[FW] write bank1 start` |
| Verify started | `[FW] verify start` |
| Verify failed | `[FW] verify FAIL at block <block_no>, bank1_off 0x<addr>` |
| Commit (copy) started | `[FW] commit bank0 start` |
| Rename result | `[FW] rename sentinel: <FR_OK / error code>` |
| Reboot imminent | `[FW] reboot` |

The log file must be **flushed and closed** (`f_sync` / `f_close` on the log FIL) before `multicore_lockout_start_blocking()` — SD I/O cannot happen safely while Core 1 is locked out and IRQs are cycling on/off for flash ops. Post-lockout log entries (verify fail, commit start, reboot) must be written *after* `multicore_lockout_end_blocking()` or buffered in SRAM and flushed at the earliest safe point.  
**Error persistence:** On `FW_ERR_VERIFY`, write the error to the existing config flash region (a dedicated `last_fw_error` field in `ode_config_t`), so the web UI can surface it on the next boot even if the UART/SD log was not captured. Include the failed `block_no` and `bank1_off` address for diagnosability.

**O6 — Update success animation on next boot** (`src/fw_update.c`, `src/main.c`, `src/display.cpp`)  
After a successful flash there is no confirmation that anything happened — the device simply reboots into what looks like a normal start-up.

**Mechanism — watchdog scratch register:**  
The RP2350 watchdog has 8 × 32-bit scratch registers (`watchdog_hw->scratch[0]–[7]`) that survive a watchdog reboot but are cleared on power-cycle. The SDK reserves `scratch[4]–scratch[7]`; `scratch[0]–scratch[3]` are free for application use.  

Write a magic value immediately before `watchdog_reboot()` in `fw_flash_and_reboot()`:
```c
#define FW_UPDATE_MAGIC  0xF1A54B007UL   /* "FLASH BOOT" */
watchdog_hw->scratch[0] = FW_UPDATE_MAGIC;
watchdog_reboot(0, 0, 0);
```

In `main.c`, after `display_init()` but before normal UI starts:
```c
if (watchdog_hw->scratch[0] == FW_UPDATE_MAGIC) {
    watchdog_hw->scratch[0] = 0;          /* consume — one-shot */
    display_fw_success_animation();       /* ~2 s celebratory effect */
}
```

**`display_fw_success_animation()` — Boing Ball:**  
Implement the iconic Amiga Boing Ball as the firmware update notification: red/white checkered sphere bouncing with a drop shadow on a grey grid background, looped for ~2–3 s on the 240×240 ST7789. Immediately recognisable to any Amiga user as "something good just happened." Implementation notes:
- Pre-compute sphere frames as RGB565 sprite-sheet stored in flash (or render on-the-fly using a polar→RGB565 lookup for the checker pattern)
- Drop shadow: semi-transparent dark ellipse beneath the ball, scaled by vertical position
- Background grid: thin grey lines on white, drawn once to the framebuffer before the loop starts
- "FIRMWARE UPDATED" text overlay beneath the ball using the existing display font

**Juggler — add as a new visualisation effect in `src/effects.c`:**  
Eric Graham's 1985 ray-traced juggling robot should be added as a sixth demoscene visualisation (alongside SPECTRUM/SCOPE/RASTER/COMBO/SPACEBALLS) selectable during normal audio playback. Implementation approach: compressed sprite-sheet of keyframes stored in flash, cycled in sync with the DA audio beat or at a fixed ~12 fps. Add `EFFECT_JUGGLER` to the effect enum in `src/effects.c` and wire it into the existing effect rotation in `src/ui.c`.  

**Caveat:** This only fires on watchdog reboot, not power-cycle. If the user pulls power immediately after flashing and reconnects, the scratch register is cleared and no animation plays — which is the correct behaviour (no false positives on normal power-on).

**O3 — `expected_num_blocks` written before `FW_ERR_NO_BLOCKS` check** (`src/fw_update.c:189`)  
If every block is NOFLASH, `*out_num_blocks = 0` is written before `FW_ERR_NO_BLOCKS` is returned. This is safe since the caller checks the return value, but it is a misleading output path.  
**Fix:** Move the `FW_ERR_NO_BLOCKS` and `FW_ERR_NO_ORIGIN` checks before the `*out_num_blocks` assignment.
