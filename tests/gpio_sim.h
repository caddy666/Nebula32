/* gpio_sim.h — simulation helpers for host-native firmware tests.
 *
 * Provides:
 *   gpio_levels / gpio_dirs  — the fake GPIO register bank shared with
 *                              the inline stubs in hardware/gpio.h.
 *   Operation log            — records every gpio_put(pin, val) call so
 *                              tests can verify bit sequences after the fact.
 *   COMMO RX simulation      — queues bytes for bit-by-bit injection into
 *                              PIN_COMMO_DATA on each PIN_COMMO_CLK rising
 *                              edge, mirroring what the Amiga host would do.
 */
#pragma once
#include <stdint.h>

#define NUM_GPIOS 30

extern uint32_t gpio_levels[NUM_GPIOS];
extern uint32_t gpio_dirs[NUM_GPIOS];

/* ── Operation log ──────────────────────────────────────────────────────── */

typedef struct { uint32_t pin; uint32_t value; } gpio_op_t;

void      gpio_log_reset(void);
int       gpio_log_size(void);
gpio_op_t gpio_log_get(int idx);

/* ── COMMO RX simulation ─────────────────────────────────────────────────
 *
 * How it works:
 *   1. Queue bytes with sim_commo_queue_byte() before starting a receive.
 *   2. The gpio_put() hook fires on every PIN_COMMO_CLK rising edge and
 *      places the next bit into gpio_levels[PIN_COMMO_DATA].
 *   3. commo_get_data() (called immediately after each CLK=1 inside
 *      get_rxd_data()) reads that value from gpio_levels[DATA].
 *
 * Bits are injected LSB-first, matching the COMMO wire protocol.
 * ─────────────────────────────────────────────────────────────────────── */

void sim_commo_reset(void);
void sim_commo_queue_byte(uint8_t byte);
