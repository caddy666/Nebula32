/**
 * @file  commo_hal_stub.c
 * @brief Test-harness implementation of commo_hal.h.
 *
 * commo_hal_rxd()      — drain from a pre-loaded byte queue
 * commo_hal_txd()      — append to a capture log
 * commo_hal_data_is_low() — return the value set by commo_hal_stub_set_data_low()
 * commo_hal_release()  — no-op
 */

#include "commo_hal.h"
#include "commo_hal_stub.h"
#include <string.h>
#include <stdint.h>

static uint8_t s_rx_queue[64];
static int     s_rx_head  = 0;
static int     s_rx_tail  = 0;
static int     s_data_low = 0;

static uint8_t s_tx_log[32];
static int     s_tx_count = 0;

/* ---- control API ---- */

void commo_hal_stub_reset(void)
{
    s_rx_head = s_rx_tail = 0;
    s_data_low = 0;
    s_tx_count = 0;
    memset(s_rx_queue, 0, sizeof(s_rx_queue));
    memset(s_tx_log,   0, sizeof(s_tx_log));
}

void commo_hal_stub_push(uint8_t b)
{
    if (s_rx_tail < (int)sizeof(s_rx_queue))
        s_rx_queue[s_rx_tail++] = b;
}

void commo_hal_stub_set_data_low(int val) { s_data_low = val; }

int     commo_hal_stub_tx_count(void)  { return s_tx_count; }

uint8_t commo_hal_stub_tx_byte(int i)
{
    if (i < 0 || i >= s_tx_count) return 0;
    return s_tx_log[i];
}

/* ---- HAL implementation ---- */

uint8_t commo_hal_rxd(void)
{
    if (s_rx_head >= s_rx_tail) return 0;
    return s_rx_queue[s_rx_head++];
}

void commo_hal_txd(uint8_t b)
{
    if (s_tx_count < (int)sizeof(s_tx_log))
        s_tx_log[s_tx_count++] = b;
}

int commo_hal_data_is_low(void) { return s_data_low; }

void commo_hal_release(void) {}
