/**
 * @file  commo_hal.h
 * @brief Hardware abstraction for the three I/O operations used by commo.c.
 *
 * Two implementations exist:
 *   upstream/core/commo_hal_pico.c  — real PIO + GPIO (linked in firmware)
 *   tests/host/commo_hal_stub.c     — test harness control (linked in host tests)
 */

#pragma once
#include <stdint.h>

/**
 * Arm the RX state machine, wait for the next byte from the host, and return it.
 * Returns 0 on timeout.
 */
uint8_t commo_hal_rxd(void);

/** Transmit one byte to the host via the TX state machine. */
void commo_hal_txd(uint8_t b);

/**
 * Returns 1 if the COMMO DATA line is low (host signalling a new byte), 0 otherwise.
 * In RX idle mode the CPU samples the DATA pin directly.
 */
int commo_hal_data_is_low(void);

/** Release the COMMO bus back to receive mode after a TX sequence completes. */
void commo_hal_release(void);
