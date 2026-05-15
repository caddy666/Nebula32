// =============================================================================
// test_maths.cpp — CD time arithmetic (BCD, add, subtract, compare, tracks)
//
// Intent: verify upstream/utils/maths.c, which provides the BCD-encoded
// cd_time_t arithmetic used throughout the COMMO protocol layer.  These
// functions run on both Pico hardware and the host; the host build allows
// exhaustive edge-case coverage without flashing firmware.
//
// Key invariants under test:
//   - bcd_to_hex() / hex_to_bcd(): exact inverse for values 0x00-0x99.
//   - add_time() / sub_time(): correct BCD carry/borrow across the
//     frame (0-74), second (0-59), and minute fields.
//   - compare_time(): returns SMALLER / EQUAL / BIGGER with correct ordering.
//   - calc_tracks(): derives track count and lead-out position from a TOC.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdint.h>
extern "C" {
#include "defs.h"    /* cd_time_t, SMALLER/EQUAL/BIGGER */
#include "maths.h"
}

TEST_GROUP(Maths) {};

/* -------------------------------------------------------------------------
 * bcd_to_hex
 * ---------------------------------------------------------------------- */

TEST(Maths, BcdToHex_0x00)  { LONGS_EQUAL(0,  (long)bcd_to_hex(0x00)); }
TEST(Maths, BcdToHex_0x09)  { LONGS_EQUAL(9,  (long)bcd_to_hex(0x09)); }
TEST(Maths, BcdToHex_0x10)  { LONGS_EQUAL(10, (long)bcd_to_hex(0x10)); }
TEST(Maths, BcdToHex_0x59)  { LONGS_EQUAL(59, (long)bcd_to_hex(0x59)); }
TEST(Maths, BcdToHex_0x99)  { LONGS_EQUAL(99, (long)bcd_to_hex(0x99)); }

/* -------------------------------------------------------------------------
 * hex_to_bcd
 * ---------------------------------------------------------------------- */

TEST(Maths, HexToBcd_0)  { BYTES_EQUAL(0x00, hex_to_bcd(0));  }
TEST(Maths, HexToBcd_9)  { BYTES_EQUAL(0x09, hex_to_bcd(9));  }
TEST(Maths, HexToBcd_10) { BYTES_EQUAL(0x10, hex_to_bcd(10)); }
TEST(Maths, HexToBcd_59) { BYTES_EQUAL(0x59, hex_to_bcd(59)); }
TEST(Maths, HexToBcd_99) { BYTES_EQUAL(0x99, hex_to_bcd(99)); }

/* -------------------------------------------------------------------------
 * Round-trip: hex_to_bcd(bcd_to_hex(x)) == x for many valid BCD values
 * ---------------------------------------------------------------------- */

TEST(Maths, RoundTripBcdHex)
{
    uint8_t vals[] = { 0x00, 0x01, 0x09, 0x10, 0x19, 0x20, 0x45, 0x59,
                       0x60, 0x74, 0x75, 0x99 };
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
        BYTES_EQUAL(vals[i], hex_to_bcd(bcd_to_hex(vals[i])));
    }
}

/* -------------------------------------------------------------------------
 * compare_time
 * ---------------------------------------------------------------------- */

TEST(Maths, CompareTimeSmaller)
{
    cd_time_t a = { 1, 0, 0 };
    cd_time_t b = { 2, 0, 0 };
    BYTES_EQUAL(SMALLER, compare_time(&a, &b));
}

TEST(Maths, CompareTimeEqual)
{
    cd_time_t a = { 3, 30, 15 };
    cd_time_t b = { 3, 30, 15 };
    BYTES_EQUAL(EQUAL, compare_time(&a, &b));
}

TEST(Maths, CompareTimeBigger)
{
    cd_time_t a = { 5, 0, 0 };
    cd_time_t b = { 4, 59, 74 };
    BYTES_EQUAL(BIGGER, compare_time(&a, &b));
}

TEST(Maths, CompareTimeSecondTieBreak)
{
    cd_time_t a = { 2, 30, 0 };
    cd_time_t b = { 2, 31, 0 };
    BYTES_EQUAL(SMALLER, compare_time(&a, &b));
}

TEST(Maths, CompareTimeFrameTieBreak)
{
    cd_time_t a = { 1, 0, 10 };
    cd_time_t b = { 1, 0, 11 };
    BYTES_EQUAL(SMALLER, compare_time(&a, &b));
}

/* -------------------------------------------------------------------------
 * add_time
 * ---------------------------------------------------------------------- */

TEST(Maths, AddTimeBasic)
{
    cd_time_t a = { 0, 1, 10 };
    cd_time_t b = { 0, 0, 20 };
    cd_time_t r;
    add_time(&a, &b, &r);
    BYTES_EQUAL(0, r.min);
    BYTES_EQUAL(1, r.sec);
    BYTES_EQUAL(30, r.frm);
}

TEST(Maths, AddTimeFrameCarry)
{
    cd_time_t a = { 0, 0, 50 };
    cd_time_t b = { 0, 0, 50 };
    cd_time_t r;
    add_time(&a, &b, &r);
    /* 100 frames = 1 sec + 25 frames */
    BYTES_EQUAL(0, r.min);
    BYTES_EQUAL(1, r.sec);
    BYTES_EQUAL(25, r.frm);
}

TEST(Maths, AddTimeSecondCarry)
{
    cd_time_t a = { 0, 59, 0 };
    cd_time_t b = { 0,  1, 0 };
    cd_time_t r;
    add_time(&a, &b, &r);
    /* 60 seconds → 1 minute carry */
    BYTES_EQUAL(1, r.min);
    BYTES_EQUAL(0, r.sec);
    BYTES_EQUAL(0, r.frm);
}

/* -------------------------------------------------------------------------
 * subtract_time
 * ---------------------------------------------------------------------- */

TEST(Maths, SubtractTimeBasic)
{
    cd_time_t a = { 2, 30, 50 };
    cd_time_t b = { 1, 10, 20 };
    cd_time_t r;
    subtract_time(&a, &b, &r);
    BYTES_EQUAL(1, r.min);
    BYTES_EQUAL(20, r.sec);
    BYTES_EQUAL(30, r.frm);
}

TEST(Maths, SubtractTimeFrameBorrow)
{
    cd_time_t a = { 0, 1, 10 };
    cd_time_t b = { 0, 0, 20 };
    cd_time_t r;
    subtract_time(&a, &b, &r);
    /* 10 - 20 → borrow: 10 + 75 - 20 = 65 frames, sec = 0 */
    BYTES_EQUAL(0, r.min);
    BYTES_EQUAL(0, r.sec);
    BYTES_EQUAL(65, r.frm);
}

TEST(Maths, SubtractTimeSecondBorrow)
{
    cd_time_t a = { 1, 0, 0 };
    cd_time_t b = { 0, 30, 0 };
    cd_time_t r;
    subtract_time(&a, &b, &r);
    BYTES_EQUAL(0, r.min);
    BYTES_EQUAL(30, r.sec);
    BYTES_EQUAL(0, r.frm);
}

/* -------------------------------------------------------------------------
 * calc_tracks
 * ---------------------------------------------------------------------- */

TEST(Maths, CalcTracksEqualTimesIsZero)
{
    cd_time_t t = { 5, 0, 0 };
    LONGS_EQUAL(0, (long)calc_tracks(&t, &t));
}

TEST(Maths, CalcTracksPositiveForLaterTime)
{
    /* t2 further out (larger time) → positive result */
    cd_time_t t1 = { 10, 0, 0 };
    cd_time_t t2 = { 20, 0, 0 };
    CHECK_TRUE(calc_tracks(&t1, &t2) > 0);
}

TEST(Maths, CalcTracksNegativeForEarlierTime)
{
    /* t2 closer to centre (smaller time) → negative result */
    cd_time_t t1 = { 20, 0, 0 };
    cd_time_t t2 = { 10, 0, 0 };
    CHECK_TRUE(calc_tracks(&t1, &t2) < 0);
}
