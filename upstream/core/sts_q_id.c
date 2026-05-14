/**
 * @file  sts_q_id.c
 * @brief Status / Q-channel / ID packet buffer module.
 *
 * Maintains a 15-byte packet buffer that the Dispatcher can request to be
 * sent over the COMMO interface.  The buffer holds either:
 *   - A status packet  (drive state + last command echo)
 *   - A Q-channel packet (subcode Q data)
 *   - An ID packet      (disc ID / ISRC)
 */

#include <stdint.h>
#include <string.h>

#include "sts_q_id.h"

/* =========================================================================
 * Packet buffer
 * ====================================================================== */
#define PACKET_BUF_SIZE  15

static uint8_t  s_packet[PACKET_BUF_SIZE];
static uint8_t  s_update_status = NO_UPDATE;

/* =========================================================================
 * Public API
 * ====================================================================== */

uint8_t *Get_sts_q_id_ptr(void)
{
    return s_packet;
}

uint8_t Get_update_status(void)
{
    return s_update_status;
}

void Store_update_status(uint8_t status)
{
    s_update_status = status;
}

void Clear_update(void)
{
    s_update_status = NO_UPDATE;
}

void Store_command(uint8_t cmd)
{
    /* Byte 0 of the status packet carries the echoed command opcode */
    s_packet[0] = cmd;
}

void Store_error_condition(uint8_t err)
{
    /* Byte 1 carries the error/condition byte */
    s_packet[1] = err | ERROR_DETECT_MASK;
}
