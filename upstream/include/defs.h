/**
 * @file  defs.h
 * @brief Master type definitions and command opcodes for the CD32 Pico firmware.
 *
 * Ported from the original Commodore/Philips 8051 firmware (1992-1993).
 * Adapted for the RP2350 (Raspberry Pi Pico 2).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Basic types (replaces 8051 'bit' and CMOS byte)
 * ---------------------------------------------------------------------- */
typedef uint8_t  byte;
typedef uint8_t  Byte;

/* 'rom' was the 8051 keyword for const data in code memory — map to const */
#undef  rom
#define rom  const

/* -------------------------------------------------------------------------
 * TOC limits
 * ---------------------------------------------------------------------- */
#define MAX_TRACK_STORED_IN_TOC   20

/* -------------------------------------------------------------------------
 * Interface field: shared between the host (Amiga CD32) and this firmware
 * ---------------------------------------------------------------------- */
typedef struct {
    byte p_status;   /**< Player status (BUSY / READY / ERROR)  */
    byte a_command;  /**< Application command opcode             */
    byte param1;     /**< Command parameter 1                    */
    byte param2;     /**< Command parameter 2                    */
    byte param3;     /**< Command parameter 3                    */
} interface_field_t;

/* -------------------------------------------------------------------------
 * Normal-mode command opcodes (a_command values)
 * ---------------------------------------------------------------------- */
#define TRAY_OUT_OPC              0x00
#define TRAY_IN_OPC               0x01
#define START_UP_OPC              0x02
#define STOP_OPC                  0x03
#define PLAY_TRACK_OPC            0x04
#define PAUSE_ON_OPC              0x05
#define PAUSE_OFF_OPC             0x06
#define SEEK_OPC                  0x07
#define READ_TOC_OPC              0x08
#define READ_SUBCODE_OPC          0x09
#define SINGLE_SPEED_OPC          0x0A
#define DOUBLE_SPEED_OPC          0x0B
#define SET_VOLUME_OPC            0x0C
#define JUMP_TRACKS_OPC           0x0D
#define ENTER_SERVICE_MODE_OPC    0x0E

/* -------------------------------------------------------------------------
 * Service-mode command opcodes
 * ---------------------------------------------------------------------- */
#define ENTER_NORMAL_MODE_OPC     0x0F
#define LASER_ON_OPC              0x10
#define LASER_OFF_OPC             0x11
#define FOCUS_ON_OPC              0x12
#define FOCUS_OFF_OPC             0x13
#define SPINDLE_MOTOR_ON_OPC      0x14
#define SPINDLE_MOTOR_OFF_OPC     0x15
#define RADIAL_ON_OPC             0x16
#define RADIAL_OFF_OPC            0x17
#define MOVE_SLEDGE_OPC           0x18
#define JUMP_GROOVES_OPC          0x19
#define WRITE_CD6_OPC             0x1A
#define WRITE_DSIC2_OPC           0x1B
#define READ_DSIC2_OPC            0x1C

/* -------------------------------------------------------------------------
 * Internal opcodes / limits
 * ---------------------------------------------------------------------- */
#define IDLE_OPC                  0xFF
#define ERROR_HANDLING_ID         0x00
#define MAX_LEGAL_NORMAL_ID       0x0F
#define MAX_LEGAL_SERVICE_ID      0x1D

/* -------------------------------------------------------------------------
 * Tray module commands
 * ---------------------------------------------------------------------- */
#define TRAY_IDLE    0x00
#define TRAY_OUT     0x01
#define TRAY_IN      0x02

/* -------------------------------------------------------------------------
 * Start/stop module commands
 * ---------------------------------------------------------------------- */
#define SS_IDLE       0x00
#define SS_STOP       0x01
#define SS_START_UP   0x02
#define SS_SPEED_N1   0x03
#define SS_SPEED_N2   0x04
#define SS_MOTOR_OFF  0x05

/* -------------------------------------------------------------------------
 * Play module commands
 * ---------------------------------------------------------------------- */
#define PLAY_IDLE                   0x00
#define PLAY_STARTUP                0x01
#define PAUSE_ON                    0x02
#define PAUSE_OFF                   0x03
#define JUMP_TO_ADDRESS             0x04
#define PLAY_TRACK                  0x05
#define PLAY_READ_SUBCODE           0x06
#define PLAY_READ_TOC               0x07
#define PLAY_PREPARE_SPEED_CHANGE   0x08
#define PLAY_RESTORE_SPEED_CHANGE   0x09
#define PLAY_SET_VOLUME             0x0A
#define PLAY_JUMP_TRACKS            0x0B

/* -------------------------------------------------------------------------
 * Player module commands
 * ---------------------------------------------------------------------- */
#define PLAYER_IDLE           0x00
#define PLAYER_HANDLE_ERROR   0x01
#define SET_SERVICE_MODE      0x02

/* -------------------------------------------------------------------------
 * Process execution states
 * ---------------------------------------------------------------------- */
#define BUSY           0
#define READY          1
#define CD_ERROR_STATE 2    /**< Renamed from ERROR to avoid clash with system headers */
#define PROCESS_READY  3

/* -------------------------------------------------------------------------
 * Time comparison results
 * ---------------------------------------------------------------------- */
#define SMALLER  0
#define EQUAL    1
#define BIGGER   2

/* -------------------------------------------------------------------------
 * Subcode area identifiers
 * ---------------------------------------------------------------------- */
#define ALL_SUBCODES       0
#define ABS_TIME           1
#define CATALOG_NR         2
#define ISRC_NR            3
#define FIRST_LEADIN_AREA  4
#define LEADIN_AREA        5
#define PROGRAM_AREA       6
#define LEADOUT_AREA       7

/* -------------------------------------------------------------------------
 * Lid / door states
 * ---------------------------------------------------------------------- */
#define CLOSED      0
#define OPEN        1
#define LID_OPEN    0
#define LID_CLOSED  1

/* -------------------------------------------------------------------------
 * Boolean convenience (use stdbool where possible, keep legacy names)
 * ---------------------------------------------------------------------- */
#ifndef TRUE
#define TRUE   1
#endif
#ifndef FALSE
#define FALSE  0
#endif

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */
#define NO_ERROR               0x00
#define ILLEGAL_COMMAND        0x01
#define ILLEGAL_PARAMETER      0x02
#define SLEDGE_ERROR           0x03
#define FOCUS_ERROR            0x04
#define MOTOR_ERROR            0x05
#define RADIAL_ERROR           0x06
#define PLL_LOCK_ERROR         0x07
#define SUBCODE_TIMEOUT_ERROR  0x08
#define SUBCODE_NOT_FOUND      0x09
#define TRAY_ERROR             0x0A
#define TOC_READ_ERROR         0x0B
#define JUMP_ERROR             0x0C
#define HF_DETECTOR_ERROR      0x0D

/* -------------------------------------------------------------------------
 * CD time structure (BCD minutes, seconds, frames)
 * ---------------------------------------------------------------------- */
typedef struct {
    byte min;
    byte sec;
    byte frm;
} cd_time_t;

/* -------------------------------------------------------------------------
 * Q-channel subcode frame
 * ---------------------------------------------------------------------- */
typedef struct {
    byte      conad;
    byte      tno;
    byte      index;
    cd_time_t r_time;
    byte      zero;
    cd_time_t a_time;
} subcode_frame_t;

/* -------------------------------------------------------------------------
 * 16-bit big/little split helper
 * ---------------------------------------------------------------------- */
typedef struct { byte high; byte low; } byte_hl_t;
typedef union  { int val; byte_hl_t b; } int_hl_t;
