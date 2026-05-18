// =============================================================================
// test_subcode.cpp — Q-channel subcode generation and subcode clock tests
//
// Subcode group: verify that subcode_build_q_position/mcn/isrc produce
//   byte-exact output matching the Red Book (ECMA-130) Q-channel spec.
//
// SubcodePio group: verify subcode_push_to_pio() PIO FIFO back-pressure.
//
// SubcodeClk group: verify subcode_pulse_sector_clocks() drives SUB_SCOR
//   (GPIO 8) and SUB_WFCLK (GPIO 7) without disturbing SUB_DATA/SUB_CLK.
//   These pulses are required by Akiko to frame-synchronise the subcode
//   bitstream delivered on SUB_DATA + SUB_CLK (see FINDING-20 in third_pass.md).
//
// Key invariants:
//   - CTRL/ADR byte 0: data track = 0x41, audio track = 0x01.
//   - CRC-16/CCITT (init=0x0000, poly=0x1021) over bytes 0-9, bit-inverted.
//   - Relative time: BCD frames since INDEX 01, NO lead-in offset.
//   - Absolute time: BCD MSF including 150-frame lead-in offset.
//   - Track and index fields (bytes 1-2) encoded as BCD.
//   - Reserved byte 6 always zero in position mode.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "subcode.h"
#include "cd_types.h"
}

// PIO FIFO stub state — definitions satisfy the extern declarations in
// hardware/pio.h.  Only this TU calls subcode_push_to_pio, so no other TU
// generates a reference to these globals.
int g_stub_pio_put_count       = 0;
int g_stub_pio_fifo_full_after = -1;

TEST_GROUP(Subcode) {};

/* -------------------------------------------------------------------------
 * CRC-16/CCITT known values
 * The Q-channel CRC is computed over 10 bytes with init=0x0000 then
 * bitwise-inverted before storage.
 * ---------------------------------------------------------------------- */

/* All-zero 10-byte input.  CRC of all-zeros with init 0 = 0x0000, inverted = 0xFFFF. */
TEST(Subcode, Crc16AllZerosIsFFFF)
{
    uint8_t data[10] = {0};
    LONGS_EQUAL(0xFFFF, (long)subcode_crc16(data, 10));
}

/* Single 0xFF byte: pre-computed expected result. */
TEST(Subcode, Crc16SingleFF)
{
    uint8_t data[1] = { 0xFF };
    /* CRC-16/CCITT (init=0, no final invert) of 0xFF = 0x1EF0; inverted = 0xE10F */
    LONGS_EQUAL(0xE10F, (long)subcode_crc16(data, 1));
}

/* CRC must differ for different inputs. */
TEST(Subcode, Crc16DiffersForDifferentInputs)
{
    uint8_t a[10] = {0}, b[10] = {0};
    b[5] = 0x01;
    CHECK_TRUE(subcode_crc16(a, 10) != subcode_crc16(b, 10));
}

/* -------------------------------------------------------------------------
 * CONAD byte (byte 0) from subcode_build_q_position
 * ---------------------------------------------------------------------- */

/* Audio track: CTRL nibble 0x0, ADR nibble 0x1 → byte 0 = 0x01 */
TEST(Subcode, ConadByteAudioTrack)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x01, buf[0]);
}

/* Data track: CTRL nibble 0x4, ADR nibble 0x1 → byte 0 = 0x41 */
TEST(Subcode, ConadByteDataTrack)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, true, 0, 0, buf);
    BYTES_EQUAL(0x41, buf[0]);
}

/* -------------------------------------------------------------------------
 * BCD track encoding in byte 1
 * ---------------------------------------------------------------------- */

TEST(Subcode, BcdTrack1Is0x01)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x01, buf[1]);
}

TEST(Subcode, BcdTrack10Is0x10)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(10, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x10, buf[1]);
}

TEST(Subcode, BcdTrack99Is0x99)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(99, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x99, buf[1]);
}

/* -------------------------------------------------------------------------
 * Absolute time fields (bytes 7-9) at well-known LBAs
 * ---------------------------------------------------------------------- */

/* LBA 0 → absolute MSF 00:02:00 */
TEST(Subcode, AbsTimeAtLba0)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x00, buf[7]);  /* minute */
    BYTES_EQUAL(0x02, buf[8]);  /* second */
    BYTES_EQUAL(0x00, buf[9]);  /* frame  */
}

/* LBA 75 → absolute MSF 00:03:00 */
TEST(Subcode, AbsTimeAtLba75)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, buf);
    BYTES_EQUAL(0x00, buf[7]);
    BYTES_EQUAL(0x03, buf[8]);
    BYTES_EQUAL(0x00, buf[9]);
}

/* LBA 150 → absolute MSF 00:04:00 */
TEST(Subcode, AbsTimeAtLba150)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 150, buf);
    BYTES_EQUAL(0x00, buf[7]);
    BYTES_EQUAL(0x04, buf[8]);
    BYTES_EQUAL(0x00, buf[9]);
}

/* -------------------------------------------------------------------------
 * Reserved byte 6 must always be zero in Mode 1 (Position)
 * ---------------------------------------------------------------------- */

TEST(Subcode, ReservedByte6IsZero)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(5, 1, true, 100, 200, buf);
    BYTES_EQUAL(0x00, buf[6]);
}

/* -------------------------------------------------------------------------
 * CRC self-consistency for generated packets
 * ---------------------------------------------------------------------- */

/* Build a packet and verify that the CRC in bytes 10-11 matches
   a fresh recomputation over bytes 0-9 with final inversion. */
TEST(Subcode, CrcSelfConsistentPosition)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(3, 1, false, 0, 300, buf);

    /* The stored CRC was computed then inverted.  Re-invert before comparing. */
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    /* To verify: ~stored should equal the un-inverted CRC of bytes 0-9 */
    /* But subcode_crc16 already inverts, so compare directly: */
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

TEST(Subcode, CrcSelfConsistentMcn)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn("1234567890123", buf);
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

TEST(Subcode, CrcSelfConsistentIsrc)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_isrc(1, false, "GBAYE9200001", buf);
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

/* -------------------------------------------------------------------------
 * subcode_append_crc writes correct bytes
 * ---------------------------------------------------------------------- */

TEST(Subcode, AppendCrcWritesCorrectBytes)
{
    uint8_t buf[QCHANNEL_SIZE];
    memset(buf, 0, sizeof(buf));
    /* Set some data in bytes 0-9 */
    buf[0] = 0x41; buf[1] = 0x01; buf[2] = 0x01;
    subcode_append_crc(buf);
    /* Verify stored bytes match recomputed CRC */
    uint16_t crc = subcode_crc16(buf, 10);
    BYTES_EQUAL((crc >> 8) & 0xFF, buf[10]);
    BYTES_EQUAL(crc & 0xFF, buf[11]);
}

/* -------------------------------------------------------------------------
 * MCN null pointer produces all-zero MCN bytes with valid CRC
 * ---------------------------------------------------------------------- */

TEST(Subcode, McnNullProducesZeroMcn)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn(NULL, buf);
    /* ADR nibble should be Q_ADR_MCN = 0x02 */
    BYTES_EQUAL(Q_ADR_MCN, buf[0] & 0x0F);
    /* MCN bytes 1-7 should all be zero */
    for (int i = 1; i <= 7; i++) {
        BYTES_EQUAL(0x00, buf[i]);
    }
}

/* -------------------------------------------------------------------------
 * Relative time fields (bytes 3-5) — BUG-1 regression tests
 *
 * Before the fix, lba_to_msf() was used to encode relative time, which
 * incorrectly added the 150-frame (2-second) lead-in offset.
 * These tests lock in the correct behaviour: direct BCD from the LBA delta,
 * no lead-in offset applied.
 * ---------------------------------------------------------------------- */

/* At the track start point (disc_lba == track_start_lba), elapsed frames = 0.
   Relative time must be 00:00:00, not 00:02:00 as lba_to_msf would give. */
TEST(Subcode, RelTimeAtTrackStart_IsZero)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150, 150, buf);
    BYTES_EQUAL(0x00, buf[3]);  /* relative minute */
    BYTES_EQUAL(0x00, buf[4]);  /* relative second */
    BYTES_EQUAL(0x00, buf[5]);  /* relative frame  */
}

/* 75 frames past the track start = 1 second elapsed → 00:01:00 */
TEST(Subcode, RelTimeAfter75Frames_IsOneSecond)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, buf);
    BYTES_EQUAL(0x00, buf[3]);  /* 0 minutes */
    BYTES_EQUAL(0x01, buf[4]);  /* 1 second  */
    BYTES_EQUAL(0x00, buf[5]);  /* 0 frames  */
}

/* 3600 frames = 48 seconds = 00:48:00 (BCD 0x48 in byte 4) */
TEST(Subcode, RelTimeAfter3600Frames_Is48Seconds)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(2, 1, false, 0, 3600, buf);
    BYTES_EQUAL(0x00, buf[3]);  /* 0 minutes */
    BYTES_EQUAL(0x48, buf[4]);  /* 48 seconds BCD */
    BYTES_EQUAL(0x00, buf[5]);  /* 0 frames  */
}

/* Pregap (index==0, disc_lba < track_start_lba): relative time counts
   DOWN from track_start - disc_lba toward 00:00:00.
   Here track_start=300, disc_lba=225 → rel_lba=75 → 00:01:00 */
TEST(Subcode, RelTimePregapCountdown)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(2, 0, false, 300, 225, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x01, buf[4]);
    BYTES_EQUAL(0x00, buf[5]);
}

/* Absolute time (bytes 7-9) must still use lba_to_msf() with the 150-frame
   lead-in offset — the BUG-1 fix must NOT have broken this.
   LBA 150 → lba_to_msf adds 150 → total 300 frames → 00:04:00 */
TEST(Subcode, AbsTimeUnaffectedByRelTimeFix)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150, 150, buf);
    BYTES_EQUAL(0x00, buf[7]);  /* absolute minute */
    BYTES_EQUAL(0x04, buf[8]);  /* absolute second — 300/75 = 4 */
    BYTES_EQUAL(0x00, buf[9]);  /* absolute frame  */
}

/* =========================================================================
 * SubcodePio — Scenario 8: PIO TX FIFO back-pressure via subcode_push_to_pio
 *
 * subcode_push_to_pio() packs 12 Q-channel bytes into 3 × 32-bit words and
 * checks pio_sm_is_tx_fifo_full() before each push.  It returns false (and
 * stops pushing) the moment the FIFO is full, mirroring real PIO back-pressure.
 *
 * The stub FIFO is controlled by g_stub_pio_fifo_full_after:
 *   -1 → never full (normal path)
 *   0  → already full before the first word (immediate rejection)
 *   N  → full after N words have been pushed
 * ======================================================================= */

TEST_GROUP(SubcodePio)
{
    uint8_t qbuf[QCHANNEL_SIZE];

    void setup() {
        g_stub_pio_put_count       = 0;
        g_stub_pio_fifo_full_after = -1;   // -1 = never full
        subcode_build_q_position(1, 1, false, 150u, 225u, qbuf);
    }
};

/* Normal path: FIFO never full → all 3 words pushed, returns true. */
TEST(SubcodePio, PushAllThreeWords_Succeeds)
{
    CHECK_TRUE(subcode_push_to_pio(pio0, 0, qbuf));
    LONGS_EQUAL(3, g_stub_pio_put_count);
}

/* Scenario 8a: FIFO is already full before the first word.
 * subcode_push_to_pio must return false immediately with 0 words pushed. */
TEST(SubcodePio, FullFifo_FirstWordRejected)
{
    g_stub_pio_fifo_full_after = 0;   // full before any push
    CHECK_FALSE(subcode_push_to_pio(pio0, 0, qbuf));
    LONGS_EQUAL(0, g_stub_pio_put_count);
}

/* Scenario 8b: FIFO fills after 2 words (simulates 4-word FIFO with 2 words
 * already occupied).  The third push is blocked; function returns false. */
TEST(SubcodePio, PartiallyFull_ThirdWordBlocked)
{
    g_stub_pio_fifo_full_after = 2;   // full after 2 pushes
    CHECK_FALSE(subcode_push_to_pio(pio0, 0, qbuf));
    LONGS_EQUAL(2, g_stub_pio_put_count);   // exactly 2 words made it in
}

/* =========================================================================
 * SubcodeClk — FINDING-20: verify subcode_pulse_sector_clocks() drives
 *              SUB_SCOR (GPIO 8) and SUB_WFCLK (GPIO 7) correctly and does
 *              not disturb SUB_DATA (GPIO 5) or SUB_CLK (GPIO 6).
 *
 * gpio_levels[] is a static array in the hardware/gpio.h stub.
 * subcode_pulse_sector_clocks() is a static inline in subcode.h.
 * When inlined into this TU it writes to this TU's gpio_levels[], which
 * the tests below can inspect directly.
 * ======================================================================= */

TEST_GROUP(SubcodeClk)
{
    void setup() {
        gpio_init(PIN_SUB_DATA);
        gpio_init(PIN_SUB_CLK);
        gpio_init(PIN_SUB_WFCLK);
        gpio_init(PIN_SUB_SCOR);
    }
};

/* Both pins idle low after pulse — confirms the function completed the
 * high-then-low sequence and did not leave either pin stuck high. */
TEST(SubcodeClk, ScorAndWfclkIdleLowAfterPulse)
{
    subcode_pulse_sector_clocks();
    LONGS_EQUAL(0, (long)gpio_levels[PIN_SUB_SCOR]);
    LONGS_EQUAL(0, (long)gpio_levels[PIN_SUB_WFCLK]);
}

/* Start both pins HIGH (as if a previous operation left them asserted).
 * After pulse() both must be LOW — proves the function drove them, not just
 * left them in their initial state. */
TEST(SubcodeClk, ScorAndWfclkReturnToLowFromHighState)
{
    gpio_put(PIN_SUB_SCOR,  1);
    gpio_put(PIN_SUB_WFCLK, 1);
    subcode_pulse_sector_clocks();
    LONGS_EQUAL(0, (long)gpio_levels[PIN_SUB_SCOR]);
    LONGS_EQUAL(0, (long)gpio_levels[PIN_SUB_WFCLK]);
}

/* SUB_DATA pin (GPIO 5) must not be touched by subcode_pulse_sector_clocks(). */
TEST(SubcodeClk, SubDataPinUnchangedByPulse)
{
    gpio_put(PIN_SUB_DATA, 1);
    subcode_pulse_sector_clocks();
    LONGS_EQUAL(1, (long)gpio_levels[PIN_SUB_DATA]);
}

/* SUB_CLK pin (GPIO 6) must not be touched by subcode_pulse_sector_clocks(). */
TEST(SubcodeClk, SubClkPinUnchangedByPulse)
{
    gpio_put(PIN_SUB_CLK, 1);
    subcode_pulse_sector_clocks();
    LONGS_EQUAL(1, (long)gpio_levels[PIN_SUB_CLK]);
}

/* SCOR and WFCLK are distinct GPIO numbers from each other and from DATA/CLK. */
TEST(SubcodeClk, SubcodePinsMutuallyDistinct)
{
    CHECK_TRUE(PIN_SUB_SCOR  != PIN_SUB_DATA);
    CHECK_TRUE(PIN_SUB_SCOR  != PIN_SUB_CLK);
    CHECK_TRUE(PIN_SUB_SCOR  != PIN_SUB_WFCLK);
    CHECK_TRUE(PIN_SUB_WFCLK != PIN_SUB_DATA);
    CHECK_TRUE(PIN_SUB_WFCLK != PIN_SUB_CLK);
}

/* Integration: pulse then push a full Q-channel block — all 3 FIFO words
 * delivered (confirms the two operations are independent and compose). */
TEST(SubcodeClk, PulseBeforePush_AllThreeWordsDelivered)
{
    g_stub_pio_put_count       = 0;
    g_stub_pio_fifo_full_after = -1;
    uint8_t qbuf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, qbuf);
    subcode_pulse_sector_clocks();
    CHECK_TRUE(subcode_push_to_pio(pio0, 0, qbuf));
    LONGS_EQUAL(3, g_stub_pio_put_count);
}

/* =========================================================================
 * Additional Subcode invariants (Gaps 6–10 from test-gap audit)
 * ======================================================================= */

/* Gap 6: MCN digit packing — "1234567890123" packs into bytes 1-7 as paired
 * BCD nibbles MSB-first; the 13th digit occupies the high nibble of byte 7
 * with the low nibble zero-padded. */
TEST(Subcode, McnDigitPacking_ByteValues)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn("1234567890123", buf);
    BYTES_EQUAL(0x12, buf[1]);   /* digits  1+2  */
    BYTES_EQUAL(0x34, buf[2]);   /* digits  3+4  */
    BYTES_EQUAL(0x56, buf[3]);   /* digits  5+6  */
    BYTES_EQUAL(0x78, buf[4]);   /* digits  7+8  */
    BYTES_EQUAL(0x90, buf[5]);   /* digits  9+0  */
    BYTES_EQUAL(0x12, buf[6]);   /* digits  1+2  */
    BYTES_EQUAL(0x30, buf[7]);   /* digit   3 hi nibble, zero lo nibble */
}

/* Gap 7: MCN byte 0 — CTRL nibble = Q_CTRL_DATA (0x4), ADR = Q_ADR_MCN (0x2)
 * → combined byte must be 0x42.  Previously only the ADR nibble was checked. */
TEST(Subcode, McnCtrlAdrByte_Is0x42)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn("1234567890123", buf);
    BYTES_EQUAL(0x42, buf[0]);
}

/* Gap 8: ISRC null pointer → bytes 1-9 must all be zero. */
TEST(Subcode, IsrcNull_BytesOneToNineAreZero)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_isrc(1, false, NULL, buf);
    for (int i = 1; i <= 9; i++) {
        BYTES_EQUAL(0x00, buf[i]);
    }
}

/* Gap 9: ISRC byte 0 CTRL/ADR format.
 * Data track:  (Q_CTRL_DATA  << 4) | Q_ADR_ISRC = 0x43
 * Audio track: (Q_CTRL_AUDIO << 4) | Q_ADR_ISRC = 0x03 */
TEST(Subcode, IsrcCtrlAdrByte_DataTrack_Is0x43)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_isrc(1, true, NULL, buf);
    BYTES_EQUAL(0x43, buf[0]);
}

TEST(Subcode, IsrcCtrlAdrByte_AudioTrack_Is0x03)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_isrc(1, false, NULL, buf);
    BYTES_EQUAL(0x03, buf[0]);
}

/* Gap 10: relative time at the second rollover boundary.
 * frame 74 stays within the same second (0m 0s 74f → BCD 0x74 in buf[5]).
 * frame 75 increments the second counter    (0m 1s 0f  → BCD 0x01 in buf[4]). */
TEST(Subcode, RelTime_Frame74_Is0m0s74f)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 74, buf);
    BYTES_EQUAL(0x00, buf[3]);   /* 0 minutes */
    BYTES_EQUAL(0x00, buf[4]);   /* 0 seconds */
    BYTES_EQUAL(0x74, buf[5]);   /* 74 frames BCD */
}

TEST(Subcode, RelTime_Frame75_Is0m1s0f)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, buf);
    BYTES_EQUAL(0x00, buf[3]);   /* 0 minutes */
    BYTES_EQUAL(0x01, buf[4]);   /* 1 second  */
    BYTES_EQUAL(0x00, buf[5]);   /* 0 frames  */
}
