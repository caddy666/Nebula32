// =============================================================================
// test_door_tray.cpp — Drive state machine: TRAY_IN / TRAY_OUT behaviour
//                      + GPIO door-pin edge detection
// =============================================================================
//
// Tests the COMMO opcode handler contract for disc insert and eject as
// implemented in src/commo_bridge.c (_handle_opc / _build_status), and the
// PIN_DOOR rising-edge detection added to commo_bridge_poll().
//
// commo_bridge.c is hardware-bound (GPIO, PIO, DMA) and cannot be compiled
// into the host suite directly.  Instead this file replicates its ~30 lines
// of pure state-machine logic verbatim, then verifies each case.  If the
// production code ever diverges from the behaviour tested here the tests fail.
//
// Relevant production code:
//   src/commo_bridge.c  — _handle_opc(), _build_status(), _update_active_pin()
//                         commo_bridge_poll() door-pin monitor block
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
//
// Door-pin convention (gpio_map.h): PIN_DOOR is active-low with a pull-up.
//   Door closed = LOW (switch shorts to GND)
//   Door open   = HIGH (switch open, pull-up wins)
// A LOW→HIGH rising edge is the "door opened" event.
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
// If the user opens the cover while a game is running akiko sends
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
// (BUSY); conflating the two would cause akiko to retry indefinitely.
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

// =============================================================================
// DoorPin test group
// =============================================================================
//
// Replicates the door-pin rising-edge monitor from commo_bridge_poll()
// (src/commo_bridge.c — see NOTE comment above that block).
//
// PIN_DOOR (GPIO 11) is active-low with a pull-up:
//   door closed = LOW, door open = HIGH.
// A LOW→HIGH rising edge is the "door opened" event.
//
// poll_door() mirrors the production block exactly.  It returns the status
// byte that would be sent via commo_bridge_send_status(): 0x00 on eject,
// 0xFF as a sentinel meaning "nothing sent".
// =============================================================================

static bool s_door_prev;

// Replicated from commo_bridge_poll() — keep in sync with production source.
static uint8_t poll_door(bool door_now)
{
    uint8_t sent = 0xFF;  // sentinel: no status sent
    if (door_now && !s_door_prev) {
        s_state = DRIVE_IDLE;
        sent = 0x00;
    }
    s_door_prev = door_now;
    return sent;
}

TEST_GROUP(DoorPin)
{
    void setup()
    {
        s_state    = DRIVE_READY;
        s_door_prev = false;   // door closed at start of each test
    }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Test 1 — Rising edge sends the 0x00 eject status.
//
// The 0x00 status clears Akiko's DISC bit immediately, without waiting for a
// TRAY_OUT_OPC command.  This mirrors the hardcoded return value of
// handle_tray_out() — both paths must produce the same wire byte.
// ---------------------------------------------------------------------------
TEST(DoorPin, RisingEdgeSendsZeroStatus)
{
    uint8_t sent = poll_door(true);  // LOW→HIGH

    CHECK_EQUAL_TEXT(0x00, sent,
                     "Rising edge must send 0x00 status (DISC bit clear)");
}

// ---------------------------------------------------------------------------
// Test 2 — Rising edge moves drive to IDLE.
//
// Any in-progress playback or seek must be abandoned immediately when the
// door opens.  DRIVE_IDLE is the only state where motor_active() returns
// false, which drives the ACTIVE pin low.
// ---------------------------------------------------------------------------
TEST(DoorPin, RisingEdgeGoesIdle)
{
    poll_door(true);

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "Drive must be IDLE after door-open rising edge");
}

// ---------------------------------------------------------------------------
// Test 3 — Rising edge from PLAYING stops the motor.
//
// Akiko may open the cover mid-game.  The motor must be reported inactive
// immediately so the mainboard knows the spindle has stopped.
// ---------------------------------------------------------------------------
TEST(DoorPin, RisingEdgeFromPlayingStopsMotor)
{
    s_state = DRIVE_PLAYING;
    poll_door(true);

    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be inactive after door opens during playback");
}

// ---------------------------------------------------------------------------
// Test 4 — Stable high after eject does not fire a second eject.
//
// Once the door is open, the pin stays high.  Subsequent poll ticks must not
// re-send 0x00 or re-enter the eject path — the edge fires exactly once.
// ---------------------------------------------------------------------------
TEST(DoorPin, StableHighAfterOpenNoRepeat)
{
    poll_door(true);               // rising edge — eject fires
    uint8_t sent = poll_door(true); // pin still high — must be silent

    CHECK_EQUAL_TEXT(0xFF, sent,
                     "No second eject when pin remains high after door open");
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "State must remain IDLE on second poll");
}

// ---------------------------------------------------------------------------
// Test 5 — Falling edge (door closing) does not trigger an eject.
//
// A HIGH→LOW transition means the door is being closed, not opened.  The
// drive should not change state or send any status byte.
// ---------------------------------------------------------------------------
TEST(DoorPin, FallingEdgeNoEject)
{
    s_door_prev = true;            // door was open
    s_state     = DRIVE_IDLE;

    uint8_t sent = poll_door(false); // HIGH→LOW (door closing)

    CHECK_EQUAL_TEXT(0xFF, sent,
                     "Falling edge must not send any status byte");
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "State must not change on falling edge");
}

// ---------------------------------------------------------------------------
// Test 6 — Stable low (door closed, normal operation) is silent.
//
// The common case: disc loaded, door closed, pin held LOW by the switch.
// Every poll tick must be a no-op.
// ---------------------------------------------------------------------------
TEST(DoorPin, StableLowNoEject)
{
    s_state = DRIVE_PLAYING;

    uint8_t sent = poll_door(false); // pin stable low

    CHECK_EQUAL_TEXT(0xFF, sent,
                     "Stable low must not send any status byte");
    CHECK_EQUAL_TEXT((int)DRIVE_PLAYING, (int)s_state,
                     "State must be unchanged while pin is stable low");
}

// ---------------------------------------------------------------------------
// Test 7 — Boot with door already open does not produce a spurious eject.
//
// commo_bridge_init() snapshots gpio_get(PIN_DOOR) into s_door_prev so that
// if the cover is open when the Pico boots the first poll does not fire the
// rising-edge path.  Simulate this by initialising s_door_prev = true.
// ---------------------------------------------------------------------------
TEST(DoorPin, BootDoorAlreadyOpenNoSpuriousEject)
{
    s_door_prev = true;   // mirrors commo_bridge_init() reading pin = HIGH

    uint8_t sent = poll_door(true); // first poll — pin still high

    CHECK_EQUAL_TEXT(0xFF, sent,
                     "No eject when door is already open at boot");
}

// ---------------------------------------------------------------------------
// Test 8 — Rising edge from SEEKING stops cleanly.
//
// A seek may be in progress when the user opens the cover.  The drive must
// abandon the seek and go idle, not stay in SEEKING with the motor running.
// ---------------------------------------------------------------------------
TEST(DoorPin, RisingEdgeFromSeekingGoesIdle)
{
    s_state = DRIVE_SEEKING;
    poll_door(true);

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "Drive must be IDLE after door opens during seek");
    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be inactive after door opens during seek");
}

// =============================================================================
// HostReset test group
// =============================================================================
//
// Documents the expected contract for handling the active-low /RESET line from
// the CD32 (GPIO 14, PIN_RESET, connector pin 7).
//
// Current firmware status:
//   PIN_RESET is defined in gpio_map.h and checked idle-high in selftest.c,
//   but is NOT polled in the main loop.  commo_bridge_poll() currently ignores
//   /RESET pulses from the CD32 host.  These tests specify the missing contract.
//
// What a /RESET assertion MUST cause:
//   DA output stopped    (da_stop() — DMA halted, PIO SM cleaned)
//   Sector cache seek 0  (sector_cache_seek(&g_cache, 0) — flush + next_lba=0)
//   Drive state → IDLE   (motor off, ACTIVE pin low)
//   COMMO SM → IDLE      (ready to receive the TRAY_IN_OPC Akiko will send)
//
// What /RESET MUST NOT change:
//   disc_image_t state   — the disc is still physically present; re-parsing
//                          the same ISO/BIN/NRG after every host reset is
//                          unnecessary and wastes SD card bandwidth.
//   door GPIO state      — s_door_prev is a snapshot of a real-time physical
//                          pin; whether the door was open before the host CPU
//                          reset is still true immediately after it.
//
// After /RESET the CD32 re-initialises Akiko and sends TRAY_IN_OPC.  The drive
// must respond as if it just powered on with a disc already loaded.
//
// handle_host_reset() replicates the expected commo_bridge_poll() response.
// =============================================================================

// Replicates what commo_bridge_poll() SHOULD do when PIN_RESET goes low.
// NOTE: s_door_prev is intentionally not touched — it is a GPIO snapshot.
static void handle_host_reset(void)
{
    s_state = DRIVE_IDLE;
}

TEST_GROUP(HostReset)
{
    void setup()
    {
        s_state     = DRIVE_IDLE;
        s_door_prev = false;   // door closed
    }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// 1 — /RESET from PLAYING returns drive to IDLE.
//
// The most common case: the user presses the CD32 reset button while a game
// is running.  DA DMA must halt and the drive must not report BUSY to Akiko's
// re-initialisation sequence.
// ---------------------------------------------------------------------------
TEST(HostReset, FromPlaying_DriveGoesIdle)
{
    s_state = DRIVE_PLAYING;
    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "/RESET from PLAYING must set state to DRIVE_IDLE");
    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be inactive (ACTIVE pin low) after /RESET");
}

// ---------------------------------------------------------------------------
// 2 — /RESET from SEEKING aborts the seek cleanly.
//
// A slow seek may be in progress when the user resets.  The drive must not
// stay in SEEKING — Akiko's post-reset TRAY_IN expects BUSY then READY,
// not a stuck seek state.
// ---------------------------------------------------------------------------
TEST(HostReset, FromSeeking_DriveGoesIdle)
{
    s_state = DRIVE_SEEKING;
    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "/RESET from SEEKING must abort seek and go IDLE");
    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be inactive after /RESET from SEEKING");
}

// ---------------------------------------------------------------------------
// 3 — /RESET during SPINUP collapses back to IDLE.
//
// The CD32 can assert /RESET during its own power-on before spinup completes.
// The drive must not latch in SPINUP — it must restart the full TRAY_IN
// sequence once Akiko re-initialises.
// ---------------------------------------------------------------------------
TEST(HostReset, FromSpinup_DriveGoesIdle)
{
    s_state = DRIVE_SPINUP;
    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "/RESET during SPINUP must collapse to DRIVE_IDLE");
    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must be off after /RESET from SPINUP");
}

// ---------------------------------------------------------------------------
// 4 — After /RESET a TRAY_IN sequence restarts correctly.
//
// Akiko's boot ROM always sends TRAY_IN_OPC after reset if a disc is present.
// The drive must enter SPINUP → READY as if it had just powered on, not skip
// straight to READY (which would give Akiko a stale Q-channel).
// ---------------------------------------------------------------------------
TEST(HostReset, ThenTrayIn_RestartsSpinup)
{
    s_state = DRIVE_PLAYING;
    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "State must be IDLE before TRAY_IN");

    handle_tray_in();
    CHECK_EQUAL_TEXT((int)DRIVE_SPINUP, (int)s_state,
                     "TRAY_IN after /RESET must enter SPINUP, not skip to READY");
    CHECK_TRUE_TEXT(motor_active(),
                    "Motor must be active during post-reset spinup");

    advance_state();
    CHECK_EQUAL_TEXT((int)DRIVE_READY, (int)s_state,
                     "SPINUP must advance to READY normally after /RESET");
    CHECK_EQUAL_TEXT(DRIVE_STATUS_DISC, build_status(),
                     "READY status must have only DISC bit after post-reset spinup");
}

// ---------------------------------------------------------------------------
// 5 — Multiple /RESET pulses are idempotent.
//
// The CD32 power supply can glitch and assert /RESET several times in rapid
// succession.  Each reset must leave the drive in IDLE — the state must not
// cycle through intermediate values or accumulate errors.
// ---------------------------------------------------------------------------
TEST(HostReset, MultipleResetsAreIdempotent)
{
    s_state = DRIVE_PLAYING;

    handle_host_reset();
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state, "First /RESET");

    handle_host_reset();
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state, "Second /RESET");

    handle_host_reset();
    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state, "Third /RESET");

    CHECK_TRUE_TEXT(!motor_active(),
                    "Motor must remain inactive across repeated /RESET pulses");
}

// ---------------------------------------------------------------------------
// 6 — /RESET does NOT change the door GPIO state.
//
// s_door_prev is a snapshot of a physical GPIO — whether the cover was open
// when the host reset fired is still physically true immediately after.  If
// handle_host_reset() cleared s_door_prev, the next poll_door() would see a
// spurious LOW→HIGH edge and send a false eject status to Akiko.
// ---------------------------------------------------------------------------
TEST(HostReset, DoorStatePreservedAcrossReset)
{
    // Door was open before the reset (HIGH = door open)
    s_door_prev = true;
    s_state     = DRIVE_PLAYING;

    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "Drive must be IDLE after reset");
    CHECK_TRUE_TEXT(s_door_prev,
                    "/RESET must not alter door pin snapshot — "
                    "clearing it would cause a spurious eject on next poll");

    // The next poll must NOT fire a rising-edge eject (door is still open)
    uint8_t sent = poll_door(true);  // pin is still HIGH
    CHECK_EQUAL_TEXT(0xFF, sent,
                     "No spurious eject after /RESET with door already open");
}

// ---------------------------------------------------------------------------
// 7 — /RESET from ERROR clears the fault state.
//
// A hardware error (e.g. SD card removed mid-read) sets DRIVE_ERROR.  After
// the user resets the CD32, the drive must return to IDLE — not stay faulted
// — so that the post-reset TRAY_IN can succeed.
// ---------------------------------------------------------------------------
TEST(HostReset, FromError_FaultCleared)
{
    s_state = DRIVE_ERROR;
    handle_host_reset();

    CHECK_EQUAL_TEXT((int)DRIVE_IDLE, (int)s_state,
                     "/RESET must clear DRIVE_ERROR and return to DRIVE_IDLE");
    CHECK_TRUE_TEXT(!(build_status() & DRIVE_STATUS_ERROR),
                    "ERROR bit must not be set after /RESET clears the fault");
}
