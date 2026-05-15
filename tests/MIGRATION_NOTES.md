# Test Migration Notes
# Generated: 2026-05-15 — do not fix yet, reference for next session

## Summary

The `tests/` root and its subdirectories contain several old test systems
that predate the CppUTest suite in `tests/host/`.  This document records
what exists, what overlaps with CppUTest, what is worth porting, and what
is safe to delete.

---

## 1. Old custom runner (tests/ root) — test_runner.h framework

Four files share a bespoke `ASSERT_EQ / ASSERT_TRUE / SUITE` harness
defined in `test_runner.h`.  They are linked by `run_tests.c` into a
single binary.

### 1a. test_cd_types.c
- **Tests:** `lba_to_msf()`, `msf_to_lba()`, round-trips for 9 LBA values
- **CppUTest coverage:** `LbaMsf` group (11 tests) — fully covers this
- **Action: DELETE** — no new tests needed

### 1b. test_subcode.c
- **Tests:** CRC-16, ctrl/adr byte, BCD encoding, absolute time MSF,
  CRC consistency for position/MCN/ISRC packets
- **Also contains:** Three `SUITE("... [EXPECTED FAIL — lba_to_msf offset bug]")`
  blocks that document the BUG-1 relative-time bug (now fixed)
- **CppUTest coverage:** `Subcode` group (14 tests) + new BUG-1 regression
  tests added this session — fully covers this
- **Action: DELETE** — the expected-fail comments are historical, the fix is
  verified by CppUTest

### 1c. test_ecc.c
- **Tests:** EDC round-trip, corruption detection (data, sync, EDC field),
  intermediate field zeroing, P-parity population, different payloads
- **CppUTest coverage:** `Ecc` group (9 tests) — fully covers this
- **Action: DELETE** — no new tests needed

### 1d. test_da_speed.c ← NEEDS PORTING (clkdiv values are WRONG)
- **Tests:** da_is_playing/da_is_double_speed state, da_output_init,
  da_set_double_speed clkdiv write, da_start_play/pause/resume/stop
  state machine, mid-play speed change
- **CppUTest coverage:** `DaExpand` (10 tests) covers I2S word packing
  and arithmetic clkdiv values, but does NOT test the playback state
  machine (da_start_play, da_pause, da_resume, da_stop) or the actual
  pio_sm_set_clkdiv() call path
- **CRITICAL:** The test currently asserts clkdiv=48 (1×) and clkdiv=24
  (2×) — these are the **old Commodore reference values**.  After the
  BUG-2/BUG-3 fix, correct values are clkdiv=32 (1×) and clkdiv=16
  (2×).  Running this test against current da_output.c would FAIL.
- **Action: PORT** as new `DaSpeed` group in `tests/host/test_da_expand.cpp`
  or a new `test_da_speed.cpp`. Steps:
  1. Move `g_stub_last_clkdiv` float and the `pio_sm_set_clkdiv()` stub
     that writes it into `tests/host/stubs/` (or replicate inline)
  2. Port all 9 test sections from test_da_speed.c
  3. Fix clkdiv assertions: 48→32 (1×), 24→16 (2×)
  4. ~12 new tests estimated

### 1e. run_tests.c / test_runner.h
- **Action: DELETE** — superseded by CppUTest; only needed to link
  test_cd_types, test_ecc, test_subcode, test_da_speed

---

## 2. Standalone tests (tests/ root)

### 2a. test_commo.c ← NEEDS PORTING + BUG NOTE
- **Tests:** 10 tests of the upstream COMMO serial state machine
  (core/commo.c) via gpio_sim GPIO interception:
  1. Single-byte opcode (STOP_OPC 0x03) — correct receive
  2. 2-byte opcode + 1 param byte — correct receive
  3. Bad checksum → COMMO_CMD_ERROR
  4. Repeated opcode → COMMO_SAME_COMMAND
  5. Zero opcode → ERR_SEND (128-tick countdown) → COMMO_CMD_ERROR
  6. FREE_CMD_BUFFER clears status
  7. 12-body-byte opcode (all params stored)
  8. A → B → A: third reception = NEW_COMMAND
  9. **KNOWN BUG EXPOSED:** Same opcode after CMD_ERROR returns
     COMMO_SAME_COMMAND instead of NEW_COMMAND.  Root cause:
     `commo.c` does not clear `last_command` when entering the
     CMD_ERROR path.  The test deliberately fails to document this.
  10. TX bit sequence: SEND_STRING transmits 0xA5 + 0x5A checksum
      16 CLK edges LSB-first; verified via gpio_op log
- **CppUTest coverage:** None — `commo.c` is not tested at all in
  the current host suite
- **Dependency:** `gpio_sim.c` / `gpio_sim.h` (already in `tests/`)
  need to be copied into `tests/host/stubs/` and added to the Makefile
- **Action: PORT** as new `CommoProtocol` group (~10 tests).  For test 9,
  write the test with the correct expected value (NEW_COMMAND) and mark
  with a comment explaining the known bug.  Optionally fix commo.c.

### 2b. test_driver.c — LOW PRIORITY
- **Tests:** 6 tests of upstream `driver.c` CXD2500BQ/DSIC2 bit-bang SPI
  via gpio_sim GPIO log inspection:
  1. cxd2500_wr(0xA5): 3 dummy + 8 data bits LSB-first, ULAT pulse
  2. cd6_wr(MOT_PLAYM_ACTIVE) → raw byte 0xE6
  3. wr_dsic2(0xB4): 8 bits MSB-first, SILD pulse
  4. audio_cxd2500(0x00): 13 CLK edges, opcode 0x0A positions 9–12
  5. cd6_wr(MOT_OFF_ACTIVE) → raw byte 0xE0
  6. get_area() zone boundaries (inner <16, mid 16–32, outer >33)
- **CppUTest coverage:** None — driver.c is not in the ODE host build
  (BUILD_WITH_UPSTREAM_SERVO=OFF)
- **Action: DEFER** — driver.c is only relevant when a physical CXD2500BQ
  is wired up.  If upstream_servo support is ever activated, port as
  `DriverBitSeq` group.  For now, keep as a reference.

### 2c. test_LBAtoOffsetMapping.cpp — PARTIAL PORT
- Contains a "CLAUDE: FIX" comment with two stub tests:
  - `LBAtoOffsetMapping`: LBA 100, 2048-byte sectors → offset 204800.
    This is `LBA × sector_size + file_offset` (with file_offset=0).
    **Salvageable** — the arithmetic is correct and not covered in CppUTest.
  - `CommandParser_ReadCommand`: Uses non-existent `CD_Emulator` class.
    **NOT salvageable** — pseudocode for a class that doesn't exist.
- **Action: PORT** the LBA→file-offset arithmetic only as 2–3 new tests
  in `test_disc_logic.cpp` (new `DiscOffset` group):
  - Mode 1 ISO (2048 bytes/sector): offset = LBA × 2048
  - Raw BIN (2352 bytes/sector): offset = LBA × 2352
  - With non-zero file_offset: offset = file_offset + LBA × sector_size

---

## 3. tests/cd32_emulator_priority_tests/

All 6 use their **own local classes**, not the actual project code.
None exercise any function in `src/` or `upstream/`.

| File | What it actually tests | Action |
|------|------------------------|--------|
| 01_sector_deadline_miss_counter.cpp | OS scheduling: sleep_for(5ms) vs 13.3ms deadline | DELETE — non-deterministic, not our code |
| 02_ring_buffer_underflow.cpp | Local `RingBuffer` class | DELETE — SectorCache group covers ours |
| 03_sd_card_stall_simulation.cpp | Random stall with rand()%1000 + sleep | DELETE — no assertions, not our code |
| 04_q_subchannel_validation.cpp | Local `lba_to_msf()` returning raw ints (no BCD) | DELETE — wrong impl; LbaMsf covers ours |
| 05_command_parser_fuzz.cpp | Local no-op `decode_command()` | DELETE — no-op stub, not our code |
| 06_dma_double_buffer_swap.cpp | `memset(bufA, 0xAA)` + `assert(bufA[0]==0xAA)` | DELETE — tests nothing about our DMA |

---

## 4. tests/pico_cd32_test_examples/tests/

All 7 depend on `include/cdrom.hpp` and `include/ringbuffer.hpp` — local
placeholder headers with their own class definitions.  None use actual
project code from `src/` or `upstream/`.

| File | Action |
|------|--------|
| test_command_parser.cpp | DELETE — decode_command() is a placeholder |
| test_dma_sim.cpp | DELETE — local DMASimulator is just memcpy |
| test_fuzz.cpp | DELETE — decode_command() is a no-op |
| test_qsubcode.cpp | DELETE — local lba_to_msf() returns ints, not BCD; LbaMsf covers ours |
| test_ringbuffer.cpp | DELETE — local RingBuffer; SectorCache covers ours |
| test_sector_builder.cpp | DELETE — build_sector() stores raw MSF not BCD; SectorLayout covers ours |
| test_trace_replay.cpp | DELETE — decode_command() placeholder |

---

## 5. tests/cd32_logic_replay_bundle/

Contains the CSV logic-analyzer captures and converter utilities used by
the CsvReplay* CppUTest groups.  NOT a test system — it is **data + tooling**.

- `digital.csv`, `zool2.csv`, `pinball.csv`, `pon-poff-idle.csv` — already
  consumed by CppUTest CsvReplay* groups.  Keep.
- `replay.c`, `replay.cpp`, `conv_csv_rtd.c`, `converter.cpp` — build tools
  for converting raw captures.  Keep.
- No action needed.

---

## Execution plan for next session

### Step 1 — Port test_da_speed.c → `DaSpeed` CppUTest group
- New file: `tests/host/test_da_speed.cpp`
- Stub: extend `tests/host/stubs/hardware/pio.h` to capture `pio_sm_set_clkdiv()`
  calls into a `float g_stub_last_clkdiv` global (already exists in run_tests.c,
  move/replicate into the host stubs)
- Fix clkdiv assertions: 48→32 (1×), 24→16 (2×)
- ~12 tests

### Step 2 — Port test_commo.c → `CommoProtocol` CppUTest group
- New file: `tests/host/test_commo_protocol.cpp`
- Copy gpio_sim.c / gpio_sim.h into `tests/host/stubs/` and add to Makefile
- Add `upstream/core/commo.c` to the host Makefile SRCS (it's pure C, no SDK)
- Port 10 tests; for test 9 (bug exposure) write the correct assertion and
  add a comment:
  ```
  // KNOWN BUG: commo.c does not clear last_command on CMD_ERROR.
  // This test will fail until commo.c is fixed.
  ```
- Optionally fix commo.c: clear `last_command` in the CMD_ERROR branch

### Step 3 — Add DiscOffset tests to test_disc_logic.cpp
- New `DiscOffset` group, 3 tests:
  - Mode 1 ISO (2048): offset = LBA × 2048
  - Raw BIN (2352): offset = LBA × 2352  
  - Non-zero file_offset: offset = file_offset + LBA × sector_size
- Salvaged from test_LBAtoOffsetMapping.cpp's first test

### Step 4 — Delete redundant files
Safe to delete (all content superseded by CppUTest):
```
tests/test_cd_types.c
tests/test_subcode.c
tests/test_ecc.c
tests/test_da_speed.c        (after Step 1)
tests/test_commo.c           (after Step 2)
tests/run_tests.c
tests/test_runner.h
tests/test_LBAtoOffsetMapping.cpp
tests/cd32_emulator_priority_tests/   (entire directory)
tests/pico_cd32_test_examples/        (entire directory)
```

Keep for reference:
```
tests/test_driver.c          (upstream hardware driver; defer until real HW)
tests/gpio_sim.c / gpio_sim.h (needed for CommoProtocol port in Step 2)
tests/cd32_logic_replay_bundle/  (CSV data + converters)
```

---

## Known bugs uncovered by old tests

| Bug | File | Description |
|-----|------|-------------|
| COMMO last_command not cleared | `upstream/core/commo.c` | After a CMD_ERROR (bad checksum), the next packet with the same opcode reports SAME_COMMAND instead of NEW_COMMAND. Exposed by test_commo.c test 9. |
| test_da_speed.c uses wrong clkdiv | `tests/test_da_speed.c` | Asserts 24/48; should be 16/32 after BUG-2/3 fix. Old test would fail against current da_output.c. |

---

## 6. What selftest.c can do that CppUTest cannot

CppUTest runs on the host (x86/x64) and has no access to hardware.
selftest.c runs **on the Pico 2** before the CD32 is connected, so it
can catch faults that no amount of host-side testing will find:

### Things only selftest.c can verify

| Test | Why CppUTest cannot do it |
|------|--------------------------|
| GPIO pull-up/pull-down (pin is not shorted) | Requires real GPIO hardware; host stub always succeeds |
| SD card mounts + disc image is readable | Requires real SDIO hardware + real SD card |
| PIO program loads without SM conflict | pio_add_program() is a no-op stub on host |
| Output pins toggle without contention | gpio_put() is a no-op or log-capture stub on host |
| COMMO bus pins are bidir and float correctly | Electrical check; requires real pins |
| /IRQ pin can be driven low (not hard-shorted) | Electrical check |
| DOOR switch reads a valid level | Requires the physical door sensor |
| I2C ACK from MCP23017 (rotary encoder IC) | Requires real I2C bus |
| flash_range_read succeeds in config area | Requires real RP2350 flash |

### What selftest.c currently covers

Only two tests survive the earlier trim:

1. **test_gpio()** — D0-D7 (GPIO 0-7) pull-up/pull-down; /RESET (GPIO 14) idles high
2. **test_sd_card()** — SD mount, image scan, disc_open, sector 0 read

### Is it worth expanding selftest.c?

**Yes — there are several uncovered connector pins that are quick to test
with the same pull-up/pull-down technique and cost nothing to add.**

Current test_gpio() only checks D0-D7 and /RESET (GPIO 0-7, 14).
The full 26-pin CD connector has these additional testable pins:

| Pin group | GPIOs | Current coverage | Testable approach |
|-----------|-------|-----------------|-------------------|
| DA output (I2S) | 0, 1, 2 | Not tested | Drive GPIO output, read back; confirm toggles |
| DA flags | 3, 4 | Not tested | Drive low, read back; confirm not hard-shorted high |
| SUB output | 5, 6, 7, 8 | Not tested | Drive GPIO output, read back |
| COMMO bus | 15, 16, 17 | Not tested | Pull-up/down (bidir — same technique as D0-D7) |
| ACTIVE / PASSIVE | 10, 13 | Not tested | Drive output, read back |
| DOOR switch | 11 | Not tested | Read level; report what it is (no assert — state depends on disc) |

Proposed additions to selftest.c (all hardware-only, all short):

- **test_output_pins()** — configure DA (0-2), DA flags (3-4), SUB (5-8),
  ACTIVE (10), PASSIVE (13) as outputs; write 1 then 0; read back to
  confirm no short to VCC/GND.  This is the output-pin equivalent of the
  existing input pull-up test.
- **test_commo_pins()** — pull-up/pull-down on GPIO 15/16/17 (IF_CLK,
  IF_DATA, IF_DIR).  Same technique as D0-D7.  Important: these pins
  conflict with the rotary encoder when BUILD_WITH_ROTARY=ON, so guard
  with `#if BUILD_WITH_COMMO`.
- **test_door_pin()** — read GPIO 11 (DOOR) and report the level.  No
  pass/fail assert — just report "DOOR open" or "DOOR closed" so the
  engineer knows the sensor is wired.

### Can test_driver.c move to selftest.c?

**Partially — but the approach must change.**

test_driver.c uses gpio_sim to capture gpio_put() calls into a log and
verify exact bit sequences after the fact.  That technique works on the
host but is not available on the Pico 2 (you cannot read back output GPIO
state on a real RP2350 after gpio_put).

On real hardware (BUILD_WITH_UPSTREAM_SERVO=ON, CXD2500BQ/DSIC2 physically
wired), the equivalent selftest.c test would be a **smoke test**, not a
bit-sequence test:

1. Call `driver_init()` — verify it completes without fault
2. Call `cxd2500_wr(0xA5)` — verify CLK (PIN_CXD_CLK) and DATA
   (PIN_CXD_DATA) toggle (read back each pin after gpio_put by briefly
   switching to input — crude but effective pre-connection check)
3. Call `cd6_wr(MOT_PLAYM_ACTIVE)` — same smoke check
4. Call `wr_dsic2(0xB4)` — verify DSIC2 CLK/DATA toggle
5. `get_area()` zone boundaries — this is **pure logic** with no
   hardware dependency; it belongs in CppUTest, not selftest.c

The full bit-sequence verification (3 dummy clocks + 8 data bits LSB-first
etc.) requires a logic analyzer or loopback; that is beyond what selftest.c
can self-verify without external equipment.

**Recommendation:**
- Move `get_area()` zone boundary tests to CppUTest (pure logic, no hardware)
- Add driver smoke tests to selftest.c only if BUILD_WITH_UPSTREAM_SERVO=ON
  (guard with `#if BUILD_WITH_UPSTREAM_SERVO`)
- Keep test_driver.c as the definitive reference for the full bit-sequence
  spec; it can be run on the host with gpio_sim at any time

### Summary: selftest.c expansion plan

```
selftest_run()
  ├── test_gpio()          ← existing: D0-D7 + /RESET pull-up/pull-down
  ├── test_output_pins()   ← NEW: DA, SUB, ACTIVE, PASSIVE drive/read-back
  ├── test_commo_pins()    ← NEW: IF_CLK/IF_DATA/IF_DIR pull-up/pull-down
  │                                 (guarded: #if BUILD_WITH_COMMO)
  ├── test_door_pin()      ← NEW: DOOR level report (no pass/fail)
  ├── test_sd_card()       ← existing: SD mount + sector read
  └── test_driver_smoke()  ← NEW: driver_init + cxd2500_wr + wr_dsic2
                                    (guarded: #if BUILD_WITH_UPSTREAM_SERVO)
```

Estimated additions: ~25 TEST_ASSERT calls across 3 new functions.

---

## 7. Stale parallel-bus architecture — widespread cleanup needed

**Root cause:** An earlier design had the CXD2545Q register-mapped to the
68EC020 CPU via an 8-bit parallel bus (D0-D7, /WR, /RD, /CS, A0, A1).
CLAUDE.md explicitly supersedes this:

> "**There is no parallel bus.** The CXD chip is not register-mapped by
> Akiko. Akiko reads a serial bit stream from the DA lines and a serial
> subcode stream from the SUB lines."

GPIO 0-7 are **not** D0-D7.  They are:

| GPIO | Actual signal | Direction |
|------|--------------|-----------|
| 0 | DA_DATA | OUT |
| 1 | DA_BCLK | OUT |
| 2 | DA_LRCLK | OUT |
| 3 | DA_C2PO | OUT |
| 4 | DA_EMPH | OUT |
| 5 | SUB_DATA | OUT |
| 6 | SUB_CLK | OUT |
| 7 | SUB_WFCLK | OUT |
| 8 | SUB_SCOR | OUT |

### Files that still describe the old parallel bus

**`configure/CMakeLists.txt` lines 68-75 — LATENT BUILD BUG**
```cmake
# ---- CXD2545Q parallel data bus ----
CXD_DATA_BASE_PIN=0     # D0–D7 on GPIO 0–7
CXD_WR_PIN=8            # /WR strobe from 68EC020
CXD_RD_PIN=9            # /RD strobe from 68EC020
CXD_CS_PIN=10           # /CS chip select from 68EC020
CXD_A0_PIN=11           # Address bit 0
CXD_A1_PIN=12           # Address bit 1
CXD_IRQ_PIN=13          # /IRQ output to 68EC020 (active low)
CXD_RESET_PIN=14        # /RESET input from 68EC020 (active low)
```
- `CXD_DATA_BASE_PIN`, `CXD_WR_PIN`, `CXD_RD_PIN`, `CXD_CS_PIN`,
  `CXD_A0_PIN`, `CXD_A1_PIN`, `CXD_IRQ_PIN` are **defined but used
  nowhere** in any `.c`, `.h`, or `.pio` file.  They are dead defines.
- `CXD_RESET_PIN=14` is used by selftest.c but is correctly GPIO 14
  (RESET from CD32) — however the comment "from 68EC020" is wrong; the
  RESET signal comes from the CD32 via pin 7 of the 26-pin connector.
- Line 217: `pico_generate_pio_header(cd32_ode .../pio/parallel_bus.pio)`
  — **`parallel_bus.pio` does not exist**.  This will cause a build
  failure when cmake/pioasm runs on the firmware target.  The host
  CppUTest build does not use CMakeLists.txt so tests pass regardless.

**`src/selftest.c`**
- Comment line 7: "GPIO data bus pins pull up and down freely"
- Comment line 59: "Verify data bus pins (0-7)"
- The loop tests GPIO 0-7 as if they are bidirectional data bus pins,
  but they are unidirectional serial output lines (DA_DATA through
  SUB_WFCLK). The pull-up/pull-down electrical test is still a valid
  short-detection method on output pins when tested before PIO claims
  them, but the labels and loop range are wrong.  GPIO 8 (SUB_SCOR) is
  missing from the test.

**`src/hw_config.c` line 48**
```c
.SDIO_PIO  = pio1,  // Use PIO1 (PIO0 reserved for parallel bus)
```
Wrong: PIO0 is used by `da_output.pio` (SM0) and `subcode_encoder.pio`
(SM1), not a parallel bus.

**`include/sector_cache.h` line 9**
```c
// SD card on Core 1 while Core 0 services the parallel bus.
```
Wrong: Core 0 services the COMMO serial bus and DA/SUB output, not a
parallel bus.

**`include/commo_bridge.h` line 20**
```
//   │           8-bit parallel bus       │           │
```
Wrong: the interface between Akiko and the drive is the COMMO 3-wire
serial bus and the DA/SUB serial streams.

**`src/upstream_player_shim.c` line 58**
```c
// parallel bus SMs on PIO0 SM0/SM1.
```
Wrong: PIO0 SM0/SM1 run `da_output.pio` and `subcode_encoder.pio`.

**`tests/MIGRATION_NOTES.md` section 6** (this file)
- The selftest expansion plan in section 6 described GPIO 0-7 as
  "DA (0-2), DA flags (3-4), SUB (5-8)" which is correct in the GPIO
  table but the prose still said "D0-D7" in the opening summary.
  Treat that description as superseded by this section.

### Fix plan (next session — do before expanding selftest.c)

1. **`configure/CMakeLists.txt`**
   - Remove the "CXD2545Q parallel data bus" block (7 dead defines)
   - Keep `CXD_RESET_PIN=14` and `CXD_SUBCODE_PIN=24` (both used/valid)
   - Fix `CXD_RESET_PIN` comment: "from CD32 26-pin connector pin 7"
   - Remove the `pico_generate_pio_header(... parallel_bus.pio)` line
   - Fix `hardware_pio` comment: "PIO for DA/SUB serial output and
     subcode encoder"

2. **`src/selftest.c` — rewrite test_gpio()**
   - Rename function to `test_connector_pins()` or keep as `test_gpio()`
     but fix all comments
   - The loop over GPIO 0-7 should be labelled as DA/SUB serial output
     pins, not "data bus"
   - Add GPIO 8 (SUB_SCOR) to the output pin test
   - Add GPIO 9 (M17SINE) as an input — read level, don't assert (it's
     a 16.9 MHz clock from the CD32; just verify it's not stuck)
   - Fix `CXD_RESET_PIN` reference: rename or comment correctly
   - The pull-up/pull-down technique is still valid for catching shorts
     on output pins before PIO programs claim them — just relabel

3. **One-liner comment fixes** (all trivially safe):
   - `src/hw_config.c:48` → "PIO0 used by da_output.pio + subcode_encoder.pio"
   - `include/sector_cache.h:9` → "SD card on Core 1 while Core 0 drives DA/SUB serial output"
   - `include/commo_bridge.h:20` → fix ASCII diagram to show COMMO 3-wire + DA/SUB serial
   - `src/upstream_player_shim.c:58` → "da_output.pio + subcode_encoder.pio on PIO0 SM0/SM1"
