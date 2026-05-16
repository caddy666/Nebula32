#pragma once
// =============================================================================
// commo_bridge.h — COMMO Bus Bridge
// =============================================================================
//
// The CD32's Amiga chipset communicates with the CD drive controller over a
// proprietary 3-wire serial bus called "COMMO" (DATA, CLK, DIR).  The ODE's
// Pico 2 replaces both the original 8051 MCU and the CXD2545Q CD DSP.
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
//   │    CD DA/data   V                                    │  command data          │
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
// COMMO GPIO ASSIGNMENTS (see gpio_map.h PIN_IF_CLK/DATA/DIR):
//   GPIO 15 (conn 20): IF_CLK  — idles high, active-low pulses
//   GPIO 16 (conn 21): IF_DATA — bidirectional
//   GPIO 17 (conn 25): IF_DIR  — output: 0=receive, 1=transmit
//

// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Build-time selection
// ---------------------------------------------------------------------------
// Set by CMakeLists.txt add_compile_definitions().
// Rotary encoder runs via MCP23017 on I2C0 (GPIO 28/29) — no GPIO conflict with COMMO.
#ifndef BUILD_WITH_COMMO
#define BUILD_WITH_COMMO   1
#endif

// Upstream COMMO command → ODE command translation
// ---------------------------------------------------------------------------
// When a command arrives on the COMMO bus from the CD32 host, this module
// translates it to the equivalent CXD2545Q opcode and queues it into the
// ODE command pipeline.  Responses flow back via the COMMO status packets.

// Initialise the COMMO bridge.
// Loads COMMO PIO programs onto PIO1 SM0/SM1 and registers the upstream
// command handler.  Must be called after clock and GPIO init in main.c.
void commo_bridge_init(void);

// Poll the COMMO bus for incoming commands.
// Call from Core 0 main loop alongside handle_console() and ui_tick().
// When a complete command packet arrives, translates it and queues it
// for the ODE command pipeline.
// Returns true if a new command was received this tick.
bool commo_bridge_poll(void);

// Send a drive status packet back to the CD32 host.
// Called automatically by the bridge after command execution completes.
// 'status_byte' is the ODE-synthesized drive status byte (see _build_status()).
void commo_bridge_send_status(uint8_t status_byte);

// Send a Q-channel status packet to the host.
// Called when the ODE has fresh subcode data (after a sector delivery).
void commo_bridge_send_qchannel(const uint8_t *qbuf_12bytes);

// Returns true if the COMMO bus hardware is present and initialised.
bool commo_bridge_is_active(void);

// Return the current drive state (used by webserver.c to render status).
drive_state_t commo_bridge_get_drive_state(void);


