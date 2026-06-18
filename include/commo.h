/**
 * @file  commo.h
 * @brief COMMO serial interface — command receive / status transmit.
 *
 * The CD32 host (Amiga chipset) communicates with the drive firmware over a
 * 3-wire serial bus: DATA (bi-directional), CLK (driven by host), DIR
 * (direction control driven by this firmware).
 *
 * Usage:
 *   commo_ctx_t ctx;
 *   commo_init(&ctx);
 *
 *   // main loop:
 *   commo_tick(&ctx);
 *   commo_cmd_t cmd;
 *   if (commo_cmd_pending(&ctx, &cmd)) {
 *       if (cmd.status == COMMO_CMD_NEW || cmd.status == COMMO_CMD_SAME) {
 *           uint8_t opc = cmd.bytes[0] & COMMO_OPCODE_MASK;
 *           // handle opcode...
 *       }
 *       commo_cmd_consumed(&ctx);
 *   }
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Opcode / index field masks (byte 0 of the received command)
 * ---------------------------------------------------------------------- */
#define COMMO_OPCODE_MASK   0x0Fu
#define COMMO_INDEX_MASK    0xF0u

/* -------------------------------------------------------------------------
 * Packet lengths
 * ---------------------------------------------------------------------- */
#define STATUS_PACKET_LENGTH  15
#define Q_PACKET_LENGTH       15
#define ID_PACKET_LENGTH      15

/* -------------------------------------------------------------------------
 * Command result status
 * ---------------------------------------------------------------------- */
typedef enum {
    COMMO_CMD_NONE  = 0,   /**< No completed command pending */
    COMMO_CMD_NEW   = 1,   /**< New opcode (differs from last accepted) */
    COMMO_CMD_SAME  = 2,   /**< Same opcode repeated */
    COMMO_CMD_ERROR = 3    /**< Checksum failure or null opcode */
} commo_cmd_status_t;

/** Filled by commo_cmd_pending(); valid while report_cmd is set. */
typedef struct {
    commo_cmd_status_t status;
    uint8_t            bytes[12];   /**< [0]=opcode, [1..11]=params */
} commo_cmd_t;

/* -------------------------------------------------------------------------
 * TX send mode
 * ---------------------------------------------------------------------- */
typedef enum {
    COMMO_SEND_APPEND   = 0,   /**< Append data without checksum */
    COMMO_SEND_COMPLETE = 1    /**< Append data then COMMO additive checksum */
} commo_send_mode_t;

/* -------------------------------------------------------------------------
 * State machine context — exposed for static allocation; treat as opaque.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t state;          /* internal sm_state_t stored as uint8_t */
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

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/** Initialise a COMMO context.  Call once before any other function. */
void commo_init(commo_ctx_t *ctx);

/**
 * Advance the state machine one step.
 * Must be called from the main loop every iteration.
 */
void commo_tick(commo_ctx_t *ctx);

/**
 * Returns true if a command result is available; fills *cmd with the status
 * and received bytes.  Idempotent — safe to call multiple times before
 * commo_cmd_consumed().
 */
bool commo_cmd_pending(commo_ctx_t *ctx, commo_cmd_t *cmd);

/** Mark the current command result consumed so the next can be reported. */
void commo_cmd_consumed(commo_ctx_t *ctx);

/**
 * Queue a packet for transmission.
 * Returns true if accepted; false if the TX channel is still busy.
 */
bool commo_send(commo_ctx_t *ctx, const uint8_t *data, uint8_t len,
                commo_send_mode_t mode);

/** Returns true when the TX interface is idle and ready to accept a packet. */
bool commo_send_ready(commo_ctx_t *ctx);
