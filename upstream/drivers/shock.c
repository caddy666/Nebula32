/**
 * @file  shock.c
 * @brief Shock / scratch detector and recovery module.
 *
 * Monitors disc progress during playback by checking that absolute disc
 * time advances by the expected amount each timer tick.  If progress
 * stalls or jumps unexpectedly the module mutes the audio and seeks back
 * to the last good position.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 */

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "driver.h"
#include "serv_def.h"
#include "timer.h"

/* =========================================================================
 * External references
 * ====================================================================== */

extern volatile uint8_t timers[];
#define progress_timer timers[TIMER_PROGRESS]

extern int n1_speed;

/* From subcode.c */
extern uint8_t is_subcode(uint8_t mode);
extern void    move_abstime(cd_time_t *p);

/* From play.c (provided as stub below) */
extern uint8_t jump_time(cd_time_t *t);

/* From driver.c */
/* cd6_wr(MUTE) / cd6_wr(FULL_SCALE) used for mute/unmute */

/* From utils/maths.c */
extern void    add_time(const cd_time_t *a, const cd_time_t *b, cd_time_t *r);
extern uint8_t compare_time(const cd_time_t *a, const cd_time_t *b);

/* =========================================================================
 * Timing constants (number of 8 ms ticks)
 * ====================================================================== */
#define PROGRESS_N1   40u   /**< ~320 ms slice at single speed  */
#define PROGRESS_N2   20u   /**< ~160 ms slice at double speed  */

/* =========================================================================
 * Module state
 * ====================================================================== */

static uint8_t   shock_phase0          = 0;
uint8_t          shock_recovery_active = 0;

static cd_time_t last_absolute_time;

/* =========================================================================
 * Static time-allocation pool
 * (replaces the alloc/release_struct_time() heap from the 8051 firmware)
 * ====================================================================== */

static cd_time_t time_pool[2];

static void reset_time_pool(void)
{
    memset(time_pool, 0, sizeof(time_pool));
}

/* =========================================================================
 * Mute helpers
 * ====================================================================== */

static uint8_t mute_on(void)  { cd6_wr(MUTE);       return READY; }
static uint8_t mute_off(void) { cd6_wr(FULL_SCALE);  return READY; }

/* =========================================================================
 * Shock register initialisation (called at play start)
 * ====================================================================== */

void init_shock_registers(void)
{
    progress_timer = n1_speed ? PROGRESS_N1 : PROGRESS_N2;
    move_abstime(&last_absolute_time);
}

/* =========================================================================
 * Public: shock_detector_off / on — called from play module step functions
 * ====================================================================== */

uint8_t shock_detector_off(void)
{
    shock_recovery_active = 0;
    return READY;
}

uint8_t shock_detector_on(void)
{
    /* Initialise with the play target time — set by play.c before calling */
    extern cd_time_t play_target_time;
    progress_timer        = n1_speed ? PROGRESS_N1 : PROGRESS_N2;
    last_absolute_time    = play_target_time;
    shock_recovery_active = 1;
    shock_phase0          = 0;
    return READY;
}

/* =========================================================================
 * Public: shock_recover — called every main-loop tick (normal mode only)
 *
 * Progress window check:
 *
 *                        < delta (progress_timer)  >
 *   =======================================================
 *   ---------|--------|---------|---------|-----------|--->
 *           O        B          N          U            time
 *
 *   O = last_absolute_time
 *   B = O + 00:00:15  (minimum expected progress)
 *   N = current Q_buffer absolute time
 *   U = O + 00:01:00  (maximum expected progress)
 *
 *   Pass condition: B < N < U
 *   Fail → mute and seek to O + 00:00:40
 * ====================================================================== */

void shock_recover(void)
{
    if (!shock_recovery_active) return;

    reset_time_pool();
    cd_time_t *position = &time_pool[0];
    cd_time_t *window   = &time_pool[1];

    if (!shock_phase0) {
        /* Phase 0: wait for the progress timer to expire */
        if (progress_timer != 0) return;

        if (is_subcode(ABS_TIME)) {
            move_abstime(position);

            /* Lower bound: last + 15 frames */
            window->min = 0; window->sec = 0; window->frm = 0x0F;
            add_time(&last_absolute_time, window, window);

            if (compare_time(position, window) == BIGGER) {
                /* Progress detected — check upper bound */
                window->min = 0; window->sec = 1; window->frm = 0;
                add_time(&last_absolute_time, window, window);

                if (compare_time(window, position) == SMALLER) {
                    /* Too far ahead (>1 s jump) — seek back */
                    shock_phase0 = 1;
                    mute_on();
                } else {
                    /* Normal progress — update reference time */
                    init_shock_registers();
                }
            } else {
                /* Progress too slow — seek back */
                shock_phase0 = 1;
                mute_on();
            }
        } else if (is_subcode(FIRST_LEADIN_AREA)) {
            shock_phase0 = 1;
            mute_on();
        }
    }

    if (shock_phase0) {
        /* Phase 1: seek to last_absolute_time + 40 frames */
        window->min = 0; window->sec = 0; window->frm = 0x28;
        add_time(&last_absolute_time, window, window);

        uint8_t status = jump_time(window);
        if (status == READY) {
            shock_phase0 = 0;
            init_shock_registers();
            mute_off();
        } else if (status == CD_ERROR_STATE) {
            /* Target unreachable — advance the reference point and try again */
            last_absolute_time = *window;
        }
    }
}
