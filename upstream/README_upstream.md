# cd32_pico — Commodore CD32 Drive Firmware for Raspberry Pi Pico 2

A clean port of the original 1992–1993 Philips/Commodore CD drive controller
firmware (written for an 8051 MCU) to the **Raspberry Pi Pico 2 (RP2350)**.

---

## What this is

The **Commodore CD32** was the world's first 32-bit CD-based games console
(1993).  Its CD drive is controlled by a dedicated MCU that handles the
hardware servo, subcode reading, and a proprietary serial bus (COMMO) back
to the Amiga chipset.  This project replaces that MCU with a Pico 2.

---

## Hardware connections

| Signal       | Pico 2 GPIO | Direction | Description                         |
|:-------------|:-----------:|:---------:|:------------------------------------|
| CXD_CLK      | 2           | OUT       | CXD2500BQ serial clock (UCL)        |
| CXD_DATA     | 3           | OUT       | CXD2500BQ serial data (UDAT)        |
| CXD_LAT      | 4           | OUT       | CXD2500BQ latch strobe (ULAT)       |
| DSIC_CLK     | 5           | OUT       | DSIC2 servo IC serial clock (SICL)  |
| DSIC_DATA    | 6           | BIDIR     | DSIC2 serial data (SIDA)            |
| DSIC_LAT     | 7           | OUT       | DSIC2 latch strobe (SILD)           |
| QDA          | 8           | IN        | Q-channel subcode data from CXD2500 |
| QCL          | 9           | OUT       | Q-channel clock to CXD2500          |
| HF_DET       | 10          | IN        | HF detector (disc presence, active low) |
| DOOR         | 11          | IN        | Lid/tray switch                     |
| SCOR         | 12          | IN        | Subcode clock interrupt (falling edge) |
| COMMO_DIR    | 13          | OUT       | COMMO bus direction control         |
| COMMO_DATA   | 14          | BIDIR     | COMMO serial data to/from CD32 host |
| COMMO_CLK    | 15          | IN        | COMMO clock (driven by host)        |
| LED_STATUS   | 25          | OUT       | Onboard LED — lit after boot        |

All input pins have internal pull-ups enabled.

---

## Building

### Prerequisites

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) ≥ 2.0
- CMake ≥ 3.13
- `arm-none-eabi-gcc` toolchain

### Steps

```bash
# 1. Set PICO_SDK_PATH
export PICO_SDK_PATH=/path/to/pico-sdk

# 2. Copy pico_sdk_import.cmake into the project root
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .

# 3. Configure and build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The build produces `cd32_pico.uf2` in the `build/` directory.  Hold BOOTSEL
while connecting the Pico 2 to USB, then copy the UF2 to the mass-storage
device that appears.

---

## Project structure

```
cd32_pico/
├── CMakeLists.txt          # Build system
├── main.c                  # Entry point — init + main loop
│
├── include/
│   ├── defs.h              # Master type defs and command opcodes
│   ├── gpio_map.h          # GPIO pin assignments
│   ├── serv_def.h          # Servo states and CXD2500 mode constants
│   ├── driver.h            # Hardware driver API
│   ├── timer.h             # Software timer API
│   ├── player.h            # Player module API
│   ├── commo.h             # COMMO serial interface API
│   ├── sts_q_id.h          # Status/Q/ID packet buffer API
│   └── cmd_hndl.h          # Command handler API
│
├── core/
│   ├── player.c            # Process dispatch table + player tick
│   ├── commo.c             # COMMO RX/TX state machine
│   ├── dispatcher.c        # COMMO packet arbitration + command routing
│   ├── cmd_hndl.c          # Opcode → player_interface translation
│   └── sts_q_id.c          # Status/Q/ID packet buffer
│
├── drivers/
│   └── driver.c            # CXD2500BQ, DSIC2, subcode, sense inputs
│
└── utils/
    ├── timer.c             # 8 ms software timers + SCOR interrupt
    └── maths.c             # BCD/time arithmetic, track estimation
```

---

## Architecture

```
CD32 Host (Amiga chipset)
        │  COMMO serial bus (3-wire)
        ▼
  ┌─────────────┐
  │  commo.c    │  ← RX/TX state machine
  └──────┬──────┘
         │ opcodes + params
  ┌──────▼──────┐
  │dispatcher.c │  ← packet arbitration, command routing
  └──────┬──────┘
         │
  ┌──────▼──────┐
  │ cmd_hndl.c  │  ← opcode validation → player_interface
  └──────┬──────┘
         │ player_interface.a_command
  ┌──────▼──────┐
  │  player.c   │  ← process dispatch table, sequence stepper
  └──────┬──────┘
         │ sub-module calls
    ┌────┴────────────────┐
    │                     │
  servo/      CXD2500BQ + DSIC2
  play/       (drivers/driver.c)
  start_stop  (bit-banged SPI)
  (stubs —
  implement
  per module)
```

### Main loop order
1. `command_handler()` — push a pending COMMO command to the player
2. `COMMO_INTERFACE()` — step the serial RX/TX state machine
3. `Dispatcher()`      — route new commands; send status updates
4. `player()`          — advance the active disc-control sequence;
                         run servo / subcode / shock-recovery tasks

---

## Porting status

| Module             | File                  | Status                        |
|:-------------------|:----------------------|:------------------------------|
| Entry point        | `main.c`              | ✅ Complete                   |
| Hardware drivers   | `drivers/driver.c`    | ✅ Complete                   |
| Software timers    | `utils/timer.c`       | ✅ Complete                   |
| Maths utilities    | `utils/maths.c`       | ✅ Complete                   |
| COMMO interface    | `core/commo.c`        | ✅ Complete                   |
| Dispatcher         | `core/dispatcher.c`   | ✅ Complete                   |
| Command handler    | `core/cmd_hndl.c`     | ✅ Complete                   |
| Status/Q/ID buffer | `core/sts_q_id.c`     | ✅ Complete                   |
| Player module      | `core/player.c`       | ✅ Complete                   |
| Servo module       | *(not yet ported)*    | 🔲 Stub in player.c           |
| Play module        | *(not yet ported)*    | 🔲 Stub in player.c           |
| Start/stop module  | *(not yet ported)*    | 🔲 Stub in player.c           |
| Service module     | *(not yet ported)*    | 🔲 Stub in player.c           |
| Shock recovery     | *(not yet ported)*    | 🔲 Stub in player.c           |

The project compiles and links.  All unimplemented sub-modules return
`PROCESS_READY` immediately (no-op stubs) so the command/dispatch/player
pipeline can be tested end-to-end before hardware bring-up.

---

## Key changes from the original 8051 source

| Issue in original                          | Fix applied                                           |
|:-------------------------------------------|:------------------------------------------------------|
| `EA = 1`, `EX0 = TRUE` (8051 SFRs)        | Replaced with Pico SDK `gpio_set_irq_enabled_with_callback()` |
| `bit` type keyword                         | Replaced with `uint8_t` / `int`                      |
| `idat` / `rom` storage class modifiers     | Removed / mapped to `const`                          |
| Duplicate `main()` in mainloop.c & main.c  | Single `main.c` entry point                          |
| Broken `CMakeLists.txt` (4 of 12 sources)  | Corrected; all sources listed with correct paths     |
| `defs.h` and `gendef.h` duplicated         | Unified into `include/defs.h`                        |
| Missing closing brace in `commo.c`         | State machine fully closed and restructured          |
| Mixed 8051 + C99 fragments in `player.c`   | Rewritten as a clean process dispatch table          |
| Three competing `timer_init()` in timer.c  | Single implementation with Pico repeating_timer API  |
| `SIDA()` used as input and output with no  | GPIO direction switched explicitly before each       |
| direction switching                        | read/write operation                                 |
| `PIN_SCOR` missing from `gpio_map.h`       | Added; COMMO pins also added                         |
| `time_t` clashed with POSIX `<time.h>`     | Renamed to `cd_time_t` throughout                    |
| `ERROR` macro clashed with system headers  | Renamed to `CD_ERROR_STATE`                          |
| `processes.c` empty but included           | Removed from build; process table lives in player.c  |
