/**
 * @file  pio_hw.h
 * @brief PIO state machine handles and initialisation for all serial buses.
 *
 * PIO allocation (ODE build):
 *
 *  PIO0:
 *    SM0 — da_output.pio         GPIO 0-2   DA_DATA / DA_BCLK / DA_LRCLK
 *    SM1 — subcode_encoder.pio   GPIO 5-8   SUB_DATA / SUB_CLK / SUB_WFCLK / SUB_SCOR
 *
 *  PIO1:
 *    SM0 — commo.pio             GPIO 44-46 IF_CLK / IF_DATA / IF_DIR  (RX)
 *    SM1 — commo.pio             GPIO 44-46 (shared)                    (TX)
 *    SM2/3 — SDIO (library)      GPIO 30-35 (via pio_claim_unused_sm)
 *
 * GPIO map — Core2350B0 (RP2350B):
 *   0  = PIN_DA_DATA    (conn 9  — I2S serial data out)
 *   1  = PIN_DA_BCLK    (conn 11 — I2S bit clock out)
 *   2  = PIN_DA_LRCLK   (conn 8  — I2S word-select out)
 *   3  = PIN_DA_C2PO    (conn 12 — C2 error pointer out)
 *   4  = PIN_DA_EMPH    (conn 13 — pre-emphasis flag out)
 *   5  = PIN_SUB_DATA   (conn 17 — subcode data out)
 *   6  = PIN_SUB_CLK    (conn 18 — subcode clock out)
 *   7  = PIN_SUB_WFCLK  (conn 14 — subcode word-frame clock out)
 *   8  = PIN_SUB_SCOR / PIN_SCOR  (conn 15 — SCOR out)
 *   9  = PIN_M17SINE    (conn 5  — 16.9344 MHz master clock in, GPIN0)
 *  10  = PIN_ACTIVE     (conn 24 — drive active/spinning out)
 *  11  = PIN_DOOR       (conn 26 — door/tray switch in)
 *  14  = PIN_RESET      (conn 7  — active-low /RESET from CD32 in)
 *  16  = PIN_UART0_TX   (debug serial TX — stdio mirror)
 *  17  = PIN_UART0_RX   (debug serial RX)
 *  20  = PIN_UART1_TX   (auxiliary serial TX)
 *  21  = PIN_UART1_RX   (auxiliary serial RX)
 *  44  = PIN_IF_CLK     (conn 20 — COMMO clock,  alias PIN_COMMO_CLK)
 *  45  = PIN_IF_DATA    (conn 21 — COMMO data,   alias PIN_COMMO_DATA)
 *  46  = PIN_IF_DIR     (conn 25 — COMMO dir,    alias PIN_COMMO_DIR)
 */

#pragma once

#include "hardware/pio.h"
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * PIO instance and SM index constants
 * ====================================================================== */

#define PIO_COMMO_RX  pio1
#define SM_COMMO_RX   0

#define PIO_COMMO_TX  pio1
#define SM_COMMO_TX   1

/* =========================================================================
 * Clock frequencies
 * ====================================================================== */

/** COMMO serial bit rate — original firmware used GPIO toggling, ~100 kHz */
#define COMMO_BIT_FREQ_HZ  100000u

/* =========================================================================
 * Global PIO program offsets (set during commo_bridge_init)
 * ====================================================================== */

extern uint g_offset_commo_rx;
extern uint g_offset_commo_tx;

/* =========================================================================
 * API
 * ====================================================================== */

/**
 * @brief Start listening for an incoming COMMO byte.
 *
 * Enables the RX state machine.  Returns true if a byte was received
 * within the timeout (non-blocking — call repeatedly from main loop).
 */
bool pio_commo_rx_ready(void);

/**
 * @brief Retrieve the received byte (call after pio_commo_rx_ready returns true).
 */
uint8_t pio_commo_rx_get(void);

/**
 * @brief Transmit one byte over the COMMO bus.
 *
 * Blocks until the byte has been fully clocked out.
 */
void pio_commo_tx_byte(uint8_t data);

/**
 * @brief Release the COMMO bus back to receive mode after transmission.
 */
void pio_commo_release(void);
