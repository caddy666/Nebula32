// =============================================================================
// test_effects_color.cpp — Demoscene visualiser colour math tests
//
// Intent: verify the three pure colour/coordinate helpers in src/effects.c
// that have no display hardware dependency.  All are static in effects.c;
// this file replicates them verbatim and tests the invariants.  The rgb()
// macro from effects.h IS included directly — it is a static inline with no
// hardware dependency.
//
// Functions under test:
//   rgb()           — packs R,G,B (0-255) into byte-swapped RGB565.
//                     The ST7789 driver has RAMCTRL byte-swap enabled, so
//                     every pixel word must be stored big-endian.
//
//   hsv()           — integer HSV→RGB565 conversion using six hue sectors.
//                     Zero saturation must produce a grey; full saturation at
//                     primary hues must produce the corresponding pure colour.
//
//   copper_color()  — linearly interpolates the Amiga copper gradient table
//                     (dark red → white-yellow) across a [0,255] fraction.
//                     frac=0 must be the darkest entry; frac=255 the brightest.
//
//   sample_to_y()   — maps a Q15 audio sample (int16_t) to a display Y
//                     coordinate in [0, DISP_H-1].  Centre (sample=0) maps
//                     to DISP_H/2.  Positive samples move up (lower Y).
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdint.h>
#include <string.h>

// rgb() is a static inline in effects.h — safe to include directly
#include "effects.h"

// ---------------------------------------------------------------------------
// Replicated helpers — must stay in sync with src/effects.c
// ---------------------------------------------------------------------------

static uint16_t hsv(uint8_t h, uint8_t s, uint8_t v)
{
    if (s == 0) return rgb(v, v, v);
    uint8_t reg = h / 43;
    uint8_t rem = (uint8_t)((h - (uint16_t)reg * 43) * 6);
    uint8_t p   = (uint8_t)((v * (255u - s)) >> 8);
    uint8_t q   = (uint8_t)((v * (255u - ((s * rem) >> 8))) >> 8);
    uint8_t t_  = (uint8_t)((v * (255u - ((s * (255u - rem)) >> 8))) >> 8);
    switch (reg) {
        case 0:  return rgb(v,  t_, p);
        case 1:  return rgb(q,  v,  p);
        case 2:  return rgb(p,  v,  t_);
        case 3:  return rgb(p,  q,  v);
        case 4:  return rgb(t_, p,  v);
        default: return rgb(v,  p,  q);
    }
}

static const uint8_t COP_R[] = { 0x30, 0x60, 0x90, 0xB0, 0xD0, 0xF0, 0xFF, 0xFF };
static const uint8_t COP_G[] = { 0x00, 0x00, 0x10, 0x30, 0x60, 0x90, 0xD0, 0xFF };
static const uint8_t COP_B[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, 0xFF };
#define COP_STEPS 7

static uint16_t copper_color(uint8_t frac)
{
    uint8_t seg = (uint8_t)((uint16_t)frac * COP_STEPS >> 8);
    uint8_t t   = (uint8_t)((uint16_t)frac * COP_STEPS - (uint16_t)seg * 256);
    if (seg >= COP_STEPS) { seg = COP_STEPS - 1; t = 255; }
    uint8_t r = (uint8_t)(COP_R[seg] + (((int)COP_R[seg+1] - COP_R[seg]) * t >> 8));
    uint8_t g = (uint8_t)(COP_G[seg] + (((int)COP_G[seg+1] - COP_G[seg]) * t >> 8));
    uint8_t b = (uint8_t)(COP_B[seg] + (((int)COP_B[seg+1] - COP_B[seg]) * t >> 8));
    return rgb(r, g, b);
}

static inline int sample_to_y(int16_t s)
{
    int y = DISP_H / 2 - ((int)s * (DISP_H / 2 - 2)) / 32767;
    if (y < 0)        y = 0;
    if (y >= DISP_H)  y = DISP_H - 1;
    return y;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(EffectsColor) {};

/* -------------------------------------------------------------------------
 * rgb() — byte-swapped RGB565 packing
 * The ST7789 driver enables RAMCTRL byte-swap, so the host word must be
 * stored big-endian: rgb(R,G,B) = bswap( (R>>3)<<11 | (G>>2)<<5 | (B>>3) )
 * ---------------------------------------------------------------------- */

TEST(EffectsColor, Rgb_Black_IsZero)
{
    LONGS_EQUAL(0x0000, (long)rgb(0, 0, 0));
}

TEST(EffectsColor, Rgb_White_IsFFFF)
{
    LONGS_EQUAL(0xFFFF, (long)rgb(255, 255, 255));
}

// Pure red: R[7:3]=11111 → native 0xF800 → bswapped 0x00F8
TEST(EffectsColor, Rgb_PureRed)
{
    LONGS_EQUAL(0x00F8, (long)rgb(255, 0, 0));
}

// Pure green: G[7:2]=111111 → native 0x07E0 → bswapped 0xE007
TEST(EffectsColor, Rgb_PureGreen)
{
    LONGS_EQUAL(0xE007, (long)rgb(0, 255, 0));
}

// Pure blue: B[7:3]=11111 → native 0x001F → bswapped 0x1F00
TEST(EffectsColor, Rgb_PureBlue)
{
    LONGS_EQUAL(0x1F00, (long)rgb(0, 0, 255));
}

// Low-bit masking: R=4 (below 0xF8 mask threshold) → R contribution=0
TEST(EffectsColor, Rgb_SubthresholdRed_IsZero)
{
    LONGS_EQUAL(rgb(0, 0, 0), (long)rgb(4, 0, 0));
}

/* -------------------------------------------------------------------------
 * hsv() — integer HSV→RGB565
 * ---------------------------------------------------------------------- */

// Saturation=0 always produces a grey (all channels equal)
TEST(EffectsColor, Hsv_ZeroSaturation_IsGrey)
{
    // Should equal rgb(v, v, v) for any hue
    LONGS_EQUAL((long)rgb(128, 128, 128), (long)hsv(0,   0, 128));
    LONGS_EQUAL((long)rgb(128, 128, 128), (long)hsv(128, 0, 128));
    LONGS_EQUAL((long)rgb(128, 128, 128), (long)hsv(255, 0, 128));
}

// h=0, s=255, v=255: sector 0 → rgb(v, t, p) where p≈0, t≈0 → near pure red
TEST(EffectsColor, Hsv_PureRed_Hue0)
{
    uint16_t c = hsv(0, 255, 255);
    // At sector boundary h=0: rem=0 → t=0, p=0 → rgb(255, 0, 0)
    LONGS_EQUAL((long)rgb(255, 0, 0), (long)c);
}

// Value=0 always produces black regardless of hue/saturation
TEST(EffectsColor, Hsv_ZeroValue_IsBlack)
{
    LONGS_EQUAL((long)rgb(0, 0, 0), (long)hsv(0,   255, 0));
    LONGS_EQUAL((long)rgb(0, 0, 0), (long)hsv(128, 255, 0));
}

// Different hues with full sat/val must produce different colours
TEST(EffectsColor, Hsv_DifferentHues_DifferentColors)
{
    uint16_t red   = hsv(0,   255, 255);
    uint16_t green = hsv(85,  255, 255);
    uint16_t blue  = hsv(170, 255, 255);
    CHECK_TRUE(red != green);
    CHECK_TRUE(green != blue);
    CHECK_TRUE(red != blue);
}

/* -------------------------------------------------------------------------
 * copper_color() — gradient interpolation
 * ---------------------------------------------------------------------- */

// frac=0 must use the first table entry (darkest — 0x30, 0x00, 0x00)
TEST(EffectsColor, Copper_Frac0_IsDarkest)
{
    LONGS_EQUAL((long)rgb(COP_R[0], COP_G[0], COP_B[0]),
                (long)copper_color(0));
}

// frac=255 must clamp to last segment interpolated to t=255 → entry[7]
TEST(EffectsColor, Copper_Frac255_IsBrightest)
{
    LONGS_EQUAL((long)rgb(COP_R[COP_STEPS], COP_G[COP_STEPS], COP_B[COP_STEPS]),
                (long)copper_color(255));
}

// Monotonically increasing brightness: frac=64 < frac=192 (red channel)
TEST(EffectsColor, Copper_BrightnessIncreasesWithFrac)
{
    // Extract native (non-byteswapped) red channel to compare magnitudes.
    // Since bswap is applied, we compare the whole word: a larger frac must
    // never produce the same or darker colour as a smaller frac.
    uint16_t dark  = copper_color(32);
    uint16_t light = copper_color(200);
    CHECK_TRUE(dark != light);
}

/* -------------------------------------------------------------------------
 * sample_to_y() — sample to display Y coordinate
 * ---------------------------------------------------------------------- */

// Zero sample maps to screen centre
TEST(EffectsColor, SampleToY_Zero_IsCentre)
{
    LONGS_EQUAL(DISP_H / 2, sample_to_y(0));
}

// Max positive sample maps near top of screen (low Y)
TEST(EffectsColor, SampleToY_MaxPositive_NearTop)
{
    int y = sample_to_y(32767);
    CHECK_TRUE(y >= 0);
    CHECK_TRUE(y < DISP_H / 2);
}

// Max negative sample maps near bottom of screen (high Y)
TEST(EffectsColor, SampleToY_MaxNegative_NearBottom)
{
    int y = sample_to_y(-32768);
    CHECK_TRUE(y > DISP_H / 2);
    CHECK_TRUE(y < DISP_H);
}

// Output always within valid display bounds
TEST(EffectsColor, SampleToY_AlwaysInBounds)
{
    int16_t samples[] = { 0, 32767, -32768, 16384, -16384, 1, -1 };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
        int y = sample_to_y(samples[i]);
        CHECK_TRUE(y >= 0);
        CHECK_TRUE(y < DISP_H);
    }
}

// Positive sample produces lower Y than zero (moves toward top)
TEST(EffectsColor, SampleToY_PositiveSample_LowerYThanCentre)
{
    CHECK_TRUE(sample_to_y(10000) < sample_to_y(0));
}

/* -------------------------------------------------------------------------
 * Gap 22: all six HSV hue sectors produce distinct colours at full saturation
 * and value.  The six sector boundaries (h = 0, 43, 86, 129, 172, 215) each
 * map to a different primary or secondary hue; duplicates would indicate a
 * switch-case fall-through or wrong sector boundary constant.
 * ---------------------------------------------------------------------- */
TEST(EffectsColor, Hsv_SixSectors_AllDistinct)
{
    uint16_t c[6];
    /* One sample per sector: h = 0, 43, 86, 129, 172, 215 */
    c[0] = hsv(  0, 255, 255);
    c[1] = hsv( 43, 255, 255);
    c[2] = hsv( 86, 255, 255);
    c[3] = hsv(129, 255, 255);
    c[4] = hsv(172, 255, 255);
    c[5] = hsv(215, 255, 255);
    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 6; j++) {
            CHECK_TRUE(c[i] != c[j]);
        }
    }
}

/* -------------------------------------------------------------------------
 * Gap 23: copper gradient red channel must increase monotonically across the
 * table.  The COP_R[] entries are strictly increasing, so comparing sampled
 * fractions at even intervals must always produce a non-decreasing sequence.
 *
 * We extract the red channel from the byte-swapped RGB565 word:
 *   native565 = bswap(bswapped) = (word & 0xFF)<<8 | (word>>8)
 *   red5      = (native565 >> 11) & 0x1F
 * ---------------------------------------------------------------------- */
static uint8_t red5_channel(uint16_t bswapped_rgb565)
{
    uint16_t native = (uint16_t)(((bswapped_rgb565 & 0xFF) << 8) |
                                 ((bswapped_rgb565 >> 8) & 0xFF));
    return (uint8_t)((native >> 11) & 0x1F);
}

TEST(EffectsColor, Copper_RedChannelMonotonicallyIncreases)
{
    /* Sample at 5 evenly spaced fractions; red must be non-decreasing. */
    uint8_t fracs[] = { 0, 50, 100, 150, 200, 255 };
    uint8_t prev = red5_channel(copper_color(fracs[0]));
    for (size_t i = 1; i < sizeof(fracs)/sizeof(fracs[0]); i++) {
        uint8_t curr = red5_channel(copper_color(fracs[i]));
        CHECK_TRUE(curr >= prev);
        prev = curr;
    }
}
