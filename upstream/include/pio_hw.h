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
 *    SM0 — commo.pio             GPIO 15-17 IF_CLK / IF_DATA / IF_DIR  (RX)
 *    SM1 — commo.pio             GPIO 15-17 (shared)                    (TX)
 *
 * GPIO map (26-pin Commodore CD32 connector signal names):
 *   0  = PIN_DA_DATA    (conn 9  — I2S serial data out)
 *   1  = PIN_DA_BCLK    (conn 11 — I2S bit clock out)
 *   2  = PIN_DA_LRCLK   (conn 8  — I2S word-select out)
 *   3  = PIN_DA_C2PO    (conn 12 — C2 error pointer out)
 *   4  = PIN_DA_EMPH    (conn 13 — pre-emphasis flag out)
 *   5  = PIN_SUB_DATA   (conn 17 — subcode data out)
 *   6  = PIN_SUB_CLK    (conn 18 — subcode clock out)
 *   7  = PIN_SUB_WFCLK  (conn 14 — subcode word-frame clock out)
 *   8  = PIN_SUB_SCOR   (conn 15 — subcode sync correlator out)
 *   9  = PIN_M17SINE    (conn 5  — 16.9344 MHz master clock in)
 *  10  = PIN_ACTIVE     (conn 24 — drive active/spinning out)
 *  11  = PIN_DOOR       (conn 26 — door/tray switch in)
 *  12  = PIN_SCOR       (free GPIO — upstream SCOR IRQ compat, never fires in ODE)
 *  14  = PIN_RESET      (conn 7  — active-low /RESET from CD32 in)
 *  15  = PIN_IF_CLK     (conn 20 — COMMO clock,  alias PIN_COMMO_CLK)
 *  16  = PIN_IF_DATA    (conn 21 — COMMO data,   alias PIN_COMMO_DATA)
 *  17  = PIN_IF_DIR     (conn 25 — COMMO dir,    alias PIN_COMMO_DIR)
 */

#pragma once

#include "hardware/pio.h"
#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * PIO instance and SM index constants
 * ====================================================================== */

#define PIO_CXD       pio0
#define SM_CXD        0

#define PIO_DSIC_TX   pio0
#define SM_DSIC_TX    1

#define PIO_DSIC_RX   pio0
#define SM_DSIC_RX    2

#define PIO_QCHAN     pio0
#define SM_QCHAN      3

#define PIO_COMMO_RX  pio1
#define SM_COMMO_RX   0

#define PIO_COMMO_TX  pio1
#define SM_COMMO_TX   1

/* =========================================================================
 * Clock frequencies
 * ====================================================================== */

/** CXD2500BQ bit clock — original firmware used GPIO toggling at ~500 kHz */
#define CXD_BIT_FREQ_HZ    500000u

/** DSIC2 bit clock — original ~200 kHz with 150 µs latch settle */
#define DSIC_BIT_FREQ_HZ   200000u

/** Q-channel bit clock — needs to keep up with subcode frame rate (~7.35 kHz frames) */
#define QCHAN_BIT_FREQ_HZ  400000u

/** COMMO serial bit rate — original firmware used GPIO toggling, ~100 kHz */
#define COMMO_BIT_FREQ_HZ  100000u

/* =========================================================================
 * Global PIO program offsets (set during pio_hw_init)
 * ====================================================================== */

extern uint g_offset_cxd;
extern uint g_offset_dsic_tx;
extern uint g_offset_dsic_rx;
extern uint g_offset_qchan;
extern uint g_offset_commo_rx;
extern uint g_offset_commo_tx;

/* =========================================================================
 * Q-channel capture state (set by PIO IRQ handler)
 * ====================================================================== */

/** Set by PIO IRQ when a complete 10-byte Q-channel frame is ready */
extern volatile bool g_qchan_ready;

/* =========================================================================
 * API
 * ====================================================================== */

/**
 * @brief Initialise all PIO state machines.
 *
 * Must be called once, after clocks are initialised and before any
 * calls to the driver functions.
 */
void pio_hw_init(void);

/**
 * @brief Send one byte to the CXD2500BQ.
 *
 * Non-blocking: writes to the PIO FIFO (depth 4) and returns immediately
 * unless the FIFO is full, in which case it blocks until space is available.
 */
void pio_cxd_write(uint8_t data);

/**
 * @brief Write one byte to the DSIC2 IC (MSB first).
 *
 * Blocks until the byte has been shifted out and latched.
 */
void pio_dsic_write(uint8_t data);

/**
 * @brief Read one byte from the DSIC2 IC (MSB first).
 *
 * Switches SIDA to input, captures 8 bits, then reverts to output mode.
 */
uint8_t pio_dsic_read(void);

/**
 * @brief Start a Q-channel subcode frame capture.
 *
 * Called when the SCOR edge fires.  The PIO SM runs autonomously and
 * sets g_qchan_ready when all 10 bytes have been captured.
 */
void pio_qchan_start(void);

/**
 * @brief Drain the completed Q-channel frame into buf[10].
 *
 * Call only when g_qchan_ready is true.  Clears g_qchan_ready.
 */
void pio_qchan_drain(uint8_t *buf);

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
