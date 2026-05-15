#pragma once
// =============================================================================
// commo_bridge.h — COMMO Bus Bridge
// =============================================================================
//
// The CD32's Amiga chipset communicates with the CD drive controller over a
// proprietary 3-wire serial bus called "COMMO" (DATA, CLK, DIR).  This is
// distinct from the CXD2545Q bus that our ODE emulates.
//
// HARDWARE TOPOLOGY:
//
//   ┌─────────────────────────────────────────────────────────────────┐
//   │                     Commodore CD32                              │
//   │                                                                 │
//   │  ┌──────────────────┐     COMMO 3-wire      ┌───────────────┐  │
//   │  │  Amiga chipset   │ ←──────────────────→  │  Drive MCU   │  │
//   │  │  (AGA akiko chip)│    <command data>     │  (8051 orig) │  │
//   │  └──────────────────┘                        └───────┬───────┘  │
//   │                 ^                                     │           │
//   │    CD DA/data   V                                    │  command datsa         │
//   │  ┌──────────────────────────────────────────────┐   │           │
//   │  │              CXD2545Q CD DSP                 │ ──┘           │
//   │  │  (servo, EFM decode, sector error correct)  │               │
//   │  └──────────────────────────────────────────────┘               │
//   └─────────────────────────────────────────────────────────────────┘
//
// In our ODE, the Pico 2 replaces BOTH the CXD2545Q AND the drive MCU:
//
//   CD32 chipset → COMMO bus → Pico 2 (this bridge) → sector_cache → disc_image
//                                                    → da_output PIO → DA lines → Akiko
//
// The COMMO bus carries:
//   HOST→DRIVE: command packets (opcode + 0-3 params + checksum)
//   DRIVE→HOST: status packets (15 bytes), Q-channel packets, ID packets
//
// COMMO GPIO ASSIGNMENTS (from upstream gpio_map.h):
//   GPIO 15: COMMO_CLK  (input when receiving, output when transmitting)
//   GPIO 16: COMMO_DATA (bidirectional)
//   GPIO 17: COMMO_DIR  (output: 0=receive, 1=transmit)
//

// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Build-time selection
// ---------------------------------------------------------------------------
// Set by CMakeLists.txt add_compile_definitions().
// Rotary encoder now runs via MCP23017 on I2C1 — no GPIO conflict with COMMO.
#ifndef BUILD_WITH_COMMO
#define BUILD_WITH_COMMO   1
#endif

// Upstream COMMO command → ODE command translation
// ---------------------------------------------------------------------------
// When a command arrives on the COMMO bus from the CD32 host, this module
// translates it to the equivalent CXD2545Q opcode and queues it into the
// ODE command pipeline.  Responses flow back via the COMMO status packets.

// Initialise the COMMO bridge.
// Calls COMMO_INIT() from the upstream firmware and registers this module
// as the command receiver.  Must be called after pio_hw_init().
void commo_bridge_init(void);

// Poll the COMMO bus for incoming commands.
// Call from Core 0 main loop alongside handle_console() and ui_tick().
// When a complete command packet arrives, translates it and queues it
// for the ODE command pipeline.
// Returns true if a new command was received this tick.
bool commo_bridge_poll(void);

// Send a drive status packet back to the CD32 host.
// Called automatically by the bridge after command execution completes.
// 'status_byte' is the CXD2545Q status byte from build_stat_byte().
void commo_bridge_send_status(uint8_t status_byte);

// Send a Q-channel status packet to the host.
// Called when the ODE has fresh subcode data (after a sector delivery).
void commo_bridge_send_qchannel(const uint8_t *qbuf_12bytes);

// Returns true if the COMMO bus hardware is present and initialised.
bool commo_bridge_is_active(void);


