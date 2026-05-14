/**
 * @file  strtstop.c
 * @brief Spindle motor start-up / stop sequencer and disc-type detection.
 *
 * Manages the SS_IDLE / SS_START_UP / SS_STOP / SS_SPEED_N1 / SS_SPEED_N2 /
 * SS_MOTOR_OFF command states by co-ordinating the servo module and subcode
 * module.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 */

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "serv_def.h"
#include "dsic2.h"
#include "driver.h"
#include "timer.h"

/* =========================================================================
 * External references
 * ====================================================================== */

extern volatile uint8_t timers[];
#define play_timer  timers[TIMER_PLAY]

extern uint8_t player_error;

/* Servo module API */
extern void    servo_start(void);
extern void    servo_stop(void);
extern void    servo_n1(void);
extern void    servo_n2(void);
extern void    servo_jump(int jump_size);
extern uint8_t get_servo_process_state(void);

/* Subcode module API */
extern void    start_subcode_reading(void);
extern void    stop_subcode_reading(void);
extern uint8_t is_subcode(uint8_t mode);
extern uint8_t is_cd_disc(void);

/* jump_time is provided by play.c */
extern uint8_t jump_time(cd_time_t *t);

/* disc_size_known is shared with servo.c */
extern uint8_t disc_size_known;

/* =========================================================================
 * Module-private shared store
 * (matches union layout in strtstop.c and shock.c in the original firmware)
 * ====================================================================== */
typedef union {
    struct {
        uint8_t   status1;
        uint8_t   status2;
        uint8_t   counter;
        uint8_t   last_read_tno;
        uint8_t   first_mode5_pointer;
        cd_time_t tmp_time;
        cd_time_t start_next_leadin_area;
    } toc_info;
    struct {
        int       tracks;
        cd_time_t time1;
        cd_time_t time2;
        cd_time_t time3;  /**< time3.min used as retry counter */
    } scan_info;
    struct {
        uint8_t        samples;
        uint16_t       offtrack_1;
        uint16_t       offtrack_2;
        cd_time_t      tmp_time;
        uint8_t        counter;
        uint16_t       ref_value;
    } sledge_cal_info;
} start_stop_store_t;

/* This union is also used by shock.c via extern — exported as 'store' */
start_stop_store_t store;

/* =========================================================================
 * Module state
 *
 * start_stop_process:
 *   high nibble = current command (SS_IDLE … SS_MOTOR_OFF)
 *   low  nibble = sub-phase within that command
 *
 * start_stop_status:
 *   high nibble = BUSY / READY / CD_ERROR_STATE
 *   low  nibble = sub-phase
 * ====================================================================== */

static uint8_t start_stop_process  = 0;
static uint8_t start_stop_status   = 0;
uint8_t        start_stop_command_busy = 0;
static uint8_t disc_type_known     = 0;
static uint8_t cd_disc_local       = 1;  /**< 1=CD, 0=CDR */

/* =========================================================================
 * Public helpers
 * ====================================================================== */

void init_for_new_disc(void)
{
    disc_size_known   = 0;
    disc_type_known   = 0;
    cd_disc_local     = 1;
}

/* =========================================================================
 * Private: disc type detection
 * ====================================================================== */

static uint8_t get_disk_type(void)
{
    if (disc_type_known) return READY;

    switch (start_stop_process & 0x0Fu) {

    case 0:
        start_subcode_reading();
        play_timer = SUBCODE_TIMEOUT_VALUE;
        start_stop_process++;
        /* fall through */

    case 1:
        if (is_subcode(FIRST_LEADIN_AREA)) {
            start_stop_process = (uint8_t)((start_stop_process & 0xF0u) | 0x03u);
        } else if (is_subcode(ABS_TIME)) {
            start_stop_process++;
        } else if (play_timer == 0) {
            player_error = SUBCODE_TIMEOUT_ERROR;
            return CD_ERROR_STATE;
        }
        break;

    case 2: {
        /* Return to the lead-in */
        store.toc_info.tmp_time.min = 0;
        store.toc_info.tmp_time.sec = 2;
        store.toc_info.tmp_time.frm = 0;
        uint8_t es = jump_time(&store.toc_info.tmp_time);
        if (es == READY) {
            start_stop_process++;
            start_subcode_reading();
            play_timer = SUBCODE_TIMEOUT_VALUE;
        } else {
            return es;
        }
        break;
    }

    case 3:
        store.toc_info.counter = 5;
        play_timer = SUBCODE_TIMEOUT_VALUE;
        start_stop_process++;
        /* fall through */

    case 4:
        if (is_subcode(FIRST_LEADIN_AREA)) {
            cd_disc_local   = (Q_buffer[1] > 90u) ? 0u : 1u;
            disc_type_known = 1;
            return READY;
        } else if (is_subcode(PROGRAM_AREA)) {
            servo_jump(TRACKS_INTO_LEADIN);
            start_stop_process++;
        } else if (play_timer == 0) {
            player_error = SUBCODE_TIMEOUT_ERROR;
            return CD_ERROR_STATE;
        }
        break;

    case 5: {
        uint8_t es = get_servo_process_state();
        if (es == READY) {
            store.toc_info.counter--;
            if (store.toc_info.counter == 0) {
                player_error = TOC_READ_ERROR;
                return CD_ERROR_STATE;
            }
            start_subcode_reading();
            play_timer = SUBCODE_TIMEOUT_VALUE;
            start_stop_process--;
        } else {
            return es;
        }
        break;
    }

    default:
        break;
    }

    return BUSY;
}

/* =========================================================================
 * Public: start_stop — called from the player process dispatch table
 * ====================================================================== */

uint8_t start_stop(uint8_t cmd)
{
    if ((start_stop_process >> 4) != cmd || !start_stop_command_busy) {
        start_stop_process     = (uint8_t)(cmd << 4);
        start_stop_status      = (uint8_t)(BUSY << 4);
        start_stop_command_busy = 1;
    }
    if ((start_stop_status >> 4) != BUSY)
        start_stop_command_busy = 0;
    return (uint8_t)(start_stop_status >> 4);
}

/* =========================================================================
 * Public: execute_start_stop_functions — called every main-loop tick
 * ====================================================================== */

void execute_start_stop_functions(void)
{
    uint8_t cmd = start_stop_process >> 4;

    if (cmd == SS_IDLE) {
        if (!(start_stop_status & 0x0Fu))
            start_stop_status = (uint8_t)((READY << 4) | 0x01u);
        return;
    }

    if (cmd == SS_START_UP) {
        switch (start_stop_status & 0x0Fu) {
        case 0:
            servo_start();
            start_stop_status++;
            /* fall through */

        case 1: {
            uint8_t es = get_servo_process_state();
            if (es == READY) {
                start_subcode_reading();
                start_stop_process &= 0xF0u;
                start_stop_status++;
            } else {
                if (es == CD_ERROR_STATE)
                    start_stop_status = (uint8_t)((CD_ERROR_STATE << 4) | 0x04u);
                break;
            }
        }
        /* fall through */

        case 2: {
            uint8_t es = get_disk_type();
            if (es == READY)
                start_stop_status = (uint8_t)((READY << 4) | 0x03u);
            else {
                if (es == CD_ERROR_STATE)
                    start_stop_status = (uint8_t)((CD_ERROR_STATE << 4) | 0x03u);
                break;
            }
        }
        /* fall through */

        case 3:
        default:
            break;
        }
        return;
    }

    /* SS_STOP / SS_SPEED_N1 / SS_SPEED_N2 / SS_MOTOR_OFF */
    switch (start_stop_status & 0x0Fu) {
    case 0:
        if      (cmd == SS_SPEED_N1) servo_n1();
        else if (cmd == SS_SPEED_N2) servo_n2();
        else                         servo_stop();

        if (cmd == SS_MOTOR_OFF)
            start_stop_status = (uint8_t)((READY << 4) | 0x02u);
        else
            start_stop_status++;
        break;

    case 1: {
        uint8_t es = get_servo_process_state();
        if (es == READY)
            start_stop_status = (uint8_t)((READY << 4) | 0x02u);
        else {
            if (es == CD_ERROR_STATE)
                start_stop_status = (uint8_t)((CD_ERROR_STATE << 4) | 0x02u);
            break;
        }
    }
    /* fall through */

    case 2:
    default:
        break;
    }
}
