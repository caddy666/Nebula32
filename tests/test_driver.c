/**
 * @file  test_driver.c
 * @brief Host-native unit tests for drivers/driver.c (CXD2500 / DSIC2).
 *
 * Verifies that the bit-bang SPI functions produce the correct GPIO
 * output sequences on the CXD2500BQ and DSIC2 pins.  All GPIO calls
 * are recorded in the operation log by gpio_sim.c.
 *
 * Build + run:  cd test && make run_driver
 */

#include <stdio.h>
#include <stdint.h>

#include "gpio_sim.h"   /* gpio_levels, op log  */
#include "gpio_map.h"   /* PIN_* constants       */
#include "driver.h"     /* driver API            */
#include "serv_def.h"   /* MOT_PLAYM_ACTIVE etc. */

static int s_fails = 0;

#define ASSERT(cond, msg) do {                                        \
    if (!(cond)) {                                                    \
        printf("  FAIL  line %-4d  %s\n", __LINE__, (msg));         \
        s_fails++;                                                    \
    } else {                                                          \
        printf("  pass        %s\n", (msg));                         \
    }                                                                 \
} while (0)

/* ── Log-analysis helpers ────────────────────────────────────────────────
 *
 * After calling a driver function, the operation log holds every gpio_put
 * call in order.  These helpers let tests inspect the sequence without
 * coupling to the exact log index.
 * ─────────────────────────────────────────────────────────────────────── */

/* Count rising edges (0→1 transitions) on pin within the current log. */
static int count_rising_edges(uint32_t pin)
{
    int n = 0;
    for (int i = 0; i < gpio_log_size(); i++) {
        gpio_op_t op = gpio_log_get(i);
        if (op.pin == pin && op.value == 1) n++;
    }
    return n;
}

/*
 * Return the value of data_pin as it was set immediately before the
 * n-th rising edge of clk_pin (0-indexed).
 *
 * In all bit-bang loops, the data line is placed before the clock rises,
 * so scanning backward from the CLK=1 entry finds the correct DATA value.
 */
static int data_at_nth_clock(uint32_t clk_pin, uint32_t data_pin, int n)
{
    int hit = 0;
    for (int i = 0; i < gpio_log_size(); i++) {
        gpio_op_t op = gpio_log_get(i);
        if (op.pin == clk_pin && op.value == 1) {
            if (hit++ == n) {
                for (int j = i - 1; j >= 0; j--) {
                    if (gpio_log_get(j).pin == data_pin)
                        return (int)gpio_log_get(j).value;
                }
            }
        }
    }
    return -1;  /* not found */
}

/* Return 1 if pin produces a low-then-high pulse anywhere in the log. */
static int has_pulse(uint32_t pin)
{
    int saw_low = 0;
    for (int i = 0; i < gpio_log_size(); i++) {
        gpio_op_t op = gpio_log_get(i);
        if (op.pin == pin) {
            if (op.value == 0) saw_low = 1;
            if (op.value == 1 && saw_low) return 1;
        }
    }
    return 0;
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

/*
 * Test 1 — cxd2500_wr bit sequence.
 *
 * For data = 0xA5 (LSB-first: 1,0,1,0,0,1,0,1):
 *   - 3 dummy CLK pulses on PIN_CXD_CLK (total 11 rising edges)
 *   - 8 data bits on PIN_CXD_DATA, sampled at each rising edge
 *   - ULAT (latch) pulse on PIN_CXD_LAT after all 8 bits
 */
static void test_cxd2500_bit_sequence(void)
{
    printf("\n[1] cxd2500_wr(0xA5): 3 dummy + 8 data bits LSB-first\n");

    gpio_log_reset();
    cxd2500_wr(0xA5);

    /* 3 dummy pulses + 8 data pulses = 11 rising edges */
    ASSERT(count_rising_edges(PIN_CXD_CLK) == 11,
           "11 CLK rising edges (3 dummy + 8 data)");

    /* Data bits are at clock positions 3..10 (0-indexed), LSB-first */
    uint8_t d = 0xA5;
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        int expected = (d >> i) & 1;
        int got = data_at_nth_clock(PIN_CXD_CLK, PIN_CXD_DATA, 3 + i);
        if (got != expected) { all_ok = 0; break; }
    }
    ASSERT(all_ok, "data bits match 0xA5 in LSB-first order");

    /* ULAT must produce a low→high pulse after the data */
    ASSERT(has_pulse(PIN_CXD_LAT), "ULAT (latch) pulse present");
}

/*
 * Test 2 — cd6_wr dispatches MOT_PLAYM_ACTIVE → cxd2500_wr(0xE6).
 *
 * Verifies the high-level dispatch layer translates the symbolic constant
 * to the correct raw CXD2500 register value (0xE6 = closed-loop CLV mode).
 */
static void test_cd6_wr_play_mode(void)
{
    printf("\n[2] cd6_wr(MOT_PLAYM_ACTIVE) dispatches to 0xE6\n");

    gpio_log_reset();
    cd6_wr(MOT_PLAYM_ACTIVE);

    ASSERT(count_rising_edges(PIN_CXD_CLK) == 11,
           "11 CLK rising edges");

    uint8_t d = 0xE6;
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        int expected = (d >> i) & 1;
        int got = data_at_nth_clock(PIN_CXD_CLK, PIN_CXD_DATA, 3 + i);
        if (got != expected) { all_ok = 0; break; }
    }
    ASSERT(all_ok, "data bits match 0xE6 (CLV play mode) LSB-first");
}

/*
 * Test 3 — wr_dsic2 uses MSB-first bit order.
 *
 * The DSIC2 protocol is the opposite of CXD2500: bits are sent MSB-first.
 * Verify that for 0xB4 (1011 0100) the first clock edge carries bit 7 = 1,
 * not bit 0 = 0.
 */
static void test_wr_dsic2_msb_first(void)
{
    printf("\n[3] wr_dsic2(0xB4): 8 bits MSB-first\n");

    gpio_log_reset();
    wr_dsic2(0xB4);

    ASSERT(count_rising_edges(PIN_DSIC_CLK) == 8, "8 CLK rising edges");

    uint8_t d = 0xB4;
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        int expected = (d >> (7 - i)) & 1;   /* MSB first */
        int got = data_at_nth_clock(PIN_DSIC_CLK, PIN_DSIC_DATA, i);
        if (got != expected) { all_ok = 0; break; }
    }
    ASSERT(all_ok, "data bits match 0xB4 in MSB-first order");

    /* SILD (latch) must also pulse */
    ASSERT(has_pulse(PIN_DSIC_LAT), "SILD (latch) pulse present");
}

/*
 * Test 4 — audio_cxd2500 frame structure.
 *
 * audio_cxd2500() sends 3 dummy pulses + 6 audio bits + 4 opcode bits = 13
 * total CLK rising edges on PIN_CXD_CLK.  The fixed opcode is 0x0A; sent
 * LSB-first it produces the pattern 0,1,0,1 at clock positions 9–12.
 */
static void test_audio_cxd2500_frame(void)
{
    printf("\n[4] audio_cxd2500(0x00): 13 CLK edges, opcode 0x0A LSB-first\n");

    gpio_log_reset();
    audio_cxd2500(0x00);   /* audio bits all zero; only opcode matters here */

    ASSERT(count_rising_edges(PIN_CXD_CLK) == 13,
           "13 CLK rising edges (3 dummy + 6 audio + 4 opcode)");

    /* Opcode 0x0A = 0000_1010; 4 bits LSB-first = 0,1,0,1 */
    uint8_t opcode = 0x0A;
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        int expected = (opcode >> i) & 1;
        int got = data_at_nth_clock(PIN_CXD_CLK, PIN_CXD_DATA, 9 + i);
        if (got != expected) { ok = 0; break; }
    }
    ASSERT(ok, "opcode 0x0A appears LSB-first at clock positions 9–12");

    ASSERT(has_pulse(PIN_CXD_LAT), "ULAT latch pulse present");
}

/*
 * Test 5 — cd6_wr(MOT_OFF_ACTIVE) dispatches to 0xE0.
 *
 * MOT_OFF_ACTIVE puts the CXD2500 spindle into free-run (motor off) mode.
 * The raw register value 0xE0 = 1110_0000 must appear LSB-first.
 */
static void test_cd6_wr_motor_off(void)
{
    printf("\n[5] cd6_wr(MOT_OFF_ACTIVE) dispatches to 0xE0\n");

    gpio_log_reset();
    cd6_wr(MOT_OFF_ACTIVE);

    ASSERT(count_rising_edges(PIN_CXD_CLK) == 11, "11 CLK rising edges");

    uint8_t d = 0xE0;
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        int expected = (d >> i) & 1;
        int got = data_at_nth_clock(PIN_CXD_CLK, PIN_CXD_DATA, 3 + i);
        if (got != expected) { ok = 0; break; }
    }
    ASSERT(ok, "data bits match 0xE0 (motor off) LSB-first");
}

/*
 * Test 6 — get_area() zone boundaries.
 *
 * The disc is divided into three radial zones by hex_abs_min:
 *   < 16  → zone 1 (inner)
 *   16–32 → zone 2 (mid)
 *   > 32  → zone 3 (outer)
 * Boundary values (15, 16, 32, 33) must land in the correct zones.
 */
static void test_get_area_boundaries(void)
{
    printf("\n[6] get_area() zone boundaries\n");

    hex_abs_min = 15; ASSERT(get_area() == 1, "min=15 → zone 1 (inner)");
    hex_abs_min = 16; ASSERT(get_area() == 2, "min=16 → zone 2 (mid, boundary)");
    hex_abs_min = 32; ASSERT(get_area() == 2, "min=32 → zone 2 (mid, boundary)");
    hex_abs_min = 33; ASSERT(get_area() == 3, "min=33 → zone 3 (outer)");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Driver bit-sequence tests (CXD2500 / DSIC2) ===\n");

    driver_init();   /* configure pin directions, leave buses idle */

    test_cxd2500_bit_sequence();
    test_cd6_wr_play_mode();
    test_wr_dsic2_msb_first();
    test_audio_cxd2500_frame();
    test_cd6_wr_motor_off();
    test_get_area_boundaries();

    printf("\n");
    if (s_fails == 0)
        printf("All %d assertions passed.\n", 17 /* total asserts */);
    else
        printf("%d assertion(s) FAILED.\n", s_fails);

    return s_fails ? 1 : 0;
}
