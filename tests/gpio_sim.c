/* gpio_sim.c — GPIO simulation backend for host-native unit tests.
 *
 * Provides:
 *   gpio_levels[], gpio_dirs[]  — fake GPIO register bank.
 *   gpio_put()                  — intercepts writes; maintains the op log
 *                                 and injects COMMO DATA bits on CLK edges.
 *   Operation log               — ring of every gpio_put call.
 *   COMMO byte queue            — supplies bits to the DATA pin as CLK rises.
 */

#include "gpio_sim.h"
#include "gpio_map.h"   /* PIN_COMMO_CLK, PIN_COMMO_DATA */
#include <string.h>
#include <stdint.h>

/* ── GPIO state ─────────────────────────────────────────────────────────── */
uint32_t gpio_levels[NUM_GPIOS] = {0};
uint32_t gpio_dirs[NUM_GPIOS]   = {0};

/* ── Operation log ─────────────────────────────────────────────────────── */
#define LOG_CAP 1024
static gpio_op_t s_log[LOG_CAP];
static int       s_log_n = 0;

void gpio_log_reset(void)       { s_log_n = 0; }
int  gpio_log_size(void)        { return s_log_n; }
gpio_op_t gpio_log_get(int i)   { return s_log[i]; }

/* ── COMMO RX bit queue ─────────────────────────────────────────────────── */
#define Q_CAP 64
static uint8_t s_q[Q_CAP];
static int     s_qh = 0, s_qt = 0, s_bit = 0;

void sim_commo_reset(void) { s_qh = s_qt = s_bit = 0; }
void sim_commo_queue_byte(uint8_t b) { s_q[s_qt++ & (Q_CAP - 1)] = b; }

/* Place the next queued bit onto PIN_COMMO_DATA (LSB-first). */
static void inject_commo_bit(void)
{
    if (s_qh == s_qt) return;
    uint8_t b = s_q[s_qh & (Q_CAP - 1)];
    gpio_levels[PIN_COMMO_DATA] = (uint32_t)((b >> s_bit) & 1);
    if (++s_bit >= 8) { s_bit = 0; s_qh++; }
}

/* ── gpio_put — the single interception point ───────────────────────────
 *
 * Called by every firmware write to any GPIO pin.  Sets the level,
 * appends to the operation log, then fires the COMMO CLK hook so that
 * commo_get_data() — called by get_rxd_data() immediately after each
 * CLK=1 — reads the freshly injected DATA bit.
 * ────────────────────────────────────────────────────────────────────── */
void gpio_put(uint32_t pin, uint32_t v)
{
    gpio_levels[pin] = v;

    if (s_log_n < LOG_CAP) {
        s_log[s_log_n].pin   = pin;
        s_log[s_log_n].value = v;
        s_log_n++;
    }

    if (pin == PIN_COMMO_CLK && v == 1)
        inject_commo_bit();
}
