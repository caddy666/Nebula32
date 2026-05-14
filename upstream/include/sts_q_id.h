/**
 * @file  sts_q_id.h
 * @brief Status / Q-channel / ID packet buffer module.
 */

#pragma once
#include <stdint.h>

/* Update status codes */
#define NO_UPDATE       0x00
#define STATUS_UPDATE   0x01
#define Q_READY         0x02
#define ID_READY        0x03
#define DISPATCH_STATUS 0x04

/* Error condition codes */
#define CHECKSUM_ERROR  0x01

/* Error detect mask */
#define ERROR_DETECT_MASK  0x10

/** Return a pointer to the current status/Q/ID packet buffer. */
uint8_t *Get_sts_q_id_ptr(void);

/** Return the current update-status code. */
uint8_t Get_update_status(void);

/** Store a new update-status code. */
void Store_update_status(uint8_t status);

/** Clear the pending update flag. */
void Clear_update(void);

/** Store the most recent opcode in the status packet. */
void Store_command(uint8_t cmd);

/** Store an error condition byte in the status packet. */
void Store_error_condition(uint8_t err);
