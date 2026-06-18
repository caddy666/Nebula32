/**
 * @file  commo_hal_stub.c
 * @brief Test-harness implementation of commo_hal.h.
 *
 * commo_hal_rxd()         — drain from a pre-loaded byte queue; TIMEOUT when empty
 * commo_hal_txd()         — append to a capture log
 * commo_hal_data_is_low() — queue non-empty OR manual override via set_data_low()
 * commo_hal_release()     — increments release counter (observable by tests)
 */

#include "commo_hal.h"
#include "commo_hal_stub.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static uint8_t s_rx_queue[64];
static int     s_rx_head  = 0;
static int     s_rx_tail  = 0;
static int     s_data_low = 0;

static uint8_t s_tx_log[32];
static int     s_tx_count = 0;

static int  s_release_count = 0;
static bool s_rx_overflow   = false;
static bool s_tx_overflow   = false;

/* ---- control API ---- */

void commo_hal_stub_reset(void)
{
    s_rx_head = s_rx_tail = 0;
    s_data_low    = 0;
    s_tx_count    = 0;
    s_release_count = 0;
    s_rx_overflow   = false;
    s_tx_overflow   = false;
    memset(s_rx_queue, 0, sizeof(s_rx_queue));
    memset(s_tx_log,   0, sizeof(s_tx_log));
}

void commo_hal_stub_push(uint8_t b)
{
    if (s_rx_tail >= (int)sizeof(s_rx_queue)) {
        s_rx_overflow = true;
        return;
    }
    s_rx_queue[s_rx_tail++] = b;
}

void    commo_hal_stub_set_data_low(int val) { s_data_low = val; }
int     commo_hal_stub_tx_count(void)        { return s_tx_count; }
int     commo_hal_stub_release_count(void)   { return s_release_count; }
bool    commo_hal_stub_rx_overflow(void)     { return s_rx_overflow; }
bool    commo_hal_stub_tx_overflow(void)     { return s_tx_overflow; }

uint8_t commo_hal_stub_tx_byte(int i)
{
    if (i < 0 || i >= s_tx_count) return 0;
    return s_tx_log[i];
}

/* ---- HAL implementation ---- */

commo_hal_rx_status_t commo_hal_rxd(uint8_t *out)
{
    if (s_rx_head >= s_rx_tail) return COMMO_HAL_RX_TIMEOUT;
    *out = s_rx_queue[s_rx_head++];
    return COMMO_HAL_RX_OK;
}

void commo_hal_txd(uint8_t b)
{
    if (s_tx_count >= (int)sizeof(s_tx_log)) {
        s_tx_overflow = true;
        return;
    }
    s_tx_log[s_tx_count++] = b;
}

/* data_is_low reflects real hardware: asserted whenever the host has a byte
 * ready to send (queue non-empty) OR the manual override flag is set.
 * Adversarial tests (bus glitch during TX, etc.) use set_data_low() directly. */
int commo_hal_data_is_low(void)
{
    return s_data_low || (s_rx_head < s_rx_tail);
}

void commo_hal_release(void) { s_release_count++; }
