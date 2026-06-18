/**
 * @file  commo_hal.h
 * @brief Hardware abstraction for the three I/O operations used by commo.c.
 *
 * Two implementations exist:
 *   src/commo_hal_pico.c        — real PIO + GPIO (linked in firmware)
 *   tests/host/commo_hal_stub.c — test harness control (linked in host tests)
 */

#pragma once
#include <stdint.h>

typedef enum {
    COMMO_HAL_RX_OK      = 0,
    COMMO_HAL_RX_TIMEOUT = 1
} commo_hal_rx_status_t;

/**
 * Arm the RX state machine and wait for the next byte from the host.
 * On success writes the byte to *out and returns COMMO_HAL_RX_OK.
 * Returns COMMO_HAL_RX_TIMEOUT (and leaves *out unmodified) on timeout.
 */
commo_hal_rx_status_t commo_hal_rxd(uint8_t *out);

/** Transmit one byte to the host via the TX state machine. */
void commo_hal_txd(uint8_t b);

/**
 * Returns 1 if the COMMO DATA line is low (host signalling a new byte), 0 otherwise.
 * In RX idle mode the CPU samples the DATA pin directly.
 */
int commo_hal_data_is_low(void);

/** Release the COMMO bus back to receive mode after a TX sequence completes. */
void commo_hal_release(void);
