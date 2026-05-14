# Upstream Merge Notes — cd32_pico → cd32_ode

This directory contains the complete source tree from the `cd32_pico` project
(a clean Pico 2 port of the original 1992–1993 Philips/Commodore CD32 drive
MCU firmware).  All files are kept verbatim; no edits were made to the
upstream source.

---

## What cd32_pico is

A port of the **original 8051 MCU firmware** that ran inside the CD32's
optical drive unit.  The original MCU controlled:

- **CXD2500BQ** — Sony CD signal processor (EFM decode, PLL, servo DSP)
- **DSIC2** — Focus, radial, sledge (sled) servo controller IC
- **COMMO bus** — 3-wire serial bus to the Amiga chipset (AGA)

The cd32_pico project replaces the 8051 with a Pico 2, communicating with
the same real hardware over the same serial buses, using PIO state machines
instead of GPIO bit-banging.

---

## What cd32_ode is (this project)

An **optical drive emulator** — it replaces the entire drive mechanism
(laser + mechanics + CXD2545Q + DSIC2 + MCU) with a Pico 2 serving ISO/BIN/
NRG/MDF images from an SD card. 

---

## Why they were merged

| Layer | cd32_pico | cd32_ode |
|-------|-----------|----------|
| CD32 host interface | COMMO 3-wire serial bus | 
| Data source | Real disc via physical laser | SD card disc images |
| Hardware needed | Real CXD2500BQ + DSIC2 ICs | None (full emulation) |
| Servo / mechanics | Real DSIC2 servo control | Not applicable |

The merge brings these benefits:

1. **COMMO bus driver** — `core/commo.c` + `pio/commo.pio` gives cd32_ode the
   ability to communicate with the CD32's Amiga chipset over the original
   3-wire bus, *in addition to* the parallel bus interface.

2. **Authoritative command opcode table** — `include/defs.h` documents all
   TRAY_OUT through WRITE_DSIC2 opcodes from the real firmware.  These cross-
   reference against cd32_ode's CXD2545Q command set.

3. **BCD time arithmetic** — `utils/maths.c` provides battle-tested
   `add_time`, `subtract_time`, `compare_time`, `calc_tracks`, and BCD↔hex
   conversion functions that are directly reusable.

4. **Software timer system** — `utils/timer.c` provides the 8 ms cooperative
   timer used throughout the original firmware's sequencer.

5. **Real hardware bring-up path** — The CXD2500BQ and DSIC2 PIO drivers
   (`drivers/driver.c`, `pio/cxd2500_tx.pio`, `pio/dsic2.pio`) let cd32_ode
   be adapted to control real hardware in the future.

---

## Files used from upstream in the merged build

| File | Used by |
|------|---------|
| `core/commo.c` | `commo_bridge.c` — COMMO bus protocol state machine |
| `core/dispatcher.c` | `commo_bridge.c` — packet arbitration |
| `core/cmd_hndl.c` | `commo_bridge.c` — opcode validation + Dispatcher decl |
| `core/sts_q_id.c` | `commo_bridge.c` — status/Q-channel update helpers |
| `utils/maths.c` | `upstream_types.h` — BCD/time arithmetic |
| `utils/timer.c` | `commo_bridge_init()` — 8 ms upstream software timer |
| `include/defs.h` | `upstream_types.h` — type bridge (`cd_time_t` etc.) |
| `include/commo.h` | `commo_bridge.c` — COMMO_INIT/INTERFACE etc. |
| `include/gpio_map.h` | `commo_bridge.c` — COMMO pin numbers |
| `include/pio_hw.h` | `commo_bridge.c` — PIO API declarations |
| `include/maths.h` | `upstream_types.h` — bcd_to_hex etc. |
| `include/serv_def.h` | `upstream_types.h` — servo state constants |
| `include/timer.h` | `commo_bridge.c` + `upstream_types.h` — timer_init() |
| `include/sts_q_id.h` | `commo_bridge.c` — Get/Store_update_status |
| `include/cmd_hndl.h` | `commo_bridge.c` — command_handler, Dispatcher |
| `include/player.h` | `player_stub.c` + `commo_bridge.c` — player_interface decl |
| `pio/commo.pio` | CMakeLists.txt — pioasm generates `commo.pio.h` |
| `pio/cxd2500_tx.pio` | CMakeLists.txt — generated but unused (future use) |
| `pio/dsic2.pio` | CMakeLists.txt — generated but unused (future use) |
| `pio/qchannel_rx.pio` | CMakeLists.txt — generated but unused (future use) |

## Files NOT used from upstream in the current merged build

| File | Reason |
|------|--------|
| `core/player.c` | cd32_ode has its own cxd2545q.c command pipeline. `src/player_stub.c` provides the `player_interface` / `player_error` / `process_id` / `function_id` globals required by `cmd_hndl.c` for linkage |
| `core/service.c` | Service mode (laser/focus/motor tests) requires real hardware |
| `core/play.c` | cd32_ode's sector cache replaces physical play sequences |
| `drivers/servo.c` | Real DSIC2 servo — not applicable without real hardware |
| `drivers/strtstop.c` | Real motor start/stop — replaced by ODE state machine |
| `drivers/shock.c` | Mechanical shock detection — not applicable |
| `drivers/subcode.c` | Real subcode reading from CXD2500 — we generate synthetically |
| `drivers/driver.c` | CXD2500BQ/DSIC2 GPIO init — only needed for real hardware |
| `hal/pio_hw.c` | `pio_hw_init()` claims PIO0 SM0–SM3 which the ODE parallel bus already owns. `src/commo_bridge.c` provides custom implementations of the four `pio_commo_*` runtime functions and no-op stubs for the six unused ones, satisfying the externs declared in `pio_hw.h` |
| `hal/pio_hw_ode.c`, `hal/pio_hw_ode_patch.c` | Earlier merge attempts; superseded by the integrated `commo_bridge.c` approach |
| `main.c` (upstream) | Renamed to `main_upstream.c` for reference only |

---

