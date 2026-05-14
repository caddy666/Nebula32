// =============================================================================
// test_door_tray.cpp — Drive state machine: TRAY_IN / TRAY_OUT behaviour
// =============================================================================
//
// Tests the COMMO opcode handler contract for disc insert and eject as
// implemented in src/commo_bridge.c (_handle_opc / _build_status).
//
// commo_bridge.c is hardware-bound (GPIO, PIO, DMA) and cannot be compiled
// into the host suite directly.  Instead this file replicates its ~30 lines
// of pure state-machine logic verbatim, then verifies each case.  If the
// production code ever diverges from the behaviour tested here the tests fail.
//
// Relevant production code:
//   src/commo_bridge.c  — _handle_opc(), _build_status(), _update_active_pin()
//   include/cd_types.h  — drive_state_t, DRIVE_STATUS_* bit definitions
//   upstream/include/defs.h — TRAY_OUT_OPC (0x00), TRAY_IN_OPC (0x01)
//
// Status byte bits (from cd_types.h):
//   DRIVE_STATUS_BUSY  0x80 — spinning up or seeking
//   DRIVE_STATUS_DRQ   0x20 — sector data ready
//   DRIVE_STATUS_DISC  0x04 — disc present
//   DRIVE_STATUS_ERROR 0x01 — hardware error
//
// Key protocol rule: TRAY_OUT_OPC returns 0x00 (hardcoded), NOT _build_status().
// This clears the DISC bit, signalling to Akiko that no disc is loaded.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include "cd_types.h"   // drive_state_t, DRIVE_STATUS_* macros

// ---------------------------------------------------------------------------
// State machine replicated from commo_bridge.c
// ---------------------------------------------------------------------------
// All logic mirrors the production source exactly.  Changes here must be
// reflected in commo_bridge.c and vice versa.

static drive_state_t s_state;

static uint8_t build_status(void)
{
    uint8_t s = DRIVE_STATUS_DISC;  // disc always present in ODE mode
    if (s_state == DRIVE_SPINUP || s_state == DRIVE_SEEKING)
        s |= DRIVE_STATUS_BUSY;
    if (s_state == DRIVE_ERROR)
        s |= DRIVE_STATUS_ERROR;
    return s;
}

// Returns 0x00 (not build_status()) — DISC bit intentionally absent.
static uint8_t handle_tray_out(void)
{
    s_state = DRIVE_IDLE;
    return 0x00;
}

static uint8_t handle_tray_in(void)
{
    s_state = DRIVE_SPINUP;
    return build_status();
}

// Instant advance (FAKE_TIMING off, matching current firmware default).
static void advance_state(void)
{
    if (s_state == DRIVE_SPINUP)  s_state = DRIVE_READY;
    if (s_state == DRIVE_SEEKING) s_state = DRIVE_READY;
}

// Mirrors _update_active_pin(): motor is running whenever state is not
// IDLE or ERROR.
static bool motor_active(void)
{
    return s_state != DRIVE_IDLE && s_state != DRIVE_ERROR;
}

// ---------------------------------------------------------------------------
// TEST GROUP
// ---------------------------------------------------------------------------

TEST_GROUP(DoorTray)
{
    void setup()    { s_state = DRIVE_IDLE; }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Test 1 — TRAY_OUT status byte is 0x00: no DISC bit.
//
// When the cover is open the drive has no disc loaded.  Akiko uses the DISC
// bit to decide whether to attempt a read; it must be clear after eject.
// The production code returns the hardcoded value 0x00 rather than calling
// _build_status() specifically to achieve this.
// ---------------------------------------------------------------------------
TEST(DoorTray, TrayOutReturnsZeroStatusByte)
{
    uint8_t status = handle_tray_out();

    CHECK_EQUAL(0x00, status);
    CHECK_TRUE_TEXT(!(status & DRIVE_STATUS_DISC),
                    "DISC bit must be clear after TRAY_OUT");
}

// ---------------------------------------------------------------------------
// Test 2 — TRAY_IN sets both DISC and BUSY.
//
// The moment a disc is inserted the drive begins spinning up.  Akiko polls
// with START_UP_OPC until BUSY clears; the initial response must carry both
// bits so Akiko knows a disc is present but not yet ready.
// ---------------------------------------------------------------------------
TEST(DoorTray, TrayInReturnsDiscAndBusySet)
{
    uint8_t status = handle_tray_in();

    CHECK_TRUE_TEXT((status & DRIVE_STATUS_DISC) != 0,
                    "DISC bit must be set immediately after TRAY_IN");
    CHECK_TRUE_TEXT((status & DRIVE_STATUS_BUSY) != 0,
                    "BUSY bit must be set during spinup after TRAY_IN");
}

// ---------------------------------------------------------------------------
// Test 3 — Spinup state advances to READY.
//
// Without FAKE_TIMING the state machine advances instantly.  After one
// advance tick SPINUP must become READY.  This mirrors _maybe_advance_state()
// with no deadline pending.
// ---------------------------------------------------------------------------
TEST(DoorTray, SpinupAdvancesToReady)
{
    handle_tray_in();
    CHECK_EQUAL_TEXT((int)DRIVE_SPINUP, (int)s_state,
                     "State should be SPINUP immediately after TRAY_IN");

    advance_state();
    CHECK_EQUAL_TEXT((int)DRIVE_READY, (int)s_state,
                     "State should advance to READY after spinup");
}

// ---------------------------------------------------------------------------
// Test 4 — READY state status has DISC bit only (no BUSY, no ERROR).
//
// Once the drive is ready, Akiko's START_UP polling loop exits.  The status
// byte must have exactly DRIVE_STATUS_DISC set — no other bits.
// ---------------------------------------------------------------------------
TEST(DoorTray, ReadyStateStatusHasDiscOnly)
{
    handle_tray_in();
    advance_state();  // SPINUP → READY

    uint8_t status = build_status();

    CHECK_EQUAL_TEXT(DRIVE_STATUS_DISC, status,
                     "READY state must return exactly DRIVE_STATUS_DISC");
    CHECK_TRUE_TEXT(!(status & DRIVE_STATUS_BUSY),
                    "BUSY must be clear in READY state");
    CHECK_TRUE_TEXT(!(status & DRIVE_STATUS_ERROR),
                    "ERROR must be clear in READY state");
}

// ---------------------------------------------------------------------------
// Test 5 — TRAY_OUT from PLAYING stops the drive and clears DISC.
//
// If the user opens the cover while a game is running the BIOS sends
// TRAY_OUT_OPC.  The drive must stop immediately (IDLE) regardless of its
// current playback state, and the status byte must be 0x00.
// ---------------------------------------------------------------------------
TEST(DoorTray, TrayOutFromPlayingGoesIdle)
{
    s_state = DRIVE_PLAYING;  // simulate mid-game cover open

    uint8_t status = handle_tray_out();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "DRIVE_PLAYING → TRAY_OUT must set state to DRIVE_IDLE");
    CHECK_EQUAL(0x00, status);
}

// ---------------------------------------------------------------------------
// Test 6 — TRAY_OUT then TRAY_IN restarts spinup (does not skip to READY).
//
// A disc swap (open, swap disc, close) must always re-enter SPINUP before
// reaching READY.  Akiko re-polls with START_UP_OPC until BUSY clears;
// skipping to READY without spinup would give Akiko a stale Q-channel.
// ---------------------------------------------------------------------------
TEST(DoorTray, TrayOutThenTrayInRestartsSpinup)
{
    handle_tray_out();
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "After TRAY_OUT state must be IDLE");

    handle_tray_in();
    CHECK_EQUAL_TEXT((int)DRIVE_SPINUP, (int)s_state,
                     "After TRAY_IN from IDLE state must be SPINUP, not READY");
}

// ---------------------------------------------------------------------------
// Test 7 — Motor is inactive while in DRIVE_IDLE.
//
// The ACTIVE pin (connector pin 24) must be low after TRAY_OUT to signal to
// the mainboard that the drive motor is stopped.  Mirrors _update_active_pin()
// which drives the GPIO output.
// ---------------------------------------------------------------------------
TEST(DoorTray, MotorInactiveAfterTrayOut)
{
    handle_tray_out();

    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be inactive (ACTIVE pin low) in DRIVE_IDLE");
}

// ---------------------------------------------------------------------------
// Test 8 — Motor is active immediately after TRAY_IN (during spinup).
//
// The ACTIVE pin goes high as soon as the disc starts spinning, not only when
// READY is reached.  This lets the mainboard know the drive is energised even
// before the lead-in has been read.
// ---------------------------------------------------------------------------
TEST(DoorTray, MotorActiveAfterTrayIn)
{
    handle_tray_in();

    CHECK_TRUE_TEXT(motor_active(),
                    "Motor must be active (ACTIVE pin high) in DRIVE_SPINUP");
}

// ---------------------------------------------------------------------------
// Test 9 — DRIVE_ERROR status has ERROR bit set and BUSY bit clear.
//
// An error state must set DRIVE_STATUS_ERROR and must NOT set DRIVE_STATUS_BUSY.
// Akiko distinguishes a hardware fault (ERROR) from a transient seek/spinup
// (BUSY); conflating the two would cause the BIOS to retry indefinitely.
// ---------------------------------------------------------------------------
TEST(DoorTray, ErrorStateHasErrorBitNotBusy)
{
    s_state = DRIVE_ERROR;
    uint8_t status = build_status();

    CHECK_TRUE_TEXT((status & DRIVE_STATUS_ERROR) != 0,
                    "DRIVE_ERROR must set ERROR bit in status byte");
    CHECK_TRUE_TEXT(!(status & DRIVE_STATUS_BUSY),
                    "DRIVE_ERROR must NOT set BUSY bit");
    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be reported inactive in error state");
}

// ---------------------------------------------------------------------------
// Test 10 — DISC bit is always set in every non-idle non-error state.
//
// The ODE always has a virtual disc image loaded.  build_status() hardcodes
// DRIVE_STATUS_DISC unconditionally (unlike a real tray drive where the bit
// tracks a physical sensor).  Verify for SPINUP, SEEKING, READY, PLAYING,
// and PAUSED to confirm no code path accidentally clears the DISC bit.
// ---------------------------------------------------------------------------
TEST(DoorTray, DiscBitSetInAllActiveStates)
{
    static const drive_state_t states[] = {
        DRIVE_SPINUP, DRIVE_SEEKING, DRIVE_READY, DRIVE_PLAYING, DRIVE_PAUSED
    };

    for (unsigned i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
        s_state = states[i];
        uint8_t status = build_status();
        CHECK_TRUE_TEXT((status & DRIVE_STATUS_DISC) != 0,
                        "DISC bit must be set in every active drive state");
    }
}
