/**
 * @file  pio_hw.c
 * @brief PIO state machine initialisation and runtime API.
 *
 * Loads all four PIO programs, initialises the six state machines,
 * and provides clean C wrappers that replace every bit-bang GPIO
 * operation in driver.c and commo.c.
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Generated headers from pioasm (cmake generates these) */
#include "cxd2500_tx.pio.h"
#include "dsic2.pio.h"
#include "qchannel_rx.pio.h"
#include "commo.pio.h"

#include "pio_hw.h"
#include "gpio_map.h"

/* =========================================================================
 * Program offsets — set during pio_hw_init()
 * ====================================================================== */

uint g_offset_cxd      = 0;
uint g_offset_dsic_tx  = 0;
uint g_offset_dsic_rx  = 0;
uint g_offset_qchan    = 0;
uint g_offset_commo_rx = 0;
uint g_offset_commo_tx = 0;

/* =========================================================================
 * Q-channel capture state
 * ====================================================================== */

volatile bool g_qchan_ready = false;

/* =========================================================================
 * PIO IRQ handlers
 * ====================================================================== */

/**
 * PIO0 IRQ — fired when Q-channel capture SM signals completion (IRQ 0).
 */
static void pio0_irq_handler(void)
{
    if (pio_interrupt_get(PIO_QCHAN, 0)) {
        pio_interrupt_clear(PIO_QCHAN, 0);
        pio_sm_set_enabled(PIO_QCHAN, SM_QCHAN, false);
        g_qchan_ready = true;
    }
}

/**
 * PIO1 IRQ — fired when COMMO TX SM signals byte sent (IRQ 1) or
 * COMMO RX SM signals byte received (IRQ 0).
 * The main-loop polling approach in commo.c means we only need to
 * clear the flags here; the SM disables itself.
 */
static void pio1_irq_handler(void)
{
    /* Clear any pending IRQ flags; commo.c polls via pio_commo_rx_ready() */
    if (pio_interrupt_get(PIO_COMMO_RX, 0))
        pio_interrupt_clear(PIO_COMMO_RX, 0);
    if (pio_interrupt_get(PIO_COMMO_TX, 1))
        pio_interrupt_clear(PIO_COMMO_TX, 1);
}

/* =========================================================================
 * pio_hw_init
 * ====================================================================== */

void pio_hw_init(void)
{
    /* ----------------------------------------------------------------
     * PIO0 programs
     * -------------------------------------------------------------- */

    /* CXD2500BQ TX */
    g_offset_cxd = pio_add_program(PIO_CXD, &cxd2500_tx_program);
    cxd2500_tx_program_init(PIO_CXD, SM_CXD, g_offset_cxd,
                             PIN_CXD_CLK, CXD_BIT_FREQ_HZ);

    /* DSIC2 TX + RX (share the same pin group) */
    g_offset_dsic_tx = pio_add_program(PIO_DSIC_TX, &dsic2_tx_program);
    g_offset_dsic_rx = pio_add_program(PIO_DSIC_RX, &dsic2_rx_program);
    dsic2_program_init(PIO_DSIC_TX, SM_DSIC_TX, SM_DSIC_RX,
                       g_offset_dsic_tx, g_offset_dsic_rx,
                       PIN_DSIC_CLK, DSIC_BIT_FREQ_HZ);

    /* Q-channel RX */
    g_offset_qchan = pio_add_program(PIO_QCHAN, &qchannel_rx_program);
    qchannel_rx_program_init(PIO_QCHAN, SM_QCHAN, g_offset_qchan,
                              PIN_QCL, QCHAN_BIT_FREQ_HZ);

    /* Enable PIO0 IRQ for Q-channel completion */
    pio_set_irq0_source_enabled(PIO_QCHAN, pis_interrupt0, true);
    irq_set_exclusive_handler(PIO0_IRQ_0, pio0_irq_handler);
    irq_set_enabled(PIO0_IRQ_0, true);

    /* ----------------------------------------------------------------
     * PIO1 programs
     * -------------------------------------------------------------- */

    g_offset_commo_rx = pio_add_program(PIO_COMMO_RX, &commo_rx_program);
    g_offset_commo_tx = pio_add_program(PIO_COMMO_TX, &commo_tx_program);
    commo_program_init(PIO_COMMO_RX, SM_COMMO_RX, SM_COMMO_TX,
                       g_offset_commo_rx, g_offset_commo_tx,
                       PIN_COMMO_CLK, COMMO_BIT_FREQ_HZ);

    /* Enable PIO1 IRQ for COMMO events */
    pio_set_irq0_source_enabled(PIO_COMMO_RX, pis_interrupt0, true);
    pio_set_irq0_source_enabled(PIO_COMMO_TX, pis_interrupt1, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, pio1_irq_handler);
    irq_set_enabled(PIO1_IRQ_0, true);
}

/* =========================================================================
 * CXD2500BQ
 * ====================================================================== */

void pio_cxd_write(uint8_t data)
{
    cxd2500_pio_write(PIO_CXD, SM_CXD, data);
}

/* =========================================================================
 * DSIC2
 * ====================================================================== */

void pio_dsic_write(uint8_t data)
{
    dsic2_pio_write(PIO_DSIC_TX, SM_DSIC_TX, data);
    /* Wait for TX FIFO to drain (SM shifts at DSIC_BIT_FREQ_HZ) */
    while (!pio_sm_is_tx_fifo_empty(PIO_DSIC_TX, SM_DSIC_TX))
        tight_loop_contents();
    /* 150 µs latch settle */
    sleep_us(150);
}

uint8_t pio_dsic_read(void)
{
    uint8_t v = dsic2_pio_read(PIO_DSIC_TX, SM_DSIC_TX, SM_DSIC_RX, PIN_DSIC_DATA);
    sleep_us(150);
    return v;
}

/* =========================================================================
 * Q-channel
 * ====================================================================== */

void pio_qchan_start(void)
{
    g_qchan_ready = false;
    qchannel_start_capture(PIO_QCHAN, SM_QCHAN);
}

void pio_qchan_drain(uint8_t *buf)
{
    qchannel_drain(PIO_QCHAN, SM_QCHAN, buf);
    g_qchan_ready = false;
}

/* =========================================================================
 * COMMO
 * ====================================================================== */

bool pio_commo_rx_ready(void)
{
    /* SM_COMMO_RX must be enabled first; check if RX FIFO has data */
    return !pio_sm_is_rx_fifo_empty(PIO_COMMO_RX, SM_COMMO_RX);
}

uint8_t pio_commo_rx_get(void)
{
    uint32_t raw = pio_sm_get_blocking(PIO_COMMO_RX, SM_COMMO_RX);
    pio_sm_set_enabled(PIO_COMMO_RX, SM_COMMO_RX, false);
    return (uint8_t)(raw & 0xFFu);
}

void pio_commo_tx_byte(uint8_t data)
{
    commo_tx_send(PIO_COMMO_RX, SM_COMMO_RX, SM_COMMO_TX,
                  PIN_COMMO_CLK, data);
    /* Wait for TX SM to finish (IRQ 1 fires, but we poll for simplicity) */
    while (!pio_sm_is_tx_fifo_empty(PIO_COMMO_TX, SM_COMMO_TX))
        tight_loop_contents();
    /* Extra half-bit settle */
    sleep_us(5);
}

void pio_commo_release(void)
{
    /* Stop TX SM, switch pins back to input, re-arm RX SM */
    pio_sm_set_enabled(PIO_COMMO_TX, SM_COMMO_TX, false);
    commo_rx_enable(PIO_COMMO_RX, SM_COMMO_RX, PIN_COMMO_CLK);
}
