// =============================================================================
// test_opc_responses.cpp — COMMO opcode → status byte and state machine
// =============================================================================
//
// Single table-driven test that sends every defined COMMO opcode and verifies
// the status byte returned and the drive state left behind.
//
// All 15 normal-mode opcodes (0x00-0x0E), all 14 service-mode opcodes
// (0x0F-0x1C), IDLE_OPC (0xFF), and one unknown opcode (0x20) are covered —
// 32 cases in total.
//
// commo_bridge.c is hardware-bound and cannot be compiled into the host suite.
// The ~40-line state machine from _handle_opc() / _build_status() is replicated
// here with hardware side-effects (da_stop, sector_cache_seek, etc.) elided as
// no-ops.  The replicated logic must stay in sync with commo_bridge.c.
//
// Status byte bits (cd_types.h):
//   0x80 = DRIVE_STATUS_BUSY   — spinning up or seeking
//   0x04 = DRIVE_STATUS_DISC   — disc present  (always set by _build_status)
//   0x01 = DRIVE_STATUS_ERROR  — hardware error
//
// TRAY_OUT_OPC is the only opcode that returns 0x00 rather than _build_status().
// This is intentional: it clears the DISC bit to tell Akiko the cover is open.
// STOP_OPC also transitions to DRIVE_IDLE but returns 0x04 (disc still loaded).
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdio.h>
#include <stdint.h>
#include "cd_types.h"   // drive_state_t, DRIVE_STATUS_*, msf_to_lba
#include "defs.h"       // *_OPC constants

// ---------------------------------------------------------------------------
// State machine — replicated verbatim from commo_bridge.c
// Hardware side-effects are no-ops; return value and state transition are exact.
// ---------------------------------------------------------------------------

static drive_state_t s_state;
static uint32_t      s_seek_lba;

static uint8_t build_status(void)
{
    uint8_t s = DRIVE_STATUS_DISC;
    if (s_state == DRIVE_SPINUP || s_state == DRIVE_SEEKING) s |= DRIVE_STATUS_BUSY;
    if (s_state == DRIVE_ERROR)                              s |= DRIVE_STATUS_ERROR;
    return s;
}

static uint8_t handle_opc(uint8_t opc, uint8_t p1, uint8_t p2, uint8_t p3)
{
    switch (opc) {
        case TRAY_OUT_OPC:
            /* da_stop() — no-op */
            s_state = DRIVE_IDLE;
            return 0x00;   // DISC bit intentionally absent: cover open, no disc

        case TRAY_IN_OPC:
            /* sector_cache_seek(0) — no-op */
            s_state = DRIVE_SPINUP;
            return build_status();

        case START_UP_OPC:
            if (s_state == DRIVE_IDLE) s_state = DRIVE_SPINUP;
            return build_status();

        case STOP_OPC:
            /* da_stop() — no-op */
            s_state = DRIVE_IDLE;
            return build_status();   // 0x04 — DISC still set unlike TRAY_OUT

        case PLAY_TRACK_OPC:
            /* sector_cache_seek / disc_find_track / da_start_play — no-ops */
            s_state = DRIVE_PLAYING;
            return build_status();

        case PAUSE_ON_OPC:
            /* da_pause() — no-op */
            s_state = DRIVE_PAUSED;
            return build_status();

        case PAUSE_OFF_OPC:
            /* disc_find_track / da_set_audio_mode / da_resume — no-ops */
            s_state = DRIVE_PLAYING;
            return build_status();

        case SEEK_OPC: {
            msf_t m = { p1, p2, p3 };
            s_seek_lba = msf_to_lba(m);
            /* sector_cache_seek — no-op */
            s_state = DRIVE_SEEKING;
            return build_status();
        }

        case JUMP_TRACKS_OPC: {
            // p1=high byte, p2=low byte of signed 16-bit relative track count.
            // No disc image in test harness; LBA computation is not replicated.
            // State and status transitions are what matter here.
            (void)p1; (void)p2; (void)p3;
            s_state = DRIVE_SEEKING;
            return build_status();
        }

        case READ_TOC_OPC:
            /* _send_toc_packets() — no-op */
            s_state = DRIVE_READY;
            return build_status();

        case READ_SUBCODE_OPC:
            /* _wait_commo_ready / commo_bridge_send_qchannel — no-ops */
            s_state = DRIVE_READY;
            return build_status();

        case SINGLE_SPEED_OPC:
        case DOUBLE_SPEED_OPC:
            /* da_set_double_speed — no-op */
            return build_status();

        case SET_VOLUME_OPC:
            return build_status();

        case ENTER_SERVICE_MODE_OPC:
            return build_status();

        default:
            // Service-mode opcodes (0x0F-0x1C), IDLE_OPC (0xFF), and any
            // unknown opcode all hit this path.  State is unchanged; current
            // status is returned rather than ERROR so Akiko does not halt.
            return build_status();
    }
}

// ---------------------------------------------------------------------------
// Test table
// ---------------------------------------------------------------------------
// Each row: name, opcode, p1/p2/p3, setup_state, expected_status, expected_state_after.
//
// setup_state is the drive state *before* the opcode is handled.
// expected_state is the state *after*.  Where the handler does not change
// state, expected_state == setup_state.
//
// Notes on START_UP_OPC: two rows — one from IDLE (promotes to SPINUP) and
// one from READY (leaves state unchanged).
// Notes on SEEK: p1=0x00 p2=0x02 p3=0x00 is BCD 00:02:00 → LBA = 0.
//   Only the resulting state (SEEKING) and status (DISC|BUSY) are asserted.
//   SEEK_from_PLAYING: Akiko may seek mid-game; state must move to SEEKING.
//   SEEK_invalid_BCD: 0xFF nibbles are not valid BCD digits (each nibble decodes
//   as 15).  Firmware passes bytes straight to msf_to_lba without validation;
//   the drive must not return an error — it must still enter SEEKING.
// Notes on JUMP_TRACKS: p1=high byte, p2=low byte of signed 16-bit track delta.
//   p1=0x00 p2=0x02 = delta +2. LBA depends on disc; state/status are asserted.
// ---------------------------------------------------------------------------
struct OpcCase {
    const char   *name;
    uint8_t       opc;
    uint8_t       p1, p2, p3;
    drive_state_t setup_state;
    uint8_t       expected_status;
    drive_state_t expected_state;
};

static const OpcCase CASES[] = {
    // name                      opc                       p1    p2    p3    setup          expected_status              expected_state
    { "TRAY_OUT",                TRAY_OUT_OPC,             0x00, 0x00, 0x00, DRIVE_READY,   0x00,                        DRIVE_IDLE    },
    { "TRAY_IN",                 TRAY_IN_OPC,              0x00, 0x00, 0x00, DRIVE_IDLE,    DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SPINUP  },
    { "START_UP_from_IDLE",      START_UP_OPC,             0x00, 0x00, 0x00, DRIVE_IDLE,    DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SPINUP  },
    { "START_UP_from_READY",     START_UP_OPC,             0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "STOP",                    STOP_OPC,                 0x00, 0x00, 0x00, DRIVE_PLAYING, DRIVE_STATUS_DISC,           DRIVE_IDLE    },
    { "PLAY_TRACK",              PLAY_TRACK_OPC,           0x01, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_PLAYING },
    { "PAUSE_ON",                PAUSE_ON_OPC,             0x00, 0x00, 0x00, DRIVE_PLAYING, DRIVE_STATUS_DISC,           DRIVE_PAUSED  },
    { "PAUSE_OFF",               PAUSE_OFF_OPC,            0x00, 0x00, 0x00, DRIVE_PAUSED,  DRIVE_STATUS_DISC,           DRIVE_PLAYING },
    { "SEEK",                    SEEK_OPC,                 0x00, 0x02, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SEEKING },
    { "SEEK_from_PLAYING",       SEEK_OPC,                 0x00, 0x02, 0x00, DRIVE_PLAYING, DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SEEKING },
    { "SEEK_invalid_BCD",        SEEK_OPC,                 0xFF, 0xFF, 0xFF, DRIVE_READY,   DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SEEKING },
    { "READ_TOC",                READ_TOC_OPC,             0x00, 0x00, 0x00, DRIVE_SPINUP,  DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "READ_SUBCODE",            READ_SUBCODE_OPC,         0x00, 0x00, 0x00, DRIVE_PLAYING, DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "SINGLE_SPEED",            SINGLE_SPEED_OPC,         0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "DOUBLE_SPEED",            DOUBLE_SPEED_OPC,         0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "SET_VOLUME",              SET_VOLUME_OPC,           0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "JUMP_TRACKS",             JUMP_TRACKS_OPC,          0x00, 0x02, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC|DRIVE_STATUS_BUSY, DRIVE_SEEKING },
    { "ENTER_SERVICE_MODE",      ENTER_SERVICE_MODE_OPC,   0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    // Service-mode opcodes — all hit default: and return current status unchanged
    { "ENTER_NORMAL_MODE",       ENTER_NORMAL_MODE_OPC,    0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "LASER_ON",                LASER_ON_OPC,             0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "LASER_OFF",               LASER_OFF_OPC,            0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "FOCUS_ON",                FOCUS_ON_OPC,             0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "FOCUS_OFF",               FOCUS_OFF_OPC,            0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "SPINDLE_MOTOR_ON",        SPINDLE_MOTOR_ON_OPC,     0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "SPINDLE_MOTOR_OFF",       SPINDLE_MOTOR_OFF_OPC,    0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "RADIAL_ON",               RADIAL_ON_OPC,            0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "RADIAL_OFF",              RADIAL_OFF_OPC,           0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "MOVE_SLEDGE",             MOVE_SLEDGE_OPC,          0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "JUMP_GROOVES",            JUMP_GROOVES_OPC,         0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "WRITE_CD6",               WRITE_CD6_OPC,            0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "WRITE_DSIC2",             WRITE_DSIC2_OPC,          0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "READ_DSIC2",              READ_DSIC2_OPC,           0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "IDLE_OPC",                IDLE_OPC,                 0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
    { "unknown_0x20",            0x20,                     0x00, 0x00, 0x00, DRIVE_READY,   DRIVE_STATUS_DISC,           DRIVE_READY   },
};

static const int NUM_CASES = (int)(sizeof(CASES) / sizeof(CASES[0]));

// ---------------------------------------------------------------------------
// TEST GROUP
// ---------------------------------------------------------------------------

TEST_GROUP(OpcResponses)
{
    void setup()    { s_state = DRIVE_IDLE; s_seek_lba = 0; }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// AllOpcodesReturnExpectedStatusAndState
//
// Iterates every case in CASES[].  A soft failure counter lets every row run
// before the test is marked failed, so a single run surfaces all mismatches.
// The final CHECK_EQUAL then fails the test if any counter > 0.
// ---------------------------------------------------------------------------
TEST(OpcResponses, AllOpcodesReturnExpectedStatusAndState)
{
    int status_failures = 0;
    int state_failures  = 0;

    for (int i = 0; i < NUM_CASES; i++) {
        const OpcCase &c = CASES[i];

        s_state    = c.setup_state;
        s_seek_lba = 0;

        uint8_t      got_status = handle_opc(c.opc, c.p1, c.p2, c.p3);
        drive_state_t got_state  = s_state;

        if (got_status != c.expected_status) {
            printf("  FAIL [%s] status: expected 0x%02x  got 0x%02x\n",
                   c.name, (unsigned)c.expected_status, (unsigned)got_status);
            status_failures++;
        }

        if (got_state != c.expected_state) {
            printf("  FAIL [%s] state:  expected %d  got %d\n",
                   c.name, (int)c.expected_state, (int)got_state);
            state_failures++;
        }
    }

    CHECK_EQUAL_TEXT(0, status_failures,
                     "One or more opcodes returned the wrong status byte (see output)");
    CHECK_EQUAL_TEXT(0, state_failures,
                     "One or more opcodes left the drive in the wrong state (see output)");
}
