/**
 * @file  commo.c
 * @brief COMMO serial interface state machine.
 *
 * Hardware I/O is isolated behind commo_hal.h:
 *   src/commo_hal_pico.c  — real PIO + GPIO (linked in firmware)
 *   tests/host/commo_hal_stub.c     — test-harness control (linked in host tests)
 *
 * PIO state machines used:
 *   PIO1 SM0 — commo_rx.pio   (host-clocked receive)
 *   PIO1 SM1 — commo_tx.pio   (self-clocked transmit)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "commo.h"
#include "commo_hal.h"

/* =========================================================================
 * Internal state IDs — stored as uint8_t in commo_ctx_t.state
 * ====================================================================== */

typedef enum {
    SM_IDLE = 0,
    SM_RXD_OPCODE,
    SM_RXD_PARM,
    SM_RXD_CHECKSUM,
    SM_TXD_DATA,
    SM_TXD_CHECKSUM,
    SM_ERR_SEND
} sm_state_t;

/* Number of commo_step() ticks the SM dwells in SM_ERR_SEND before re-emitting
 * COMMO_CMD_ERROR.  Loaded into byte_counter on entry; counted down one per
 * tick (see SM_ERR_SEND). */
#define COMMO_ERR_SEND_TICKS  128u

static const uint8_t command_length_table[16] = {
    1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1
};

/* =========================================================================
 * State machine step
 * ====================================================================== */

static void commo_step(commo_ctx_t *c)
{
    switch ((sm_state_t)c->state) {

    case SM_IDLE:
        if (c->tx_req) {
            c->byte_pointer = 0;
            c->checksum     = 0;
            c->state        = SM_TXD_DATA;
        } else if (commo_hal_data_is_low()) {
            c->state        = SM_RXD_OPCODE;
            c->byte_counter = 0;
            c->checksum     = 0;
        }
        break;

    case SM_RXD_OPCODE: {
        uint8_t b;
        if (commo_hal_rxd(&b) == COMMO_HAL_RX_TIMEOUT || b == 0) {
            /* b==0: null opcode is a Chinon COMMO bus-reset signal, not a
             * valid command.  Treat it identically to a hardware timeout. */
            c->state        = SM_ERR_SEND;
            c->byte_counter = COMMO_ERR_SEND_TICKS;
            break;
        }
        c->rx_buffer[0] = b;
        c->checksum     = b;
        c->byte_counter = 1;
        c->cmd_length   = command_length_table[b & 0x0FU];
        c->state = (c->cmd_length == 1) ? SM_RXD_CHECKSUM : SM_RXD_PARM;
        break;
    }

    case SM_RXD_PARM:
        if (!commo_hal_data_is_low()) break;
        {
            uint8_t b;
            if (commo_hal_rxd(&b) == COMMO_HAL_RX_TIMEOUT) break;
            c->rx_buffer[c->byte_counter++] = b;
            c->checksum += b;
            if (c->byte_counter >= c->cmd_length)
                c->state = SM_RXD_CHECKSUM;
        }
        break;

    case SM_RXD_CHECKSUM:
        if (!commo_hal_data_is_low()) break;
        {
            uint8_t rx;
            if (commo_hal_rxd(&rx) == COMMO_HAL_RX_TIMEOUT) break;
            if ((uint8_t)~rx == c->checksum) {
                c->rx_status = (c->rx_buffer[0] == c->last_command)
                               ? (uint8_t)COMMO_CMD_SAME
                               : (uint8_t)COMMO_CMD_NEW;
                if (c->rx_status == (uint8_t)COMMO_CMD_NEW)
                    c->last_command = c->rx_buffer[0];
            } else {
                c->rx_status    = (uint8_t)COMMO_CMD_ERROR;
                c->last_command = 0;  /* invalidate so retry is treated as NEW */
            }
            c->report_cmd = 1;
            c->checksum   = 0;
            c->state      = SM_IDLE;
        }
        break;

    case SM_TXD_DATA:
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
            c->state  = c->tx_chk_req ? SM_TXD_CHECKSUM : SM_IDLE;
            commo_hal_release();
        }
        break;

    case SM_TXD_CHECKSUM:
        if (commo_hal_data_is_low()) break;
        commo_hal_txd((uint8_t)~c->checksum);
        c->tx_req     = 0;
        c->tx_chk_req = 0;
        c->checksum   = 0;
        c->state      = SM_IDLE;
        commo_hal_release();
        break;

    case SM_ERR_SEND:
        if (c->byte_counter > 0) {
            c->byte_counter--;
        } else {
            c->rx_status  = (uint8_t)COMMO_CMD_ERROR;
            c->report_cmd = 1;
            c->state      = SM_IDLE;
        }
        break;

    default:
        c->state = SM_IDLE;
        break;
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void commo_init(commo_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = SM_IDLE;
}

void commo_tick(commo_ctx_t *ctx)
{
    commo_step(ctx);
}

bool commo_cmd_pending(commo_ctx_t *ctx, commo_cmd_t *cmd)
{
    if (!ctx->report_cmd) return false;
    cmd->status = (commo_cmd_status_t)ctx->rx_status;
    memcpy(cmd->bytes, ctx->rx_buffer, sizeof(cmd->bytes));
    return true;
}

void commo_cmd_consumed(commo_ctx_t *ctx)
{
    ctx->report_cmd   = 0;
    ctx->cmd_buf_free = 1;
}

bool commo_send(commo_ctx_t *ctx, const uint8_t *data, uint8_t len,
                commo_send_mode_t mode)
{
    if (ctx->tx_req)                             return false;
    if (len > (uint8_t)sizeof(ctx->tx_buffer))   return false;
    memcpy(ctx->tx_buffer, data, len);
    ctx->byte_counter = len;
    ctx->tx_length    = len;
    ctx->tx_chk_req   = (mode == COMMO_SEND_COMPLETE) ? 1U : 0U;
    ctx->tx_req       = 1;
    return true;
}

bool commo_send_ready(commo_ctx_t *ctx)
{
    return ctx->state != SM_TXD_DATA && ctx->state != SM_TXD_CHECKSUM;
}
