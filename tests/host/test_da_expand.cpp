// =============================================================================
// test_da_expand.cpp — DA output I2S expansion and clkdiv tests
//
// Intent: verify the two pure helpers in src/da_output.c that have no DMA,
// PIO, or Pico SDK dependency.  Both are static; this file replicates them
// verbatim and tests the invariants.
//
// Functions under test:
//   expand_to_i2s24() — converts a 2352-byte raw CD sector into 1176 uint32_t
//                       I2S words, one word per channel per stereo pair.
//                       Each word carries the 16-bit sample in bits [31:16];
//                       bits [15:0] are always zero (the PIO's autopull=24
//                       outputs bits [31:8] and discards [7:0]).
//                       Little-endian byte order: L = raw[0]|(raw[1]<<8).
//
//   _clkdiv()         — returns 32.0f (1×) or 16.0f (2×).  Documents the
//                       corrected clkdiv values after the BUG-2 fix; the
//                       original Commodore reference used 48/24.
//
// The expand function is the critical path for audio correctness.  A wrong
// shift or byte-order swap here produces silence or noise on Akiko's I2S
// input with no other visible error.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Replicated helpers — must stay in sync with src/da_output.c
// ---------------------------------------------------------------------------

#define SECTOR_RAW_SIZE   2352
#define SECTOR_DMA_WORDS  1176   // 588 stereo pairs × 2 words

static void expand_to_i2s24(const uint8_t *raw, uint32_t *out)
{
    for (int i = 0; i < 588; i++) {
        uint16_t l = (uint16_t)raw[i * 4 + 0] | ((uint16_t)raw[i * 4 + 1] << 8);
        uint16_t r = (uint16_t)raw[i * 4 + 2] | ((uint16_t)raw[i * 4 + 3] << 8);
        out[i * 2 + 0] = (uint32_t)l << 16;
        out[i * 2 + 1] = (uint32_t)r << 16;
    }
}

static float clkdiv(bool dbl) { return dbl ? 16.0f : 32.0f; }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(DaExpand) {};

/* -------------------------------------------------------------------------
 * expand_to_i2s24 — zero input
 * ---------------------------------------------------------------------- */

TEST(DaExpand, ZeroSector_AllWordsZero)
{
    uint8_t  raw[SECTOR_RAW_SIZE] = {0};
    uint32_t out[SECTOR_DMA_WORDS];
    memset(out, 0xFF, sizeof(out));   // pre-fill with noise
    expand_to_i2s24(raw, out);
    for (int i = 0; i < SECTOR_DMA_WORDS; i++) {
        LONGS_EQUAL(0, (long)out[i]);
    }
}

/* -------------------------------------------------------------------------
 * expand_to_i2s24 — left/right channel separation
 * ---------------------------------------------------------------------- */

// Left sample 0x1234, right=0 → out[0]=0x12340000, out[1]=0x00000000
TEST(DaExpand, LeftSample_InWordZero)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    raw[0] = 0x34;   // L low byte
    raw[1] = 0x12;   // L high byte
    raw[2] = 0x00;   // R low byte
    raw[3] = 0x00;   // R high byte

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);
    LONGS_EQUAL(0x12340000ul, (long)out[0]);  // left
    LONGS_EQUAL(0x00000000ul, (long)out[1]);  // right
}

// Right sample 0x5678, left=0 → out[0]=0x00000000, out[1]=0x56780000
TEST(DaExpand, RightSample_InWordOne)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    raw[0] = 0x00;
    raw[1] = 0x00;
    raw[2] = 0x78;   // R low byte
    raw[3] = 0x56;   // R high byte

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);
    LONGS_EQUAL(0x00000000ul, (long)out[0]);
    LONGS_EQUAL(0x56780000ul, (long)out[1]);
}

/* -------------------------------------------------------------------------
 * expand_to_i2s24 — boundary sample values
 * ---------------------------------------------------------------------- */

// Max positive int16_t: 0x7FFF → word = 0x7FFF0000
TEST(DaExpand, MaxPositiveSample)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    raw[0] = 0xFF;   // 0x7FFF little-endian
    raw[1] = 0x7F;

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);
    LONGS_EQUAL(0x7FFF0000ul, (long)out[0]);
}

// Min negative int16_t: 0x8000 → word = 0x80000000
TEST(DaExpand, MaxNegativeSample)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    raw[0] = 0x00;   // 0x8000 little-endian
    raw[1] = 0x80;

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);
    LONGS_EQUAL((long)0x80000000ul, (long)out[0]);
}

/* -------------------------------------------------------------------------
 * expand_to_i2s24 — lower 16 bits always zero
 * ---------------------------------------------------------------------- */

TEST(DaExpand, LowerHalfwordAlwaysZero)
{
    // Fill raw with a walking pattern to hit all bit positions
    uint8_t raw[SECTOR_RAW_SIZE];
    for (int i = 0; i < SECTOR_RAW_SIZE; i++)
        raw[i] = (uint8_t)(i & 0xFF);

    uint32_t out[SECTOR_DMA_WORDS];
    expand_to_i2s24(raw, out);

    for (int i = 0; i < SECTOR_DMA_WORDS; i++) {
        LONGS_EQUAL(0, (long)(out[i] & 0x0000FFFFul));
    }
}

/* -------------------------------------------------------------------------
 * expand_to_i2s24 — pair addressing
 * ---------------------------------------------------------------------- */

// Sample placed at stereo pair N maps to output words N*2 and N*2+1
TEST(DaExpand, PairN_MapsToCorrectOutputWords)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    // Place a recognisable sample at pair 100 (byte offset 400)
    raw[400] = 0xAB;   // L low
    raw[401] = 0xCD;   // L high

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);

    LONGS_EQUAL(0xCDAB0000ul, (long)out[200]);  // pair 100, left word
    LONGS_EQUAL(0x00000000ul, (long)out[201]);  // pair 100, right word
}

// Last stereo pair (587) maps to out[1174] and out[1175]
TEST(DaExpand, LastPair_MapsToLastTwoWords)
{
    uint8_t raw[SECTOR_RAW_SIZE] = {0};
    raw[587 * 4 + 2] = 0x11;   // R low of last pair
    raw[587 * 4 + 3] = 0x22;   // R high of last pair

    uint32_t out[SECTOR_DMA_WORDS] = {0};
    expand_to_i2s24(raw, out);

    LONGS_EQUAL(0x00000000ul, (long)out[1174]);  // left word of last pair
    LONGS_EQUAL(0x22110000ul, (long)out[1175]);  // right word of last pair
}

/* -------------------------------------------------------------------------
 * _clkdiv — corrected values post BUG-2 fix
 * Original Commodore reference: 48 (1×) and 24 (2×) → wrong BCLK freq.
 * Corrected:                    32 (1×) and 16 (2×) → 2.117 / 4.234 MHz.
 * ---------------------------------------------------------------------- */

TEST(DaExpand, Clkdiv_SingleSpeed_Is32)
{
    DOUBLES_EQUAL(32.0, clkdiv(false), 0.001);
}

TEST(DaExpand, Clkdiv_DoubleSpeed_Is16)
{
    DOUBLES_EQUAL(16.0, clkdiv(true), 0.001);
}
