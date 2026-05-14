#pragma once
// =============================================================================
// upstream_types.h — Type bridge between cd32_pico upstream and cd32_ode
// =============================================================================
//
// The upstream cd32_pico firmware (Philips/Commodore 8051 port) and our ODE
// project define overlapping types and names.  This header resolves conflicts
// so both codebases can coexist in a single build.
//
// CONFLICT MAP:
//
//   Upstream defs.h          Our project             Resolution
//   ─────────────────────────────────────────────────────────────
//   cd_time_t {min,sec,frm}  msf_t {minute,second,frame}  → Both kept; bridge provided
//   BUSY / READY             cxd_drive_state_t values     → Upstream keeps own names
//   byte (uint8_t typedef)   uint8_t used directly        → 'byte' kept, no conflict
//   MAX_TRACK_STORED_IN_TOC  MAX_TRACKS (99)              → Both kept, different uses
//   Q-channel subcode types  subcode_frame_t (ours)       → Upstream struct is identical
//
// ARCHITECTURE NOTE:
//   The upstream project controls REAL hardware (CXD2500BQ DSP + DSIC2 servo)
//   via the COMMO 3-wire serial bus.  Our ODE project replaces the physical disc
//   by outputting correct DA/SUB serial streams from SD card data.
//
//   In the merged build, the upstream code provides:
//     • The COMMO bus PIO driver (commo.pio / core/commo.c)
//     • The hardware command dispatch pipeline (dispatcher / player / cmd_hndl)
//     • BCD/time arithmetic utilities (utils/maths.c)
//     • 8 ms software timer system (utils/timer.c)
//
//   Our ODE code provides:
//     • SD card sector serving (disc_image, sector_cache)
//     • DA serial output PIO (da_output.pio / da_output.c) → Akiko + LC78835M
//     • Disc image format parsers (ISO/BIN/NRG/MDF)
//     • COMMO bridge (translates COMMO opcodes to ODE pipeline calls)
//     • Web server, rotary encoder UI, display, logger
//
// =============================================================================


// ---------------------------------------------------------------------------
// Pull in the upstream type definitions
// Bare includes resolve via upstream/include in CMakeLists include_directories
// ---------------------------------------------------------------------------
#include "defs.h"      // cd_time_t, byte, opcodes, process states
#include "serv_def.h"  // servo states, CXD2500 mode constants
#include "timer.h"     // TIMER_* indices, software timer API

// ---------------------------------------------------------------------------
// cd_time_t ↔ msf_t conversion bridge
// ---------------------------------------------------------------------------
// Our cd_types.h uses msf_t {minute, second, frame} with BCD values.
// The upstream uses cd_time_t {min, sec, frm} with either BCD or hex
// depending on context (see maths.c bcd_to_hex_time).
//
// These inline converters let both representations be used interchangeably.
#include "cd_types.h"  // msf_t, drive_state_t
#include "maths.h"     // bcd_to_hex, compare_time, calc_tracks etc.

static inline cd_time_t msf_to_cd_time(msf_t m) {
    cd_time_t t;
    t.min = m.minute;
    t.sec = m.second;
    t.frm = m.frame;
    return t;
}

static inline msf_t cd_time_to_msf(cd_time_t t) {
    msf_t m;
    m.minute = t.min;
    m.second = t.sec;
    m.frame  = t.frm;
    return m;
}

// ---------------------------------------------------------------------------
// Opcode summary
// ---------------------------------------------------------------------------
// The upstream firmware uses these opcodes on the COMMO bus.
// commo_bridge.c handles each one and routes it to the ODE pipeline.
//
//   START_UP_OPC     (0x02) → motor on, drive ready
//   STOP_OPC         (0x03) → stop DA output
//   PLAY_TRACK_OPC   (0x04) → start DA streaming
//   PAUSE_ON_OPC     (0x05) → pause DA DMA
//   PAUSE_OFF_OPC    (0x06) → resume DA DMA
//   SEEK_OPC         (0x07) → seek to MSF address (sector_cache_seek)
//   READ_TOC_OPC     (0x08) → send TOC via COMMO
//   READ_SUBCODE_OPC (0x09) → send Q-channel subcode
//   SINGLE_SPEED_OPC (0x0A) → da_set_double_speed(false)
//   DOUBLE_SPEED_OPC (0x0B) → da_set_double_speed(true)

