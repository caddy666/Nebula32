# Phase 5 — Carousel / Jukebox / Disc-Change Hardware Bring-Up & Validation

This is the runbook for validating the multi-disc carousel work (Phases 1–4 + 3b)
on real hardware. It is **manual** — these checks need a physical CD32 and a logic
analyzer and cannot be run in CI, which is exactly why they were split into their
own phase. Run the bench section first, then the in-machine section.

What is being validated:
- **Disc-change event** — a runtime swap makes Akiko clear the DISC bit and re-read
  the TOC (eject→insert, reusing the existing door/tray transitions).
- **Core-1 quiesce** — `disc_swap_locked()` parks Core 1 (multicore_lockout) so the
  SD read in flight is never torn by `f_close`.
- **Carousel navigation** — console `n`/`p`, web `/api/carousel/*`, `.m3u` playlists.
- **Jukebox** — `auto_advance` advances + auto-plays at end of an all-audio disc.

---

## 0. Prerequisites

| Item | Detail |
|------|--------|
| Firmware | `./build.sh core2350b` → `build-core2350b/Nebula32.uf2` (wipe `build-core2350b/` once after the board rename: `rm -rf build-core2350b`) |
| Board | Waveshare Core2350B0 (RP2350B) + RM2; built via `core2350b0_w` board file |
| SD card | ≥ 3 disc images incl. at least **two CD-DA (all-audio) images** and one data game; FAT/exFAT |
| Playlist | `0:/playlists/default.m3u` listing 2–3 image basenames (one per line) for the playlist tests |
| Debug | USB-CDC console (primary) AND a USB-serial adapter on **GPIO 16/17** (UART0 @ 115200) |
| Analyzer | ≥ 16 MHz, on **IF_CLK (GPIO 44)** + **IF_DATA (GPIO 45)**; optional DA_BCLK(1)/DA_LRCLK(2) |
| Reference | COMMO decode: `cd32_decode.py`; capture conventions match `tests/host/*.csv` |

Console keys used below (press in a terminal on the USB-CDC port):
`n`/`p` carousel next/prev · `m` activate `default.m3u` · `a` all discs ·
`P` step playlist menu (console proxy for the rotary hold+turn gesture) ·
`j` toggle jukebox · `L` list page · `s` status · `H` help.

---

## 1. Bench tests (NO CD32 attached)

The COMMO bus is idle (`commo_bridge_send_status` no-ops when `!s_active`), so these
verify the firmware-side logic without a host. Use the USB console.

| # | Action | Pass criteria |
|---|--------|---------------|
| B1 | Boot, watch console | Boots to "System ready"; no watchdog reboot loop; Core 1 prefetch line printed |
| B2 | Press `n` several times | Each press logs a load + "Disc loaded"; cover art updates; no hang/fault |
| B3 | Press `p` past the first disc | Wraps to the last disc (carousel wrap) |
| B4 | Press `n` rapidly ~10× | No crash; each swap completes; Core 1 keeps running (no stall message) |
| B5 | Put `default.m3u` on SD, press `m` | "playlist 'default': N/M entries resolved"; subsequent `n`/`p` cycle only those discs |
| B6 | Press `a` | "carousel source: all discs (T)"; `n`/`p` spans the whole library again |
| B7 | Press `j`, power-cycle | After reboot, `s`/`H` reflect jukebox ON (auto_advance persisted; no version-bump data loss) |
| B8 | With a playlist active, power-cycle | Playlist is restored on boot (console shows it resolving); start page still correct |
| B9 | Web (if WiFi up): `curl -X POST http://<ip>/api/carousel/next` | `{"ok":true,...}`; disc advances on the device |
| B10 | `curl -X POST http://<ip>/api/playlist/set/default` then `/api/playlist/all` | 200 then carousel switches source; traversal guard: `/api/playlist/set/..%2f..` → 400/!ok |
| B11 | `curl http://<ip>/api/playlist/list` | `{"playlists":[...]}` lists every `.m3u` in `0:/playlists/` by base name (no dir, no `.m3u`); empty dir → `[]` |
| B12 | Web "Carousel & Playlists" panel: pick a playlist, **Apply** | Page reloads; source switches; selecting "All discs" + Apply returns to the full library |
| B13 | Press `P` repeatedly (or rotary hold+turn) | Cycles `all → pl0 → pl1 → … → all`; each step logs `[PL] gesture → slot k/N`; first disc of a playlist loads on entry |
| B14 | Spin `P` fast through several slots, then wait for the debounce window | Exactly **one** `[PL] playlist choice persisted` line after the spin settles (debounced save — not one write per detent) |
| B15 | After B13/B14, power-cycle | Last-selected source is restored on boot (`active_playlist` persisted) |
| B16 | Set `playlist_save_ms = 0` in `nebula32.cfg`, reboot, press `P` | Each step persists immediately (a `[PL] playlist choice persisted` line per step, no coalescing) |
| B17 | Set `playlist_save_ms = 5000`, reboot, press `P` then wait | Persist line appears ~5 s after the last step (window is user-configurable; default 1500 ms, clamped ≤ 60000) |

**TSan-style stress (optional, bench):** hold `n` so swaps fire continuously for a
minute while watching for any `[CORE1]` stall or a fault — exercises the Phase-1
lockout race window under load.

---

## 2. In-machine tests (CD32 attached, powered)

> ⚠ Supported use is **front-end / CD-audio / jukebox** swapping. Swapping a *data*
> disc out from under a running game will likely crash the game — that is inherent
> (the Amiga has no "disc changed, re-load everything" path mid-game). The UI should
> only swap a data disc from an idle/menu state.

### 2A. Disc-change event on the wire
1. Boot the CD32 to the CD player / front end with an **audio** image loaded.
2. Arm the analyzer on IF_CLK/IF_DATA.
3. On the console press `n` to swap to another **audio** image.
4. Decode the capture with `cd32_decode.py`.

**Pass:**
- An eject status byte **`0x00`** (DISC bit clear) is sent at the swap (from
  `commo_bridge_signal_eject`).
- The drive then reports **BUSY** during spin-up and **READY** after
  (`signal_insert` → `_maybe_advance_state`), and Akiko **re-reads the TOC**
  (TOC/Q-channel packets follow).
- The CD player UI shows the **new disc's track count / TOC**, not the old one.
- No bus desync, no watchdog reboot, audio of the new disc plays cleanly.

### 2B. Core-1 quiesce under a real read
1. Start audio playback of disc A (so Core 1 is actively prefetching from SD).
2. Swap to disc B mid-playback via `n`.

**Pass:** clean transition; no SD/FatFS error spam on the debug UART; no fault.
(Confirms the swap parked Core 1 instead of tearing an `f_read`.)

### 2C. Jukebox auto-advance
1. Build a `default.m3u` of 2–3 **audio** images; `m` to activate, `j` to enable.
2. Let disc A play to the lead-out (or seek near end).

**Pass:**
- At end-of-disc, console logs `Jukebox: advanced to disc N (playing)`.
- The next image loads, the disc-change event fires (2A criteria), and audio
  **continues** onto the next disc with no user input.
- `(stalled)` instead of `(playing)` means the post-swap retry didn't catch a
  prefetched lead-in — see Troubleshooting.

### 2D. Data-game safety
1. From the **idle/menu** state, swap to a data game and boot it.

**Pass:** game boots normally. With the game running, a data EOD must **not**
auto-advance (the `disc_is_all_audio()` gate prevents consuming the EOD edge).

---

## 3. Tuning knobs (if Akiko mistimes the TOC re-read)

All in `src/commo_bridge.c`:
- `SPINUP_DELAY_US` (1.8 s) — time held in SPINUP before READY after an insert.
- The eject→insert gap — currently back-to-back in `load_disc_image`; some titles
  may want a short dwell with the DISC bit clear before re-insert.
- `FAKE_TIMING` deadlines generally — the same values real titles (e.g. Zool 2,
  per the `TRAY_IN` gap comment) are sensitive to.

Re-measure 2A after any change. Record the **eject → TOC-ready latency** for the README.

---

## 4. Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| Jukebox logs `(stalled)` | Core 1 hadn't prefetched the lead-in within the ~32 ms retry. Increase the retry budget in the main-loop jukebox block, or confirm SD throughput. |
| CD player keeps showing old TOC | Akiko didn't see eject or didn't re-read. Verify `0x00` on the capture; lengthen the eject→insert gap / `SPINUP_DELAY_US`. |
| Garbled debug UART only when in CD32 | The clk_peri M17SINE slave is disabled, but if re-enabled, ensure `uart_set_baudrate(uart0,...)` runs after it (already wired). |
| SD/FatFS errors during swap | Core-1 quiesce not holding — confirm `s_core1_running` is set and `multicore_lockout_*` brackets close/open/init in `disc_swap_locked`. |
| Playlist resolves 0 entries | `.m3u` basenames must match scanned filenames (case-insensitive). Confirm files exist in the image base dir; check `[PL]` console log. |
| Swap crashes a running game | Expected for data discs — gate swaps to idle/menu; this is a documented non-goal. |

---

## 5. Sign-off checklist

- [ ] B1–B10 pass on the bench (no CD32).
- [ ] 2A: eject `0x00` + TOC re-read observed on a logic-analyzer capture.
- [ ] 2B: mid-playback swap is clean (no FatFS tear).
- [ ] 2C: jukebox advances **and** auto-plays across ≥ 2 audio discs.
- [ ] 2D: data game boots from idle; never auto-advances while running.
- [ ] Measured eject→TOC-ready latency recorded in the README.
- [ ] Any tuned `commo_bridge.c` timing values committed with the measurement that justified them.

When all boxes are checked, the carousel/jukebox/disc-change feature set is
hardware-validated. Remaining optional polish (tracked in
`~/.claude/plans/multi-disc-carousel-phase-3b.md`): `/api/playlist/list` JSON +
HTML GUI buttons + a rotary playlist-menu gesture.
