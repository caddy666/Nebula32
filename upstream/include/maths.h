/**
 * @file  maths.h
 * @brief BCD/time arithmetic declarations (implementations in utils/maths.c)
 *
 * These functions operate on cd_time_t (BCD or hex MSF values).
 * See upstream_types.h for msf_t ↔ cd_time_t conversion helpers.
 */
#pragma once
#include <stdint.h>
#include "defs.h"  // cd_time_t

/** Convert BCD byte to binary (e.g. 0x35 → 35). */
uint8_t bcd_to_hex(uint8_t bcd);

/** Convert binary byte to BCD (e.g. 35 → 0x35). */
uint8_t hex_to_bcd(uint8_t hex);

/** Convert all fields of a cd_time_t from BCD to binary in-place. */
void bcd_to_hex_time(const cd_time_t *src, cd_time_t *dst);

/** Compare two cd_time_t values. Returns SMALLER / EQUAL / BIGGER. */
uint8_t compare_time(const cd_time_t *a, const cd_time_t *b);

/** Add two cd_time_t values, storing result in r. */
void add_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r);

/** Subtract b from a, storing result in r. */
void subtract_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r);

/**
 * Estimate the number of disc grooves (tracks) between two times.
 * Positive result: t2 is further out than t1.
 * Negative result: t2 is closer to the centre than t1.
 */
int calc_tracks(const cd_time_t *t1, const cd_time_t *t2);
