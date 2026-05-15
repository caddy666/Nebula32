// =============================================================================
// test_lba_msf.cpp — LBA ↔ MSF conversion and BCD encoding tests
//
// Intent: verify the lba_to_msf() / msf_to_lba() functions in cd_types.h
// and that all callers get the Red Book lead-in offset right.
//
// Key invariants under test:
//   - lba_to_msf() adds the 150-frame (2-second) lead-in offset before
//     converting: LBA 0 → 00:02:00, LBA 75 → 00:03:00, LBA 150 → 00:04:00.
//   - msf_to_lba() is the exact inverse: subtracts 150 frames before returning.
//   - Round-trip: msf_to_lba(lba_to_msf(n)) == n for all valid LBAs.
//   - BCD encoding: minute/second/frame fields are stored as packed BCD
//     (high nibble = tens digit, low nibble = units digit).
//   - LBA 0 produces BCD 0x00, 0x02, 0x00 — not 0x00, 0x00, 0x00.
// =============================================================================

#include <CppUTest/TestHarness.h>
extern "C" {
#include "cd_types.h"
}

TEST_GROUP(LbaMsf) {};

/* LBA 0 → total = 150 = 0m 2s 0f → BCD 00:02:00 */
TEST(LbaMsf, Lba0IsMsf000200)
{
    msf_t m = lba_to_msf(0);
    BYTES_EQUAL(0x00, m.minute);
    BYTES_EQUAL(0x02, m.second);
    BYTES_EQUAL(0x00, m.frame);
}

/* LBA 75 → total = 225 = 0m 3s 0f → BCD 00:03:00 */
TEST(LbaMsf, Lba75IsMsf000300)
{
    msf_t m = lba_to_msf(75);
    BYTES_EQUAL(0x00, m.minute);
    BYTES_EQUAL(0x03, m.second);
    BYTES_EQUAL(0x00, m.frame);
}

/* LBA 150 → total = 300 = 0m 4s 0f → BCD 00:04:00 */
TEST(LbaMsf, Lba150IsMsf000400)
{
    msf_t m = lba_to_msf(150);
    BYTES_EQUAL(0x00, m.minute);
    BYTES_EQUAL(0x04, m.second);
    BYTES_EQUAL(0x00, m.frame);
}

/* LBA 149 → total = 299 = 0m 3s 74f → BCD 00:03:74 */
TEST(LbaMsf, Lba149IsMsf000374)
{
    msf_t m = lba_to_msf(149);
    BYTES_EQUAL(0x00, m.minute);
    BYTES_EQUAL(0x03, m.second);
    BYTES_EQUAL(0x74, m.frame);   /* 74 decimal = 0x74 in BCD */
}

/* LBA 4350 → total = 4500 = 1m 0s 0f → BCD 01:00:00 */
TEST(LbaMsf, Lba4350IsMsf010000)
{
    msf_t m = lba_to_msf(4350);
    BYTES_EQUAL(0x01, m.minute);
    BYTES_EQUAL(0x00, m.second);
    BYTES_EQUAL(0x00, m.frame);
}

/* LBA 600 → total = 750 = 0m 10s 0f → BCD second should be 0x10 (not 10) */
TEST(LbaMsf, BcdEncodingSecond10)
{
    msf_t m = lba_to_msf(600);
    BYTES_EQUAL(0x00, m.minute);
    BYTES_EQUAL(0x10, m.second);   /* 10 decimal BCD-encoded = 0x10 */
    BYTES_EQUAL(0x00, m.frame);
}

/* LBA 44850 → total = 45000 = 10m 0s 0f → BCD minute = 0x10 */
TEST(LbaMsf, BcdEncodingMinute10)
{
    msf_t m = lba_to_msf(44850);
    BYTES_EQUAL(0x10, m.minute);   /* 10 decimal BCD-encoded = 0x10 */
    BYTES_EQUAL(0x00, m.second);
    BYTES_EQUAL(0x00, m.frame);
}

TEST(LbaMsf, RoundTripZero)
{
    LONGS_EQUAL(0, (long)msf_to_lba(lba_to_msf(0)));
}

TEST(LbaMsf, RoundTripMany)
{
    uint32_t lbas[] = { 1, 74, 75, 150, 299, 1000, 4350, 10000, 44850 };
    for (size_t i = 0; i < sizeof(lbas)/sizeof(lbas[0]); i++) {
        LONGS_EQUAL((long)lbas[i], (long)msf_to_lba(lba_to_msf(lbas[i])));
    }
}

/* MSF 00:02:00 → LBA 0 */
TEST(LbaMsf, MsfToLbaKnownValue)
{
    msf_t m;
    m.minute = 0x00;
    m.second = 0x02;
    m.frame  = 0x00;
    LONGS_EQUAL(0, (long)msf_to_lba(m));
}

/* MSF 00:01:00 → total = 75, 75 < 150 → clamped to LBA 0 */
TEST(LbaMsf, MsfToLbaBeforeLeadIn)
{
    msf_t m;
    m.minute = 0x00;
    m.second = 0x01;
    m.frame  = 0x00;
    LONGS_EQUAL(0, (long)msf_to_lba(m));
}
