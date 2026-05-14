/**
 * @file  pio_hw.h
 * @brief PIO state machine handles and initialisation for all serial buses.
 *
 * Resource allocation (RP2350 has 3 PIO blocks × 4 SMs = 12 SMs total):
 *
 *  PIO0:
 *    SM0 — CXD2500BQ TX      (cxd2500_tx.pio)   pins 2-4
 *    SM1 — DSIC2 TX          (dsic2.pio)         pins 5-7
 *    SM2 — DSIC2 RX          (dsic2.pio)         pins 5-7
 *    SM3 — Q-channel RX      (qchannel_rx.pio)   pins 8-9
 *
 *  PIO1:
 *    SM0 — COMMO RX          (commo.pio)         pins 15-17
 *    SM1 — COMMO TX          (commo.pio)         pins 15-17
 *
 * GPIO map (PIO-optimised, consecutive pin groupings):
 *   2  = PIN_CXD_CLK    (UCL)
 *   3  = PIN_CXD_DATA   (UDAT)
 *   4  = PIN_CXD_LAT    (ULAT)
 *   5  = PIN_DSIC_CLK   (SICL)
 *   6  = PIN_DSIC_DATA  (SIDA)
 *   7  = PIN_DSIC_LAT   (SILD)
 *   8  = PIN_QCL        (Q-channel clock out)
 *   9  = PIN_QDA        (Q-channel data in)
 *  10  = PIN_HF_DET     (HF detector, input)
 *  11  = PIN_DOOR       (Door switch, input)
 *  12  = PIN_SCOR       (Subcode clock interrupt, input)
 *  15  = PIN_COMMO_CLK  (COMMO clock)
 *  16  = PIN_COMMO_DATA (COMMO data)
 *  17  = PIN_COMMO_DIR  (COMMO direction)
 *  25  = PIN_LED_STATUS (onboard LED)
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
