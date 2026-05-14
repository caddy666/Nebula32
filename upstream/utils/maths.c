/**
 * @file  maths.c
 * @brief BCD ↔ hex conversion, CD time arithmetic, and track-count estimation.
 *
 * All functions are pure (no side-effects) and operate on cd_time_t structs.
 */

#include <stdint.h>
#include "defs.h"

/* =========================================================================
 * BCD helpers
 * ====================================================================== */

uint8_t bcd_to_hex(uint8_t bcd)
{
    return ((bcd >> 4) * 10u) + (bcd & 0x0Fu);
}

uint8_t hex_to_bcd(uint8_t hex)
{
    uint8_t tens = hex / 10u;
    return (uint8_t)((tens << 4) | (hex - tens * 10u));
}

void bcd_to_hex_time(const cd_time_t *src, cd_time_t *dst)
{
    dst->min = bcd_to_hex(src->min);
    dst->sec = bcd_to_hex(src->sec);
    dst->frm = bcd_to_hex(src->frm);
}

/* =========================================================================
 * Time comparison
 * Returns: SMALLER (0), EQUAL (1), or BIGGER (2)
 * ====================================================================== */

uint8_t compare_time(const cd_time_t *a, const cd_time_t *b)
{
    if (a->min < b->min) return SMALLER;
    if (a->min > b->min) return BIGGER;
    if (a->sec < b->sec) return SMALLER;
    if (a->sec > b->sec) return BIGGER;
    if (a->frm < b->frm) return SMALLER;
    if (a->frm > b->frm) return BIGGER;
    return EQUAL;
}

/* =========================================================================
 * Time arithmetic
 * ====================================================================== */

void add_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r)
{
    uint32_t frm = (uint32_t)a->frm + b->frm;
    uint32_t sec = (uint32_t)a->sec + b->sec;
    uint32_t min = (uint32_t)a->min + b->min;

    sec += frm / 75u;
    frm %= 75u;
    min += sec / 60u;
    sec %= 60u;

    r->min = (uint8_t)min;
    r->sec = (uint8_t)sec;
    r->frm = (uint8_t)frm;
}

void subtract_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r)
{
    int frm = (int)a->frm - (int)b->frm;
    int sec = (int)a->sec - (int)b->sec;
    int min = (int)a->min - (int)b->min;

    if (frm < 0) { frm += 75; sec--; }
    if (sec < 0) { sec += 60; min--; }

    r->min = (uint8_t)min;
    r->sec = (uint8_t)sec;
    r->frm = (uint8_t)frm;
}

/* Convert a time to an absolute frame count (used for track estimation). */
static uint32_t convert_time(const cd_time_t *t)
{
    return ((uint32_t)t->min * 60u + t->sec) * 75u + t->frm;
}

/* =========================================================================
 * Integer square root (Newton's method, fixed-point)
 * ====================================================================== */

static uint32_t isqrt(uint32_t x)
{
    if (x == 0) return 0;

    uint32_t res = 0;
    uint32_t bit = 1u << 30;

    while (bit > x)  bit >>= 2;

    while (bit) {
        uint32_t tmp = res + bit;
        res >>= 1;
        if (x >= tmp) { x -= tmp; res += bit; }
        bit >>= 2;
    }
    return res;
}

/* Track position estimate from absolute frame count (disc geometry). */
static uint32_t tracks_calc(const cd_time_t *t)
{
    /* Constants derived from standard CD disc geometry. */
    const uint32_t A = 0xBB3Du;
    const uint32_t B = (0x671Fu >> 3u);
    uint32_t T = convert_time(t);
    return isqrt(A * T + B);
}

/**
 * @brief  Estimate the number of tracks (grooves) between two disc times.
 * @return Positive = t2 is further out than t1; negative = t2 is inside t1.
 */
int calc_tracks(const cd_time_t *t1, const cd_time_t *t2)
{
    uint8_t cmp = compare_time(t1, t2);
    if (cmp == EQUAL) return 0;

    const cd_time_t *big   = (cmp == BIGGER) ? t1 : t2;
    const cd_time_t *small = (cmp == BIGGER) ? t2 : t1;

    uint32_t bv = tracks_calc(big);
    uint32_t sv = tracks_calc(small);
    int result  = (int)((bv - sv) >> 4);   /* divide by 16 */

    return (cmp == BIGGER) ? -result : result;
}
