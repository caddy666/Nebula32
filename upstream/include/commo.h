/**
 * @file  commo.h
 * @brief COMMO serial interface — command receive / status transmit.
 *
 * The CD32 host (Amiga chipset) communicates with the drive firmware over a
 * 3-wire serial bus: DATA (bi-directional), CLK (driven by host), DIR
 * (direction control driven by this firmware).
 */

#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Return values used throughout the dispatcher
 * ---------------------------------------------------------------------- */
#define COMMO_FALSE               0x00
#define COMMO_TRUE                0x01
#define COMMO_ERROR               0x02

#define COMMO_READY_WITHOUT_ERROR 0x00
#define COMMO_READY_WITH_ERROR    0x01
#define COMMO_BUSY                0x03
#define COMMO_PENDING             0x04

#define COMMO_NO_COMMAND          0x00
#define COMMO_NEW_COMMAND         0x01
#define COMMO_SAME_COMMAND        0x02
#define COMMO_CMD_ERROR           0x03

/* -------------------------------------------------------------------------
 * Opcode masks
 * ---------------------------------------------------------------------- */
#define COMMO_OPCODE_MASK   0x0F
#define COMMO_INDEX_MASK    0xF0

/* -------------------------------------------------------------------------
 * SEND_STRING modes
 * ---------------------------------------------------------------------- */
#define SEND_STRING_COMPLETE  1   /**< Include checksum byte */
#define SEND_STRING_APPEND    0   /**< Append without checksum */

/* -------------------------------------------------------------------------
 * Packet lengths
 * ---------------------------------------------------------------------- */
#define STATUS_PACKET_LENGTH  15
#define Q_PACKET_LENGTH       15
#define ID_PACKET_LENGTH      15

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/** Initialise the COMMO hardware and state machine. */
void COMMO_INIT(void);

/**
 * @brief  Service the COMMO state machine — one step per call.
 *
 * Must be called from the main loop every iteration.
 */
void COMMO_INTERFACE(void);

/** @return COMMO_NEW_COMMAND / COMMO_SAME_COMMAND / COMMO_CMD_ERROR / COMMO_NO_COMMAND */
uint8_t NEW_CMD_RECEIVED(void);

/** Return the byte at index @p idx in the receive buffer. */
uint8_t GET_BUFFER(uint8_t idx);

/**
 * @brief  Queue a packet for transmission.
 * @param  mode    SEND_STRING_COMPLETE or SEND_STRING_APPEND
 * @param  data    Pointer to the packet bytes
 * @param  length  Number of bytes
 * @return COMMO_TRUE if accepted, COMMO_FALSE otherwise.
 */
uint8_t SEND_STRING(uint8_t mode, uint8_t *data, uint8_t length);

/** @return COMMO_READY_WITHOUT_ERROR / COMMO_READY_WITH_ERROR / COMMO_BUSY */
uint8_t SEND_STRING_READY(void);

/** Release the COMMO command buffer after the command has been processed. */
uint8_t FREE_CMD_BUFFER(void);
