/**
 * @file  cmd_hndl.h
 * @brief Command handler — translates COMMO opcodes into player commands.
 */

#pragma once
#include <stdint.h>

/* Playback state values */
#define OPEN_S    0x00
#define STOP_S    0x01
#define PLAY_S    0x02
#define PAUSE_S   0x03

/* Command handler opcodes (internal) */
#define RESEND          0x80
#define S_STAT          0x81
#define S_CMD_ERR       0x82
#define S_ID            0x83
#define LED_CNTRL       0x84
#define SET_ERROR_STATUS 0x85
#define SEND_AUTO_Q     0x86
#define SEND_Q          0x87
#define S_DISK_ERR      0x88
#define S_CLOSED        0x89

/* Play state sub-commands */
#define START_PAUSE  0x00
#define START_PLAY   0x01
#define MODIFY_PLAY  0x02
#define STOP_C       0x03
#define PLAY_PAUSE   0x04
#define NEW_PLAY     0x05
#define SEEK_PLAY    0x06
#define SEEK_STOP    0x07
#define PAUSE_PLAY   0x08
#define ENT_DIA      0x09
#define DIA          0x0A
#define SEEK_PAUSE   0x0B
#define OPEN_C       0x0C
#define AREA_ERROR   0x0D

/* Miscellaneous */
#define RESEND_REQ  0x80
#define AUTO_Q      0x06
#define MS_80       10
#define IDLE        0xFF
#define NO_CMD      0x00
#define STOPPED_CLOSED 0x01
#define STOPPED_OPEN   0x00

/** Initialise the command handler module. */
void Init_command_handler(void);

/**
 * @brief  Dispatcher — arbitrates COMMO transmits and routes received commands.
 *
 * Defined in upstream/core/dispatcher.c.  Declared here (rather than a
 * separate dispatcher.h) because it is always used alongside the command
 * handler.  Call once per main-loop iteration before command_handler().
 */
void Dispatcher(void);

/** Run the command handler — call once per main-loop iteration. */
void command_handler(void);

/** Report a new command to the player.  @return COMMO_TRUE if accepted. */
uint8_t New_command(void);

/** @return TRUE if the command handler can accept another command. */
uint8_t Cmd_acception_status(void);

/** @return TRUE if the command handler is idle (ready). */
uint8_t Command_handler_ready(void);
