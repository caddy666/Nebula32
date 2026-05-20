/**
 * @file  commo.c
 * @brief COMMO serial interface state machine.
 *
 * Hardware I/O is isolated behind commo_hal.h:
 *   upstream/core/commo_hal_pico.c  — real PIO + GPIO (linked in firmware)
 *   tests/host/commo_hal_stub.c     — test-harness control (linked in host tests)
 *
 * PIO state machines used:
 *   PIO1 SM0 — commo_rx.pio   (host-clocked receive)
 *   PIO1 SM1 — commo_tx.pio   (self-clocked transmit)
 */

#include <stdint.h>
#include <string.h>

#include "commo.h"
#include "commo_hal.h"

/* =========================================================================
 * Internal state machine
 * ====================================================================== */

typedef enum {
    COMMO_SM_IDLE = 0,
    COMMO_SM_RXD_OPCODE,
    COMMO_SM_RXD_PARM,
    COMMO_SM_RXD_CHECKSUM,
    COMMO_SM_TXD_DATA,
    COMMO_SM_TXD_CHECKSUM,
    COMMO_SM_ERR_SEND
} commo_sm_state_t;

typedef struct {
    commo_sm_state_t state;

    uint8_t cmd_length;
    uint8_t checksum;
    uint8_t byte_counter;
    uint8_t byte_pointer;
    uint8_t rx_status;
    uint8_t last_command;

    uint8_t rx_buffer[12];
    uint8_t tx_buffer[16];
    uint8_t tx_length;

    uint8_t tx_req;
    uint8_t tx_chk_req;
    uint8_t report_cmd;
    uint8_t cmd_buf_free;
} commo_ctx_t;

static commo_ctx_t s_commo;

static const uint8_t command_length_table[16] = {
    1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1
};

/* =========================================================================
 * State machine step
 * ====================================================================== */

static void commo_step(commo_ctx_t *c)
{
    switch (c->state) {

    case COMMO_SM_IDLE:
        if (c->tx_req) {
            c->byte_pointer = 0;
            c->checksum     = 0;
            c->state        = COMMO_SM_TXD_DATA;
        } else if (commo_hal_data_is_low()) {
            c->state        = COMMO_SM_RXD_OPCODE;
            c->byte_counter = 0;
            c->checksum     = 0;
        }
        break;

    case COMMO_SM_RXD_OPCODE: {
        uint8_t b = commo_hal_rxd();
        if (b == 0) {
            c->state        = COMMO_SM_ERR_SEND;
            c->byte_counter = 128;
            break;
        }
        c->rx_buffer[0] = b;
        c->checksum     = b;
        c->byte_counter = 1;
        c->cmd_length   = command_length_table[b & 0x0Fu];
        c->state = (c->cmd_length == 1) ? COMMO_SM_RXD_CHECKSUM
                                        : COMMO_SM_RXD_PARM;
        break;
    }

    case COMMO_SM_RXD_PARM:
        if (!commo_hal_data_is_low()) break;
        {
            uint8_t b = commo_hal_rxd();
            c->rx_buffer[c->byte_counter++] = b;
            c->checksum += b;
            if (c->byte_counter >= c->cmd_length)
                c->state = COMMO_SM_RXD_CHECKSUM;
        }
        break;

    case COMMO_SM_RXD_CHECKSUM:
        if (!commo_hal_data_is_low()) break;
        {
            uint8_t rx = commo_hal_rxd();
            if ((uint8_t)~rx == c->checksum) {
                c->rx_status = (c->rx_buffer[0] == c->last_command)
                               ? COMMO_SAME_COMMAND
                               : COMMO_NEW_COMMAND;
                if (c->rx_status == COMMO_NEW_COMMAND)
                    c->last_command = c->rx_buffer[0];
            } else {
                c->rx_status  = COMMO_CMD_ERROR;
                c->last_command = 0;  // invalidate so retry is treated as NEW_COMMAND
            }
            c->report_cmd = 1;
            c->checksum   = 0;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    case COMMO_SM_TXD_DATA:
        /* commo_hal_data_is_low() reads gpio_get(PIN_COMMO_DATA).  During TX
         * the pin is driven high by commo_tx_send(), so this always returns
         * false and execution always falls through.  The check is a vestige of
         * the original bit-bang code where the host could abort mid-transfer;
         * it is harmless but intentionally left in place to preserve the SM
         * shape for bisect-ability. */
        if (commo_hal_data_is_low()) break;
        commo_hal_txd(c->tx_buffer[c->byte_pointer]);
        c->checksum += c->tx_buffer[c->byte_pointer];
        c->byte_pointer++;
        if (--c->byte_counter == 0) {
            c->tx_req = 0;
            c->state  = c->tx_chk_req ? COMMO_SM_TXD_CHECKSUM
                                      : COMMO_SM_IDLE;
            commo_hal_release();
        }
        break;

    case COMMO_SM_TXD_CHECKSUM:
        if (commo_hal_data_is_low()) break;
        commo_hal_txd((uint8_t)~c->checksum);
        c->tx_req     = 0;
        c->tx_chk_req = 0;
        c->checksum   = 0;
        c->state      = COMMO_SM_IDLE;
        commo_hal_release();
        break;

    case COMMO_SM_ERR_SEND:
        if (c->byte_counter > 0) {
            c->byte_counter--;
        } else {
            c->rx_status  = COMMO_CMD_ERROR;
            c->report_cmd = 1;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    default:
        c->state = COMMO_SM_IDLE;
        break;
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void COMMO_INIT(void)
{
    memset(&s_commo, 0, sizeof(s_commo));
    s_commo.state = COMMO_SM_IDLE;
    /* PIO pins are configured by pio_hw_init() called from driver_init() */
}

void COMMO_INTERFACE(void)
{
    commo_step(&s_commo);
}

uint8_t NEW_CMD_RECEIVED(void)
{
    if (!s_commo.report_cmd) return COMMO_NO_COMMAND;
    return s_commo.rx_status;
}

uint8_t GET_BUFFER(uint8_t idx)
{
    if (idx >= (uint8_t)sizeof(s_commo.rx_buffer)) return 0;
    return s_commo.rx_buffer[idx];
}

uint8_t SEND_STRING(uint8_t mode, uint8_t *data, uint8_t length)
{
    if (s_commo.tx_req)                                   return COMMO_FALSE;
    if (length > (uint8_t)sizeof(s_commo.tx_buffer))      return COMMO_FALSE;

    memcpy(s_commo.tx_buffer, data, length);
    s_commo.byte_counter = length;
    s_commo.tx_length    = length;
    s_commo.tx_chk_req   = (mode == SEND_STRING_COMPLETE) ? 1u : 0u;
    s_commo.tx_req       = 1;
    return COMMO_TRUE;
}

uint8_t SEND_STRING_READY(void)
{
    if (s_commo.state == COMMO_SM_TXD_DATA ||
        s_commo.state == COMMO_SM_TXD_CHECKSUM)
        return COMMO_BUSY;
    return COMMO_READY_WITHOUT_ERROR;
}

uint8_t FREE_CMD_BUFFER(void)
{
    s_commo.report_cmd   = 0;
    s_commo.cmd_buf_free = 1;
    return COMMO_TRUE;
}
