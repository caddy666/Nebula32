/**
 * @file  play.c
 * @brief Play module — disc-address seeking, pause, TOC reading, subcode
 *        retrieval, speed change, and shock recovery interface.
 *
 * The play module owns the play_process / play_status state and provides:
 *   play()                 — command entry point (called from player dispatch)
 *   execute_play_functions() — background tick (called every main-loop iter)
 *   jump_time()            — shared seek-to-address helper (used by shock.c,
 *                            strtstop.c, and play itself)
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 * All 8051-specific constructs removed.
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
#define play_timer timers[TIMER_PLAY]

extern uint8_t  player_error;
extern interface_field_t player_interface;

/* From subcode.c */
extern uint8_t  is_subcode(uint8_t mode);
extern void     start_subcode_reading(void);
extern void     stop_subcode_reading(void);
extern void     move_abstime(cd_time_t *p);

/* From servo.c */
extern void     servo_jump(int jump_size);
extern uint8_t  get_servo_process_state(void);
extern uint8_t  get_jump_status(void);
extern uint8_t  servo_tracking(void);

/* From shock.c */
extern uint8_t  shock_detector_off(void);
extern uint8_t  shock_detector_on(void);
extern cd_time_t play_target_time;   /* set here, read by shock_detector_on */

/* From utils/maths.c */
extern int      calc_tracks(const cd_time_t *a, const cd_time_t *b);
extern uint8_t  compare_time(const cd_time_t *a, const cd_time_t *b);
extern void     add_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r);
extern void     subtract_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r);

/* From driver.c / timer.c */
extern int      zero_scor_counter(void);
extern void     init_scor_counter(uint8_t count);

/* =========================================================================
 * BCD helpers (hex_to_bcd lives in maths.c; hex_to_bcd_time is local)
 * ====================================================================== */

static uint8_t hex_to_bcd(uint8_t hex)
{
    uint8_t tens = hex / 10u;
    return (uint8_t)((tens << 4) | (hex - tens * 10u));
}

static void hex_to_bcd_time(const cd_time_t *src, cd_time_t *dst)
{
    dst->min = hex_to_bcd(src->min);
    dst->sec = hex_to_bcd(src->sec);
    dst->frm = hex_to_bcd(src->frm);
}

static void bcd_to_hex_time_local(const cd_time_t *src, cd_time_t *dst)
{
    dst->min = (uint8_t)(((src->min  >> 4) * 10u) + (src->min  & 0x0Fu));
    dst->sec = (uint8_t)(((src->sec  >> 4) * 10u) + (src->sec  & 0x0Fu));
    dst->frm = (uint8_t)(((src->frm  >> 4) * 10u) + (src->frm  & 0x0Fu));
}

/* =========================================================================
 * Play mode constants (stored in low nibble of play_status)
 * ====================================================================== */

#define IDLE_MODE             0x00u
#define PAUSE_MODE            0x01u
#define TRACKING_MODE         0x02u
#define TOC_MODE              0x03u
#define RETURN_TO_TARGET      0x08u   /**< Bit set in play_status during speed change */

/* =========================================================================
 * Shared store — union shared with strtstop.c and shock.c
 *
 * The play_times overlay holds seek addresses; play_subcode holds the
 * most recently captured Q-channel frame (in BCD for transmission).
 * ====================================================================== */

typedef union {
    struct {
        /* subframe fields match subcode_frame_t layout */
        uint8_t   conad;
        uint8_t   tno;
        uint8_t   index;
        cd_time_t r_time;
        uint8_t   zero;
        cd_time_t a_time;
        uint8_t   left;
        uint8_t   right;
    } play_subcode;
    struct {
        cd_time_t low_time;
        cd_time_t high_time;
        cd_time_t target_time;
        cd_time_t tmp_time;
    } play_times;
} play_store_t;

/* Exported to shock.c as 'store' — must match the layout declared there */
play_store_t store;

/* ARM replacement for the 8051 param1-as-pointer trick: callers that need the
 * subcode buffer address read this instead of reconstructing it from param1. */
const void *play_subcode_result = NULL;

/* shock.c reads this when shock_detector_on() is called */
cd_time_t play_target_time;

/* =========================================================================
 * Module state
 *
 * play_process:
 *   high nibble = play command index (PLAY_IDLE … PLAY_JUMP_TRACKS)
 *   low  nibble = step index within that command
 *
 * play_status:
 *   high nibble = BUSY / READY / CD_ERROR_STATE
 *   low  nibble = play mode (IDLE_MODE … TOC_MODE) + RETURN_TO_TARGET bit
 * ====================================================================== */

static uint8_t play_process = 0;
static uint8_t play_status  = 0;

static uint8_t volume_level = 0x80u;  /**< Shadow of CXD2500 volume register */
static uint8_t jumps_counter = 0;

static uint8_t play_phase0 = 0;
static uint8_t play_phase1 = 0;
static uint8_t jump_phase0 = 0;
static uint8_t jump_phase1 = 0;

static uint8_t reload_play_timer0 = 0;
static uint8_t reload_play_timer1 = 0;

static uint8_t track_found          = 0;
static uint8_t mute_on_after_search = 0;

uint8_t play_monitor      = 0;   /**< Set once a command is done (ok or error) */
uint8_t play_command_busy = 0;   /**< Set while a play command is executing     */

/* =========================================================================
 * Public helpers called from cmd_hndl.c / service.c
 * ====================================================================== */

uint8_t drive_is_pausing(void)
{
    return (play_status & 0x0Fu) == PAUSE_MODE ? 1u : 0u;
}

void set_dac_mode(uint8_t dac_mode)
{
    if      (dac_mode == 0xF0u) mute_on_after_search = 1;
    else if (dac_mode == 0xF1u) mute_on_after_search = 0;
    else                        cd6_wr(dac_mode);
}

void set_play_mode(uint8_t mode) { cd6_wr(mode); }
void attenuate_on(void)          { cd6_wr(ATTENUATE); }

/* =========================================================================
 * Mute helpers
 * ====================================================================== */

uint8_t mute_on(void)
{
    cd6_wr(MUTE);
    return READY;
}

uint8_t mute_off(void)
{
    if (!mute_on_after_search) cd6_wr(FULL_SCALE);
    return READY;
}

/* =========================================================================
 * jump_time — seek to an absolute disc time
 *
 * Uses a binary-search-style approach:
 *   Phase 0 (entry): read subcode to find current position
 *   Phase 1: subcode received — calculate groove delta and jump
 *   Phase 2: wait for servo to complete the jump
 *   Phase 3: wait for SCOR frame counter to reach target frame
 * ====================================================================== */

uint8_t jump_time(cd_time_t *pt)
{
    /* Phase 0 — initialise */
    if (!jump_phase0 && !jump_phase1) {
        start_subcode_reading();
        play_timer    = SUBCODE_TIMEOUT_VALUE;
        jumps_counter = 0;
        jump_phase0   = 1;
    }

    /* Phase 1 — subcode received: calculate and execute jump */
    if (jump_phase0 && !jump_phase1) {
        if (is_subcode(ABS_TIME)) {
            move_abstime(&store.play_times.tmp_time);
            int nr = calc_tracks(&store.play_times.tmp_time, pt);

            if (nr == 0) {
                /* On target — count remaining frames via SCOR.
                 * Guard against overshoot: if current > pt, subtract_time()
                 * would underflow and wrap delta.frm to 75-N instead of 0. */
                uint8_t frm_count = 0;
                if (compare_time(&store.play_times.tmp_time, pt) == SMALLER) {
                    cd_time_t delta;
                    subtract_time(pt, &store.play_times.tmp_time, &delta);
                    frm_count = delta.frm;
                }
                init_scor_counter(frm_count);
                jump_phase1 = 1;    /* → phase 3 */
            } else {
                servo_jump(nr);
                jump_phase1 = 1;
                jump_phase0 = 0;    /* → phase 2 */
            }
        } else if (is_subcode(FIRST_LEADIN_AREA)) {
            servo_jump(TRACKS_OUTOF_LEADIN);
            jump_phase1 = 1;
            jump_phase0 = 0;        /* → phase 2 */
        } else if (play_timer == 0) {
            player_error = SUBCODE_TIMEOUT_ERROR;
            jump_phase0  = 0;
            return CD_ERROR_STATE;
        }
    }

    /* Phase 2 — wait for servo jump to complete */
    if (!jump_phase0 && jump_phase1) {
        uint8_t es = get_servo_process_state();
        if (es == READY) {
            if (get_jump_status() & NO_HF_ON_TARGET) {
                player_error = JUMP_ERROR;
                jump_phase1  = 0;
                return CD_ERROR_STATE;
            }
            jumps_counter++;
            if (jumps_counter > 20u) {
                player_error = JUMP_ERROR;
                jump_phase1  = 0;
                return CD_ERROR_STATE;
            }
            jump_phase1 = 0;   /* back to phase 0 / re-read subcode */
        } else if (es == CD_ERROR_STATE) {
            jump_phase1 = 0;
            return CD_ERROR_STATE;
        }
    }

    /* Phase 3 — on target: wait for the SCOR frame counter */
    if (jump_phase0 && jump_phase1) {
        if (zero_scor_counter()) {
            jump_phase0 = 0;
            jump_phase1 = 1;
            return READY;
        }
    }

    return BUSY;
}

/* =========================================================================
 * play — command entry point (called by the player dispatch table)
 * ====================================================================== */

uint8_t play(uint8_t play_cmd)
{
    if ((play_process >> 4) != play_cmd || !play_command_busy) {
        play_process = (uint8_t)(play_cmd << 4);
        play_phase0  = 0;
        play_phase1  = 0;
        play_command_busy = 1;
        play_monitor      = 0;

        if (play_cmd == PLAY_RESTORE_SPEED_CHANGE)
            play_status &= 0x0Fu;
        else
            play_status &= 0x07u;   /* clear RETURN_TO_TARGET too */

        /* Pre-flight validation */
        switch (play_cmd) {
        case PAUSE_ON:
            if (!servo_tracking() || (play_status & 0x0Fu) == TOC_MODE) {
                if (player_error == NO_ERROR) player_error = ILLEGAL_COMMAND;
                play_status  |= (uint8_t)(CD_ERROR_STATE << 4);
                play_monitor  = 1;
            } else if ((play_status & 0x0Fu) == PAUSE_MODE) {
                play_status  |= (uint8_t)(READY << 4);
                play_monitor  = 1;
            }
            break;

        case PAUSE_OFF:
            if (!servo_tracking() || (play_status & 0x0Fu) != PAUSE_MODE) {
                play_monitor = 1;
                if (servo_tracking() || (play_status & 0x0Fu) == TRACKING_MODE)
                    play_status |= (uint8_t)(READY << 4);
                else {
                    if (player_error == NO_ERROR) player_error = ILLEGAL_COMMAND;
                    play_status |= (uint8_t)(CD_ERROR_STATE << 4);
                }
            }
            break;

        case PLAY_READ_SUBCODE:
            if (!servo_tracking()) {
                if (player_error == NO_ERROR) player_error = ILLEGAL_COMMAND;
                play_status  |= (uint8_t)(CD_ERROR_STATE << 4);
                play_monitor  = 1;
            }
            break;

        case PLAY_SET_VOLUME:
        case PLAY_PREPARE_SPEED_CHANGE:
            if (player_error != NO_ERROR) {
                play_status  |= (uint8_t)(CD_ERROR_STATE << 4);
                play_monitor  = 1;
            }
            break;

        case PLAY_JUMP_TRACKS:
            if (!servo_tracking() ||
                (play_status & 0x0Fu) == TOC_MODE ||
                (play_status & 0x0Fu) == PAUSE_MODE)
            {
                if (player_error == NO_ERROR) player_error = ILLEGAL_COMMAND;
                play_status  |= (uint8_t)(CD_ERROR_STATE << 4);
                play_monitor  = 1;
            }
            break;

        default:
            break;
        }

        if (!play_monitor)
            play_status |= (uint8_t)(BUSY << 4);
    }

    if ((play_status >> 4) != BUSY)
        play_command_busy = 0;

    return (uint8_t)(play_status >> 4);
}

/* =========================================================================
 * Step functions — called from execute_play_functions()
 * ====================================================================== */

/* Helper: copy Q_buffer → store.play_subcode, converting hex → BCD */
static void set_subcode_buffer(void)
{
    store.play_subcode.conad         = Q_buffer[0];
    store.play_subcode.tno           = Q_buffer[1];
    store.play_subcode.index         = Q_buffer[2];
    store.play_subcode.r_time.min    = Q_buffer[3];
    store.play_subcode.r_time.sec    = Q_buffer[4];
    store.play_subcode.r_time.frm    = Q_buffer[5];
    store.play_subcode.zero          = Q_buffer[6];
    store.play_subcode.a_time.min    = Q_buffer[7];
    store.play_subcode.a_time.sec    = Q_buffer[8];
    store.play_subcode.a_time.frm    = Q_buffer[9];

    uint8_t conad_lo = store.play_subcode.conad & 0x0Fu;

    if (conad_lo == 0x01u) {
        if (store.play_subcode.tno != 0xAAu)
            store.play_subcode.tno = hex_to_bcd(store.play_subcode.tno);
        hex_to_bcd_time(&store.play_subcode.r_time, &store.play_subcode.r_time);
        if (store.play_subcode.index == 0xA0u ||
            store.play_subcode.index == 0xA1u) {
            store.play_subcode.a_time.min =
                hex_to_bcd(store.play_subcode.a_time.min);
        } else {
            hex_to_bcd_time(&store.play_subcode.a_time,
                            &store.play_subcode.a_time);
            if (store.play_subcode.index != 0xA2u)
                store.play_subcode.index = hex_to_bcd(store.play_subcode.index);
        }
    } else if (conad_lo == 0x05u) {
        if (store.play_subcode.tno   == 0x00u &&
            store.play_subcode.index == 0xB0u &&
            store.play_subcode.r_time.min != 0xFFu)
        {
            hex_to_bcd_time(&store.play_subcode.r_time,
                            &store.play_subcode.r_time);
        }
    }
}

/* True if the subcode currently in store matches what param1/2 requested */
static uint8_t req_subc_stored_subc(void)
{
    if (player_interface.param1 == 0) return TRUE;
    if ((store.play_subcode.conad & 0x0Fu) == player_interface.param1) {
        if (store.play_subcode.index == player_interface.param2 ||
            player_interface.param2 == 0xFFu)
            return TRUE;
    }
    return FALSE;
}

/* --- PLAY_IDLE -------------------------------------------------------- */
static uint8_t playing_idle(void)
{
    mute_on();
    shock_detector_off();
    stop_subcode_reading();
    jumps_counter = 0;
    play_status   = (uint8_t)((play_status & 0xF0u) | IDLE_MODE);
    return PROCESS_READY;
}

/* --- PLAY_STARTUP ----------------------------------------------------- */
static uint8_t play_process_ready(void) { return PROCESS_READY; }

/* --- PAUSE_ON helpers ------------------------------------------------- */
static uint8_t pause_on_init(void)
{
    if (!play_phase0) {
        start_subcode_reading();
        play_timer  = SUBCODE_TIMEOUT_VALUE;
        play_phase0 = 1;
    }
    if (is_subcode(ABS_TIME)) {
        set_subcode_buffer();
        return READY;
    }
    if (play_timer == 0) { player_error = SUBCODE_TIMEOUT_ERROR; return CD_ERROR_STATE; }
    return BUSY;
}

static uint8_t set_pause_mode(void)
{
    play_status = (uint8_t)((play_status & 0xF0u) | PAUSE_MODE);
    return PROCESS_READY;
}

/* --- PAUSE_OFF helpers ------------------------------------------------ */
static uint8_t restore_target_address(void)
{
    if (!play_phase0) {
        start_subcode_reading();
        play_timer  = SUBCODE_TIMEOUT_VALUE;
        play_phase0 = 1;
    }
    if (is_subcode(ABS_TIME)) {
        move_abstime(&store.play_times.target_time);
        return READY;
    }
    if (play_timer == 0) { player_error = SUBCODE_TIMEOUT_ERROR; return CD_ERROR_STATE; }
    return BUSY;
}

static uint8_t set_tracking_mode(void)
{
    play_status = (uint8_t)((play_status & 0xF0u) | TRACKING_MODE);
    /* Update shock module's target reference */
    play_target_time = store.play_times.target_time;
    return PROCESS_READY;
}

/* --- JUMP_TO_ADDRESS helpers ------------------------------------------ */
static uint8_t copy_parameters(void)
{
    store.play_times.target_time.min = player_interface.param1;
    store.play_times.target_time.sec = player_interface.param2;
    store.play_times.target_time.frm = player_interface.param3;
    play_target_time = store.play_times.target_time;
    return READY;
}

static uint8_t search_for_address(void)
{
    return jump_time(&store.play_times.target_time);
}

/* --- PLAY_READ_SUBCODE ------------------------------------------------ */
static uint8_t monitor_subcodes(void)
{
    /* Phase 0 — initialise or handle pause case */
    if (!play_phase1 && !play_phase0) {
        if ((play_status & 0x0Fu) == PAUSE_MODE) {
            if (req_subc_stored_subc()) {
                play_subcode_result     = &store.play_subcode;
                player_interface.param1 = 0x01u;   /* non-zero sentinel; pointer in play_subcode_result */
                return PROCESS_READY;
            }
            player_error = ILLEGAL_PARAMETER;
            return CD_ERROR_STATE;
        }
        start_subcode_reading();
        play_timer          = 255u;
        reload_play_timer0  = 1;
        reload_play_timer1  = 1;
        track_found         = 0;
        play_phase0         = 1;
    }

    /* Phase 1 — wait for the requested subcode */
    if (!play_phase1 && play_phase0) {
        if (play_timer == 0) {
            if (!track_found) {
                player_error = SUBCODE_TIMEOUT_ERROR;
                return CD_ERROR_STATE;
            }
            track_found = 0;
            play_timer  = 255u;
            if (reload_play_timer1) {
                if (reload_play_timer0) reload_play_timer0 = 0;
                else { reload_play_timer1 = 0; reload_play_timer0 = 1; }
            } else {
                if (reload_play_timer0) reload_play_timer0 = 0;
                else { player_error = SUBCODE_NOT_FOUND; return CD_ERROR_STATE; }
            }
        }

        if (is_subcode(ALL_SUBCODES)) {
            track_found = 1;
            if ((play_status & 0x0Fu) == TOC_MODE && is_subcode(PROGRAM_AREA)) {
                servo_jump(TRACKS_INTO_LEADIN);
                play_phase1 = 1;
                play_phase0 = 0;    /* → phase 2 */
            } else {
                set_subcode_buffer();
                if (req_subc_stored_subc()) {
                    play_subcode_result     = &store.play_subcode;
                    player_interface.param1 = 0x01u;   /* sentinel; pointer in play_subcode_result */
                    return PROCESS_READY;
                }
                start_subcode_reading();
            }
        }
    }

    /* Phase 2 — wait for jump back into lead-in */
    if (play_phase1 && !play_phase0) {
        uint8_t es = get_servo_process_state();
        if (es == READY) {
            start_subcode_reading();
            play_phase1 = 0;
            play_phase0 = 1;
        } else {
            return es;
        }
    }

    return BUSY;
}

/* --- PLAY_READ_TOC ---------------------------------------------------- */
static uint8_t set_toc_mode(void)
{
    if (player_interface.param2 == 0)
        play_status = (uint8_t)((play_status & 0xF0u) | TOC_MODE);
    else
        play_status = (uint8_t)((play_status & 0xF0u) | IDLE_MODE);
    return PROCESS_READY;
}

static uint8_t jump_to_toc(void)
{
    if (!play_phase0) {
        store.play_times.target_time.min = 0;
        store.play_times.target_time.sec = 2;
        store.play_times.target_time.frm = 0;
        uint8_t es = jump_time(&store.play_times.target_time);
        if (es == READY) {
            servo_jump(TRACKS_INTO_LEADIN);
            play_phase0 = 1;
        } else {
            return es;
        }
    } else {
        return get_servo_process_state();
    }
    return BUSY;
}

/* --- PLAY_PREPARE_SPEED_CHANGE --------------------------------------- */
static uint8_t prepare_before_speed_change(void)
{
    if (!play_phase1 && !play_phase0) {
        if ((play_status & 0x0Fu) != TRACKING_MODE) return READY;
        mute_on();
        play_phase0 = 1;
    }

    if (!play_phase1 && play_phase0) {
        uint8_t es = shock_detector_off();
        if (es == READY) {
            start_subcode_reading();
            play_timer  = SUBCODE_TIMEOUT_VALUE;
            play_phase1 = 1;
            play_phase0 = 0;
        } else return es;
    }

    if (play_phase1 && !play_phase0) {
        if (is_subcode(LEADIN_AREA)) return READY;
        if (is_subcode(ABS_TIME)) {
            move_abstime(&store.play_times.target_time);
            play_status |= RETURN_TO_TARGET;
            return READY;
        }
        if (play_timer == 0) {
            player_error = SUBCODE_TIMEOUT_ERROR;
            return CD_ERROR_STATE;
        }
    }
    return BUSY;
}

/* --- PLAY_RESTORE_SPEED_CHANGE --------------------------------------- */
static uint8_t restore_after_speed_change(void)
{
    if (!play_phase1 && !play_phase0) {
        if ((play_status & 0x07u) == TRACKING_MODE) {
            if (play_status & RETURN_TO_TARGET) {
                play_status &= (uint8_t)~RETURN_TO_TARGET;
                play_phase0  = 1;
            } else {
                mute_off();
                play_phase1 = 1;
            }
        } else return READY;
    }

    if (!play_phase1 && play_phase0) {
        uint8_t es = jump_time(&store.play_times.target_time);
        if (es == READY) {
            mute_off();
            play_phase1 = 1;
            play_phase0 = 0;
        } else return es;
    }

    if (play_phase1 && !play_phase0) {
        return shock_detector_on();
    }
    return BUSY;
}

/* --- PLAY_JUMP_TRACKS ------------------------------------------------ */
static uint8_t jump_tracks(void)
{
    int_hl_t nr;
    nr.b.high = player_interface.param1;
    nr.b.low  = player_interface.param2;

    if (!play_phase0) {
        servo_jump(nr.val);
        play_phase0 = 1;
        return BUSY;
    }
    return get_servo_process_state();
}

/* =========================================================================
 * Play process dispatch table
 * ====================================================================== */

#define MAX_PLAY_STEPS  5

typedef uint8_t (*play_fn_t)(void);

static const play_fn_t play_processes[][MAX_PLAY_STEPS] = {
/*  0 PLAY_IDLE                */  { playing_idle,                NULL,                NULL,               NULL,                 NULL              },
/*  1 PLAY_STARTUP             */  { play_process_ready,          NULL,                NULL,               NULL,                 NULL              },
/*  2 PAUSE_ON                 */  { mute_on,                     shock_detector_off,  pause_on_init,      set_pause_mode,       NULL              },
/*  3 PAUSE_OFF                */  { restore_target_address,      shock_detector_on,   set_tracking_mode,  NULL,                 NULL              },
/*  4 JUMP_TO_ADDRESS          */  { copy_parameters,             search_for_address,  shock_detector_on,  set_tracking_mode,    NULL              },
/*  5 PLAY_TRACK               */  { play_process_ready,          NULL,                NULL,               NULL,                 NULL              },
/*  6 PLAY_READ_SUBCODE        */  { monitor_subcodes,            NULL,                NULL,               NULL,                 NULL              },
/*  7 PLAY_READ_TOC            */  { jump_to_toc,                 set_toc_mode,        play_process_ready, NULL,                 NULL              },
/*  8 PLAY_PREPARE_SPEED_CHANGE*/  { prepare_before_speed_change, play_process_ready,  NULL,               NULL,                 NULL              },
/*  9 PLAY_RESTORE_SPEED_CHANGE*/  { restore_after_speed_change,  play_process_ready,  NULL,               NULL,                 NULL              },
/* 10 PLAY_SET_VOLUME          */  { play_process_ready,          NULL,                NULL,               NULL,                 NULL              },
/* 11 PLAY_JUMP_TRACKS         */  { shock_detector_off,          jump_tracks,         restore_target_address, shock_detector_on, play_process_ready },
};

/* =========================================================================
 * Background monitors (called when play_monitor == 1)
 * ====================================================================== */

static void monitor_pausing(void)
{
    if (get_servo_process_state() != READY) return;

    if (is_subcode(ABS_TIME)) {
        cd_time_t current, pause_hex;

        /* current = Q_buffer absolute time */
        current.min = Q_buffer[7];
        current.sec = Q_buffer[8];
        current.frm = Q_buffer[9];

        /* pause address is stored in BCD — convert to hex for compare */
        if (store.play_subcode.tno == 0)
            bcd_to_hex_time_local(&store.play_subcode.r_time, &pause_hex);
        else
            bcd_to_hex_time_local(&store.play_subcode.a_time, &pause_hex);

        if (compare_time(&current, &pause_hex) == BIGGER) {
            servo_jump(calc_tracks(&current, &pause_hex));
        } else {
            cd_time_t diff;
            subtract_time(&pause_hex, &current, &diff);
            if (diff.min != 0 || diff.sec != 0) {
                cd_time_t target;
                add_time(&diff, &current, &target);
                servo_jump(calc_tracks(&current, &target));
            }
        }
        start_subcode_reading();

    } else if (is_subcode(FIRST_LEADIN_AREA)) {
        servo_jump(100);   /* jumped into lead-in due to shock — recover outward */
    }
}

static void monitor_toc_reading(void)
{
    if (get_servo_process_state() == READY) {
        if (is_subcode(PROGRAM_AREA))
            servo_jump(TRACKS_INTO_LEADIN);
    }
}

/* =========================================================================
 * execute_play_functions — called once per main-loop tick
 * ====================================================================== */

void execute_play_functions(void)
{
    if (!play_monitor) {
        uint8_t cmd  = play_process >> 4;
        uint8_t step = play_process & 0x0Fu;

        if (cmd  >= 12u) goto monitor;
        if (step >= MAX_PLAY_STEPS) goto monitor;

        play_fn_t fn = play_processes[cmd][step];
        if (fn == NULL) goto monitor;

        switch (fn()) {
        case PROCESS_READY:
            play_status  = (uint8_t)((play_status & 0x0Fu) | (uint8_t)(READY << 4));
            play_monitor = 1;
            break;

        case READY:
            play_process++;   /* advance to next step */
            play_phase0 = 0;
            play_phase1 = 0;
            break;

        case CD_ERROR_STATE:
            play_status  = (uint8_t)((play_status & 0x0Fu) | (uint8_t)(CD_ERROR_STATE << 4));
            play_monitor = 1;
            break;

        case BUSY:
        default:
            break;
        }
    }

monitor:
    if (play_monitor) {
        uint8_t mode = play_status & 0x0Fu;
        uint8_t stat = play_status >> 4;

        if (stat == READY) {
            if      (mode == PAUSE_MODE)    monitor_pausing();
            else if (mode == TOC_MODE)      monitor_toc_reading();
            /* TRACKING_MODE and IDLE_MODE: nothing to do */
        }
    }
}
