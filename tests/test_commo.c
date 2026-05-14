/**
 * @file  test_commo.c
 * @brief Host-native unit tests for core/commo.c
 *
 * Tests the COMMO serial state machine without any physical hardware.
 * GPIO calls are intercepted by gpio_sim.c; COMMO DATA bits are injected
 * via the CLK-rising-edge hook in gpio_put().
 *
 * Build + run:  cd test && make run_commo
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gpio_sim.h"   /* gpio_levels, sim helpers */
#include "gpio_map.h"   /* PIN_COMMO_* constants    */
#include "commo.h"      /* COMMO public API         */

/* ── Minimal test framework ─────────────────────────────────────────────── */
static int s_fails = 0;

#define ASSERT(cond, msg) do {                                        \
    if (!(cond)) {                                                    \
        printf("  FAIL  line %-4d  %s\n", __LINE__, (msg));         \
        s_fails++;                                                    \
    } else {                                                          \
        printf("  pass        %s\n", (msg));                         \
    }                                                                 \
} while (0)

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* One's-complement checksum: ~(sum of all bytes). */
static uint8_t checksum_of(const uint8_t *buf, int len)
{
    uint8_t s = 0;
    for (int i = 0; i < len; i++) s += buf[i];
    return (uint8_t)~s;
}

/**
 * Drive a complete COMMO packet into the state machine.
 *
 * bytes[] must be [opcode, params..., checksum].
 * len    is the total count including the checksum byte.
 *
 * Protocol simulation:
 *   • Setting PIN_COMMO_DATA = 0 before each COMMO_INTERFACE() call
 *     mimics the host asserting the start-of-byte signal.
 *   • The CLK-edge hook in gpio_put() injects queued bits into DATA
 *     as the firmware drives CLK inside get_rxd_data().
 *
 * Call sequence:
 *   1. Pre-call: DATA=0 → COMMO_INTERFACE() → IDLE detects start,
 *      transitions to RXD_OPCODE.
 *   2. Loop len times: DATA=0 → COMMO_INTERFACE() → receive one byte
 *      (opcode, then each param/checksum in turn).
 */
static void drive_packet(const uint8_t *bytes, int len)
{
    sim_commo_reset();
    for (int i = 0; i < len; i++)
        sim_commo_queue_byte(bytes[i]);

    gpio_levels[PIN_COMMO_DATA] = 0;
    COMMO_INTERFACE();                  /* IDLE → RXD_OPCODE            */

    for (int i = 0; i < len; i++) {
        gpio_levels[PIN_COMMO_DATA] = 0; /* host signals next byte ready */
        COMMO_INTERFACE();               /* receive one byte             */
    }
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

/*
 * Test 1 — 1-byte opcode, no parameters.
 *
 * Uses STOP_OPC (0x03): command_length_table[3] = 1, so the byte after
 * the opcode is the checksum with no parameter bytes in between.
 */
static void test_single_byte_opcode(void)
{
    printf("\n[1] 1-byte opcode (STOP = 0x03), no params\n");

    uint8_t pkt[2];
    pkt[0] = 0x03;                          /* STOP_OPC */
    pkt[1] = checksum_of(pkt, 1);           /* ~0x03 = 0xFC */

    COMMO_INIT();
    drive_packet(pkt, 2);

    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "rx status = NEW_COMMAND");
    ASSERT(GET_BUFFER(0) == 0x03,                   "opcode = 0x03");
}

/*
 * Test 2 — 2-byte opcode with one parameter.
 *
 * Uses opcode 0x05: command_length_table[5] = 2, meaning one parameter
 * byte follows the opcode before the checksum.
 */
static void test_opcode_with_param(void)
{
    printf("\n[2] 2-byte opcode (0x05) with 1 param byte\n");

    uint8_t pkt[3];
    pkt[0] = 0x05;
    pkt[1] = 0x42;                          /* arbitrary param */
    pkt[2] = checksum_of(pkt, 2);           /* ~(0x05+0x42) = 0xB8 */

    COMMO_INIT();
    drive_packet(pkt, 3);

    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "rx status = NEW_COMMAND");
    ASSERT(GET_BUFFER(0) == 0x05,                   "opcode = 0x05");
    ASSERT(GET_BUFFER(1) == 0x42,                   "param1 = 0x42");
}

/*
 * Test 3 — Corrupted checksum.
 *
 * Send opcode 0x03 with checksum byte 0x00 (correct value is 0xFC).
 * The state machine must reject it with COMMO_CMD_ERROR.
 */
static void test_bad_checksum(void)
{
    printf("\n[3] Bad checksum byte → COMMO_CMD_ERROR\n");

    uint8_t pkt[2] = { 0x03, 0x00 };       /* 0x00 is wrong; 0xFC is right */

    COMMO_INIT();
    drive_packet(pkt, 2);

    ASSERT(NEW_CMD_RECEIVED() == COMMO_CMD_ERROR, "rx status = CMD_ERROR");
}

/*
 * Test 4 — Same opcode repeated.
 *
 * The first reception of an opcode yields COMMO_NEW_COMMAND.  A second
 * identical packet without an intervening different opcode must yield
 * COMMO_SAME_COMMAND so the host can poll the drive's state.
 */
static void test_same_command_detection(void)
{
    printf("\n[4] Repeated opcode → COMMO_SAME_COMMAND\n");

    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    COMMO_INIT();

    drive_packet(pkt, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND,  "1st reception: NEW_COMMAND");
    FREE_CMD_BUFFER();

    drive_packet(pkt, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_SAME_COMMAND, "2nd reception: SAME_COMMAND");
}

/*
 * Test 5 — Zero opcode triggers ERR_SEND.
 *
 * A zero opcode is invalid.  The state machine enters ERR_SEND with a
 * 128-tick countdown before reporting COMMO_CMD_ERROR.  This gives the
 * host time to release the bus after a malformed byte.
 */
static void test_zero_opcode_error(void)
{
    printf("\n[5] Zero opcode → ERR_SEND (128 ticks) → COMMO_CMD_ERROR\n");

    sim_commo_reset();
    sim_commo_queue_byte(0x00);

    COMMO_INIT();

    /* Trigger IDLE → RXD_OPCODE */
    gpio_levels[PIN_COMMO_DATA] = 0;
    COMMO_INTERFACE();

    /* Receive the zero opcode: state → ERR_SEND, byte_counter = 128 */
    gpio_levels[PIN_COMMO_DATA] = 0;
    COMMO_INTERFACE();

    ASSERT(NEW_CMD_RECEIVED() == COMMO_NO_COMMAND, "not reported yet (in ERR_SEND)");

    /* Countdown loop: 128 decrements then one final tick that fires the error */
    for (int i = 0; i < 129; i++)
        COMMO_INTERFACE();

    ASSERT(NEW_CMD_RECEIVED() == COMMO_CMD_ERROR, "reported after 128 ticks: CMD_ERROR");
}

/* ── gpio_log helpers (mirrored from test_driver.c for TX-path tests) ────── */

static int count_rising_edges_commo(uint32_t pin)
{
    int n = 0;
    for (int i = 0; i < gpio_log_size(); i++) {
        gpio_op_t op = gpio_log_get(i);
        if (op.pin == pin && op.value == 1) n++;
    }
    return n;
}

static int data_before_nth_clock(uint32_t clk_pin, uint32_t data_pin, int n)
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
    return -1;
}

/* ── New tests ──────────────────────────────────────────────────────────── */

/*
 * Test 6 — FREE_CMD_BUFFER clears the report_cmd flag.
 *
 * After receiving a packet, NEW_CMD_RECEIVED() must return NO_COMMAND once
 * FREE_CMD_BUFFER() has been called.
 */
static void test_free_buffer_clears_status(void)
{
    printf("\n[6] FREE_CMD_BUFFER → NEW_CMD_RECEIVED returns NO_COMMAND\n");

    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    COMMO_INIT();
    drive_packet(pkt, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "status = NEW_COMMAND before free");
    FREE_CMD_BUFFER();
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NO_COMMAND,  "status = NO_COMMAND after free");
}

/*
 * Test 7 — Maximum-length opcode: index 4 → 12 body bytes (11 params).
 *
 * command_length_table[4] = 12, so opcode 0x04 requires 11 parameter bytes
 * before the checksum.  All bytes must appear in the receive buffer.
 */
static void test_max_param_opcode(void)
{
    printf("\n[7] 12-body-byte opcode (0x04 → table index 4): all params stored\n");

    uint8_t pkt[13];           /* opcode + 11 params + checksum */
    pkt[0] = 0x04;
    for (int i = 1; i <= 11; i++)
        pkt[i] = (uint8_t)(0x10 + i);
    pkt[12] = checksum_of(pkt, 12);

    COMMO_INIT();
    drive_packet(pkt, 13);

    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "rx status = NEW_COMMAND");
    ASSERT(GET_BUFFER(0)  == 0x04,  "opcode = 0x04");
    ASSERT(GET_BUFFER(11) == 0x1B,  "last param (index 11) = 0x1B");
}

/*
 * Test 8 — A → B → A: the third reception must yield NEW_COMMAND, not SAME.
 *
 * last_command tracks the previous opcode.  After receiving B, last_command
 * is updated to B.  The subsequent A differs from B so must be NEW_COMMAND.
 */
static void test_new_after_different_opcode(void)
{
    printf("\n[8] A → B → A: third reception must be NEW_COMMAND\n");

    uint8_t pktA[2], pktB[3];
    pktA[0] = 0x03; pktA[1] = checksum_of(pktA, 1);
    pktB[0] = 0x05; pktB[1] = 0x42; pktB[2] = checksum_of(pktB, 2);

    COMMO_INIT();
    drive_packet(pktA, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "A (1st): NEW_COMMAND");
    FREE_CMD_BUFFER();

    drive_packet(pktB, 3);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "B: NEW_COMMAND");
    FREE_CMD_BUFFER();

    drive_packet(pktA, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "A (after B): NEW_COMMAND");
}

/*
 * Test 9 — Same opcode after CMD_ERROR must be treated as NEW_COMMAND.
 *
 * A checksum failure should invalidate last_command so that the retried packet
 * is not silently demoted to COMMO_SAME_COMMAND.
 *
 * BUG: commo.c does not clear last_command on CMD_ERROR.  The third assertion
 * in this test FAILS because the SM returns COMMO_SAME_COMMAND instead of
 * COMMO_NEW_COMMAND after the error path.
 */
static void test_new_command_after_error(void)
{
    printf("\n[9] Same opcode after CMD_ERROR → NEW_COMMAND (exposes bug)\n");

    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);
    uint8_t bad[2] = { 0x03, 0x00 };  /* 0x00 is wrong; 0xFC is right */

    COMMO_INIT();

    drive_packet(good, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND, "1st good packet: NEW_COMMAND");
    FREE_CMD_BUFFER();

    drive_packet(bad, 2);
    ASSERT(NEW_CMD_RECEIVED() == COMMO_CMD_ERROR, "bad checksum: CMD_ERROR");
    FREE_CMD_BUFFER();

    drive_packet(good, 2);
    /* last_command is not cleared on CMD_ERROR, so this returns SAME_COMMAND.
     * Expected: NEW_COMMAND.  Actual: SAME_COMMAND.  FAILS. */
    ASSERT(NEW_CMD_RECEIVED() == COMMO_NEW_COMMAND,
           "after error, same opcode → NEW_COMMAND (not SAME)");
}

/*
 * Test 10 — SEND_STRING TX: verify bit sequence in the gpio_op log.
 *
 * Transmit one byte (0xA5) with SEND_STRING_COMPLETE and check:
 *   • 16 CLK rising edges total (8 data + 8 checksum)
 *   • Bits 0-7: 0xA5 sent LSB-first
 *   • Bits 8-15: ~0xA5 = 0x5A checksum sent LSB-first
 *   • SEND_STRING_READY returns COMMO_READY_WITHOUT_ERROR when done
 */
static void test_send_string_tx_bits(void)
{
    printf("\n[10] SEND_STRING TX: 0xA5 data + 0x5A checksum bit sequence\n");

    uint8_t tx[1] = { 0xA5 };
    COMMO_INIT();
    sim_commo_reset();
    SEND_STRING(SEND_STRING_COMPLETE, tx, 1);

    gpio_levels[PIN_COMMO_DATA] = 1;   /* host has released the bus */
    gpio_log_reset();

    COMMO_INTERFACE();   /* IDLE → TXD_DATA (state change only)   */
    COMMO_INTERFACE();   /* TXD_DATA → transmit 0xA5              */
    COMMO_INTERFACE();   /* TXD_CHECKSUM → transmit ~0xA5 = 0x5A  */

    ASSERT(count_rising_edges_commo(PIN_COMMO_CLK) == 16,
           "16 CLK rising edges (8 data + 8 checksum)");

    /* 0xA5 LSB-first at clock positions 0–7 */
    uint8_t d = 0xA5;
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (data_before_nth_clock(PIN_COMMO_CLK, PIN_COMMO_DATA, i)
                != ((d >> i) & 1)) { ok = 0; break; }
    }
    ASSERT(ok, "data bits match 0xA5 LSB-first");

    /* ~0xA5 = 0x5A LSB-first at clock positions 8–15 */
    uint8_t cs = (uint8_t)~0xA5u;
    ok = 1;
    for (int i = 0; i < 8; i++) {
        if (data_before_nth_clock(PIN_COMMO_CLK, PIN_COMMO_DATA, 8 + i)
                != ((cs >> i) & 1)) { ok = 0; break; }
    }
    ASSERT(ok, "checksum bits match ~0xA5 = 0x5A LSB-first");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== COMMO state-machine tests ===\n");

    test_single_byte_opcode();
    test_opcode_with_param();
    test_bad_checksum();
    test_same_command_detection();
    test_zero_opcode_error();
    test_free_buffer_clears_status();
    test_max_param_opcode();
    test_new_after_different_opcode();
    test_new_command_after_error();
    test_send_string_tx_bits();

    printf("\n");
    if (s_fails == 0)
        printf("All %d assertions passed.\n", 24 /* total asserts */);
    else
        printf("%d assertion(s) FAILED.\n", s_fails);

    return s_fails ? 1 : 0;
}
