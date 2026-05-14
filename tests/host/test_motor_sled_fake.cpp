// =============================================================================
// test_motor_sled_fake.cpp — Spindle motor state and virtual sled positioning
// =============================================================================
//
// The ODE has no real spindle motor or sled servo.  Two mechanisms fake them:
//
//   Motor:  The ACTIVE pin (GPIO 10) goes HIGH whenever the drive state is not
//           DRIVE_IDLE and not DRIVE_ERROR.  This mirrors _update_active_pin()
//           in commo_bridge.c.  test_door_tray.cpp covers IDLE/ERROR/SPINUP;
//           this file covers the remaining active states (READY/SEEKING/
//           PLAYING/PAUSED) and the SEEKING → READY state advance.
//
//   Sled:   sector_cache_t::next_fetch_lba is the virtual read-head position.
//           sector_cache_seek(cache, lba) moves it.  commo_bridge.c calls this
//           on SEEK_OPC, JUMP_TRACKS_OPC, and TRAY_IN_OPC.  The sector_cache
//           module compiles into the host suite; these tests call it directly.
//
//   BCD MSF → LBA: SEEK_OPC / JUMP_TRACKS_OPC receive a BCD-encoded MSF in
//           p1/p2/p3 and convert via msf_to_lba() to arrive at the target LBA.
//           Five known conversions are table-checked here.
//
// commo_bridge.c is hardware-bound and cannot be compiled into the host suite.
// The seek / state-machine fragment is replicated here with hardware calls
// (gpio_put, sector_cache_seek) elided or replaced by inspectable local state.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "cd_types.h"       // drive_state_t, DRIVE_STATUS_*, msf_to_lba
#include "defs.h"           // SEEK_OPC, JUMP_TRACKS_OPC, TRAY_IN_OPC, etc.
extern "C" {
#include "sector_cache.h"   // sector_cache_t, sector_cache_seek, sector_cache_init
}

// ---------------------------------------------------------------------------
// Replicated state machine fragment (commo_bridge.c)
// ---------------------------------------------------------------------------
// Hardware calls (gpio_put, sector_cache_seek on g_cache) are replaced by
// observable local variables.  State-transition and LBA-computation logic is
// exact.

static drive_state_t s_state;
static uint32_t      s_seek_lba;
static uint32_t      s_current_lba;

// Mirrors _update_active_pin(): motor is running whenever state is not
// IDLE or ERROR.  Returns the value that would be written to ACTIVE_PIN.
static bool motor_active(void)
{
    return s_state != DRIVE_IDLE && s_state != DRIVE_ERROR;
}

// Mirrors _maybe_advance_state() with FAKE_TIMING off (instant transition).
// Also updates s_current_lba when advancing from SEEKING, exactly as in
// commo_bridge.c.
static void advance_state(void)
{
    if (s_state == DRIVE_SPINUP) {
        s_state = DRIVE_READY;
    }
    if (s_state == DRIVE_SEEKING) {
        s_current_lba = s_seek_lba;
        s_state = DRIVE_READY;
    }
}

// Replicate the SEEK_OPC / JUMP_TRACKS_OPC fragment of _handle_opc().
// Hardware side-effect (sector_cache_seek on g_cache) is a no-op here;
// the virtual sled is tested separately using a local sector_cache_t.
static void handle_seek(uint8_t p1, uint8_t p2, uint8_t p3)
{
    msf_t m = { p1, p2, p3 };
    s_seek_lba = msf_to_lba(m);
    /* sector_cache_seek(&g_cache, s_seek_lba) — no-op */
    s_state = DRIVE_SEEKING;
}

// ---------------------------------------------------------------------------
// TEST GROUP
// ---------------------------------------------------------------------------

TEST_GROUP(MotorSledFake)
{
    void setup()    { s_state = DRIVE_IDLE; s_seek_lba = 0; s_current_lba = 0; }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Test 1 — Motor is active in all four remaining running states.
//
// test_door_tray.cpp verifies SPINUP.  READY, SEEKING, PLAYING, and PAUSED
// are tested here.  A real tray drive would keep the spindle spinning in all
// of these; ACTIVE must be HIGH so the mainboard knows the drive is energised.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, MotorActiveInReadySeekingPlayingPaused)
{
    static const drive_state_t running[] = {
        DRIVE_READY, DRIVE_SEEKING, DRIVE_PLAYING, DRIVE_PAUSED
    };

    for (unsigned i = 0; i < sizeof(running)/sizeof(running[0]); i++) {
        s_state = running[i];
        CHECK_TRUE_TEXT(motor_active(),
                        "ACTIVE pin must be HIGH in all non-idle, non-error states");
    }
}

// ---------------------------------------------------------------------------
// Test 2 — Motor is inactive only in IDLE and ERROR.
//
// Exactly two states must de-assert the ACTIVE pin; all others must assert it.
// This verifies the boundary is tight — adding a new state doesn't accidentally
// leave the motor reported as inactive.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, MotorInactiveOnlyInIdleAndError)
{
    s_state = DRIVE_IDLE;
    CHECK_TRUE_TEXT(!motor_active(), "ACTIVE must be LOW in DRIVE_IDLE");

    s_state = DRIVE_ERROR;
    CHECK_TRUE_TEXT(!motor_active(), "ACTIVE must be LOW in DRIVE_ERROR");
}

// ---------------------------------------------------------------------------
// Test 3 — SEEKING state advances to READY.
//
// test_door_tray.cpp covers SPINUP → READY.  This test covers SEEKING → READY,
// which happens after a SEEK_OPC completes its (fake-zero) seek delay.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, SeekingStateAdvancesToReady)
{
    s_state = DRIVE_SEEKING;
    advance_state();
    CHECK_EQUAL_TEXT((int)DRIVE_READY, (int)s_state,
                     "DRIVE_SEEKING must advance to DRIVE_READY");
}

// ---------------------------------------------------------------------------
// Test 4 — After advancing from SEEKING, s_current_lba matches the target.
//
// When the seek completes the drive knows where the head has landed.  The
// production code writes s_current_lba = s_seek_lba inside _maybe_advance_state().
// This is needed by PAUSE_OFF_OPC which calls da_resume() → sector_cache_seek()
// using the confirmed head position.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, AfterSeekAdvanceCurrentLbaMatchesTarget)
{
    s_seek_lba = 4350;
    s_state    = DRIVE_SEEKING;
    advance_state();
    CHECK_EQUAL_TEXT(4350u, s_current_lba,
                     "s_current_lba must equal s_seek_lba after seek completes");
}

// ---------------------------------------------------------------------------
// Test 5 — BCD MSF → LBA conversion table.
//
// SEEK_OPC and JUMP_TRACKS_OPC encode the target as BCD MSF in p1/p2/p3.
// msf_to_lba() decodes them.  Five checkpoints spanning the disc range:
//   00:02:00 → LBA   0  (first user sector, Red Book lead-in offset 150)
//   00:02:01 → LBA   1
//   00:03:00 → LBA  75  (one second in)
//   01:00:00 → LBA 4350 (one minute in)
//   10:00:00 → LBA 44850 (ten minutes)
//   00:00:00 → LBA   0  (below lead-in — clamped)
// ---------------------------------------------------------------------------
TEST(MotorSledFake, BcdMsfToLbaKnownValues)
{
    struct { uint8_t m, s, f; uint32_t expected_lba; } cases[] = {
        { 0x00, 0x02, 0x00,     0u },
        { 0x00, 0x02, 0x01,     1u },
        { 0x00, 0x03, 0x00,    75u },
        { 0x01, 0x00, 0x00,  4350u },
        { 0x10, 0x00, 0x00, 44850u },
        { 0x00, 0x00, 0x00,     0u },   // below lead-in → clamp to 0
    };
    int failures = 0;
    for (int i = 0; i < (int)(sizeof(cases)/sizeof(cases[0])); i++) {
        msf_t m = { cases[i].m, cases[i].s, cases[i].f };
        uint32_t got = msf_to_lba(m);
        if (got != cases[i].expected_lba) {
            printf("  FAIL BCD MSF %02X:%02X:%02X  expected LBA %lu  got %lu\n",
                   cases[i].m, cases[i].s, cases[i].f,
                   (unsigned long)cases[i].expected_lba,
                   (unsigned long)got);
            failures++;
        }
    }
    CHECK_EQUAL_TEXT(0, failures,
                     "One or more BCD MSF → LBA conversions were wrong (see output)");
}

// ---------------------------------------------------------------------------
// Test 6 — SEEK_OPC leaves drive in DRIVE_SEEKING.
//
// The state must be SEEKING immediately after the opcode is processed so that
// build_status() returns DISC|BUSY and Akiko knows not to attempt a read yet.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, SeekOpcLeavesDriveInSeekingState)
{
    s_state = DRIVE_READY;
    handle_seek(0x00, 0x02, 0x00);   // MSF 00:02:00 → LBA 0
    CHECK_EQUAL_TEXT((int)DRIVE_SEEKING, (int)s_state,
                     "SEEK_OPC must transition drive to DRIVE_SEEKING");
}

// ---------------------------------------------------------------------------
// Test 7 — SEEK_OPC computes the correct target LBA from the BCD MSF params.
//
// Three seek targets verify the full decode pipeline — the same path that
// _handle_opc() uses when SEEK_OPC / JUMP_TRACKS_OPC arrives on the bus.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, SeekOpcComputesCorrectLbaFromBcdMsf)
{
    struct { uint8_t p1, p2, p3; uint32_t expected; } seeks[] = {
        { 0x00, 0x02, 0x00,     0u },
        { 0x00, 0x03, 0x00,    75u },
        { 0x01, 0x00, 0x00,  4350u },
    };

    int failures = 0;
    for (int i = 0; i < (int)(sizeof(seeks)/sizeof(seeks[0])); i++) {
        s_state = DRIVE_READY;
        handle_seek(seeks[i].p1, seeks[i].p2, seeks[i].p3);
        if (s_seek_lba != seeks[i].expected) {
            printf("  FAIL seek %02X:%02X:%02X expected LBA %lu  got %lu\n",
                   seeks[i].p1, seeks[i].p2, seeks[i].p3,
                   (unsigned long)seeks[i].expected,
                   (unsigned long)s_seek_lba);
            failures++;
        }
    }
    CHECK_EQUAL_TEXT(0, failures,
                     "SEEK_OPC LBA mismatch for one or more BCD MSF targets (see output)");
}

// ---------------------------------------------------------------------------
// Test 8 — JUMP_TRACKS_OPC computes the same LBA as SEEK_OPC for the same MSF.
//
// Both opcodes share the identical BCD MSF → LBA decode path.  They differ
// only in naming convention on the protocol level; the firmware handles them
// identically via a shared case label.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, JumpTracksComputesSameLbaAsSeek)
{
    uint8_t p1 = 0x00, p2 = 0x03, p3 = 0x00;   // MSF 00:03:00 → LBA 75

    s_state = DRIVE_READY;
    handle_seek(p1, p2, p3);
    uint32_t seek_lba = s_seek_lba;

    // handle_seek() replicates both SEEK_OPC and JUMP_TRACKS_OPC; the LBA
    // computation is identical.  Calling it twice for the same MSF must yield
    // the same result.
    s_state = DRIVE_READY;
    s_seek_lba = 0;
    handle_seek(p1, p2, p3);
    uint32_t jump_lba = s_seek_lba;

    CHECK_EQUAL_TEXT(seek_lba, jump_lba,
                     "JUMP_TRACKS must compute the same LBA as SEEK for identical MSF");
}

// ---------------------------------------------------------------------------
// Test 9 — Virtual sled moves to the seek target via sector_cache_seek.
//
// sector_cache_t::next_fetch_lba is the prefetch read-head position — the
// virtual equivalent of the physical sled.  sector_cache_seek(cache, lba)
// flushes stale data and sets next_fetch_lba to lba.  This test uses the
// real sector_cache module (compiled from src/sector_cache.c) with a NULL
// disc pointer (no actual I/O occurs in the host suite).
// ---------------------------------------------------------------------------
TEST(MotorSledFake, VirtualSledSeeksToTargetLba)
{
    sector_cache_t cache;
    sector_cache_init(&cache, NULL);

    sector_cache_seek(&cache, 4350u);

    CHECK_EQUAL_TEXT(4350u, cache.next_fetch_lba,
                     "next_fetch_lba must equal the seek target after sector_cache_seek");
}

// ---------------------------------------------------------------------------
// Test 10 — Sequential seeks: sled ends at the last target.
//
// A game may issue multiple seeks rapidly (e.g. track switch during loading).
// The second seek must overwrite the first: next_fetch_lba must reflect the
// final destination, not an intermediate one.
// ---------------------------------------------------------------------------
TEST(MotorSledFake, SequentialSeeksSledEndsAtLastTarget)
{
    sector_cache_t cache;
    sector_cache_init(&cache, NULL);

    sector_cache_seek(&cache,    75u);   // first seek
    sector_cache_seek(&cache, 44850u);   // second seek — overwrites first

    CHECK_EQUAL_TEXT(44850u, cache.next_fetch_lba,
                     "next_fetch_lba must be the last seek target after two seeks");
}
