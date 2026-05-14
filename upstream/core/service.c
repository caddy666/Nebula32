/**
 * @file  service.c
 * @brief Service-mode command executor.
 *
 * Executes diagnostic commands (laser on/off, focus on/off, motor on/off,
 * radial on/off, sledge move, groove jump, raw DSIC2/CD6 access) that are
 * only available after ENTER_SERVICE_MODE_OPC.
 *
 * Each service command is a short sequence of step-functions stored in
 * service_functions[][].  execute_service_functions() drives the sequence
 * one step per main-loop tick.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 */

#include <stdint.h>

#include "defs.h"
#include "serv_def.h"
#include "dsic2.h"
#include "driver.h"
#include "timer.h"
#include <stddef.h>

/* =========================================================================
 * External references
 * ====================================================================== */

extern volatile uint8_t timers[];
#define servo_timer timers[TIMER_SERVO]

extern uint8_t player_error;
extern int_hl_t grooves;
extern uint8_t  initialized;
extern uint8_t  motor_started;
extern uint8_t  motor_on_speed;
extern uint8_t  reload_servo_timer;

/* From player.c */
extern interface_field_t player_interface;

/* From servo.c */
extern uint8_t dsic_in_focus_pub(void);
extern uint8_t dsic_on_track_pub(void);
extern void    jump_servo_state(void);
extern void    servo_reinit_sledge(void);
extern uint8_t switch_laser_on(void);
extern uint8_t switch_focus_off(void);
extern uint8_t turn_radial_off(void);
extern uint8_t turn_focus_laser_off(void);
extern uint8_t turn_laser_focus_on(void);
extern void    rad_hold(void);
extern void    rad_start(void);
extern void    sledge_off(void);
extern int status_cd6(uint8_t);

/* Convenience shims (servo.c marks these static — call via inline wrappers) */
static uint8_t dsic_in_focus(void)  { return rd_dsic2() & 0x01u ? 0u : 1u; }
static uint8_t dsic_on_track(void)  { return rd_dsic2() & 0x02u ? 0u : 1u; }
static uint8_t sledge_switch(void)  { return rd_dsic2() & 0x04u ? OPEN : CLOSED; }
static uint8_t mute_on(void)        { cd6_wr(MUTE); return READY; }

/* =========================================================================
 * Constants
 * ====================================================================== */

#define HALF_SERVICE_JUMP_TIME   250u   /**< ~2 s max jump time (reload once) */
#define RAD_ON_TIMEOUT           (500u / 8u)  /**< ~500 ms radial-on timeout  */

/* =========================================================================
 * Module state
 * ====================================================================== */

static uint8_t service_active;
static uint8_t service_phase0;
static uint8_t service_phase1;

/**
 * service_process:
 *   high nibble = service command index
 *   low  nibble = function step index within that command
 */
static uint8_t service_process;
static uint8_t service_status;

uint8_t service_command_busy = 0;

/* =========================================================================
 * Step functions — each returns BUSY / READY / CD_ERROR_STATE / PROCESS_READY
 * ====================================================================== */

static uint8_t write_cd6(void)
{
    cd6_wr(player_interface.param1);
    return READY;
}

static uint8_t write_dsic2(void)
{
    wr_dsic2(player_interface.param1);
    return READY;
}

static uint8_t read_dsic2_fn(void)
{
    player_interface.param1 = rd_dsic2();
    return READY;
}

static uint8_t radial_on(void)
{
    if (!service_phase1 && !service_phase0) {
        /* Phase 0 — turn radial off, optionally initialise */
        turn_radial_off();
        if (!initialized) {
            servo_timer = RAD_INITIALIZE_TIME;
            rad_start();
        } else {
            servo_timer = 0;
        }
        service_phase0 = 1;
    }

    if (!service_phase1 && service_phase0) {
        /* Phase 1 — jump 10 tracks to gain radial lock */
        if (servo_timer == 0) {
            rad_hold();
            initialized = 1;
            wr_dsic2(SRCOMM3);
            wr_dsic2(0xFF);
            wr_dsic2(0xF6);
            wr_dsic2(RAD_STAT3);
            servo_timer    = RAD_ON_TIMEOUT;
            service_phase1 = 1;
            service_phase0 = 0;
        }
    }

    if (service_phase1 && !service_phase0) {
        /* Phase 2 — wait for radial lock */
        if (servo_timer == 0) {
            player_error = RADIAL_ERROR;
            return CD_ERROR_STATE;
        }
        if (dsic_on_track()) {
            cd6_wr(MOT_PLAYM_ACTIVE);
            return READY;
        }
    }

    return BUSY;
}

static uint8_t start_motor(void)
{
    if (motor_on_speed) return READY;

    if (!service_phase1 && !service_phase0) {
        servo_timer    = FOCUS_TIME_OUT;
        service_phase0 = 1;
    }

    if (!service_phase1 && service_phase0) {
        if (servo_timer == 0) { player_error = FOCUS_ERROR; return CD_ERROR_STATE; }
        if (dsic_in_focus()) {
            servo_timer    = SPEEDUP_TIME;
            cd6_wr(MOT_STRTM1_ACTIVE);
            motor_started  = 1;
            service_phase1 = 1;
            service_phase0 = 0;
        }
    }

    if (service_phase1 && !service_phase0) {
        if (servo_timer == 0) {
            servo_timer    = NOMINAL_SPEED_TIME;
            cd6_wr(MOT_STRTM2_ACTIVE);
            service_phase0 = 1;
        }
    }

    if (service_phase1 && service_phase0) {
        if (servo_timer == 0) { player_error = MOTOR_ERROR; return CD_ERROR_STATE; }
        if (status_cd6(MOT_STRT_1)) { motor_on_speed = 1; return READY; }
    }

    return BUSY;
}

static uint8_t stop_motor(void)
{
    cd6_wr(MOT_OFF_ACTIVE);
    motor_started  = 0;
    motor_on_speed = 0;
    initialized    = 0;
    return READY;
}

static uint8_t set_normal_mode(void)
{
    servo_reinit_sledge();
    return PROCESS_READY;
}

static uint8_t monitor_focussing(void)
{
    return dsic_in_focus() ? READY : BUSY;
}

static uint8_t service_process_ready(void)
{
    return PROCESS_READY;
}

static uint8_t move_sledge(void)
{
    if (!service_phase0) {
        servo_timer    = player_interface.param2;
        wr_dsic2(SRSLEDGE);
        wr_dsic2(player_interface.param1);
        service_phase0 = 1;
    }

    /* Timeout or sledge hit home while moving inward */
    if (servo_timer == 0 ||
        (sledge_switch() == CLOSED && (player_interface.param1 & 0x80u)))
    {
        sledge_off();
        return READY;
    }
    return BUSY;
}

static uint8_t service_jump(void)
{
    if (!service_phase0) {
        /* Phase 0 — check readiness and start jump */
        if (!dsic_in_focus()) { player_error = FOCUS_ERROR;  return CD_ERROR_STATE; }
        if (!status_cd6(MOT_STRT_1)) { player_error = MOTOR_ERROR; return CD_ERROR_STATE; }
        if (!dsic_on_track()) { player_error = RADIAL_ERROR; return CD_ERROR_STATE; }

        grooves.b.high = player_interface.param1;
        grooves.b.low  = player_interface.param2;
        grooves.val    = -grooves.val;

        jump_servo_state();

        service_phase0     = 1;
        servo_timer        = HALF_SERVICE_JUMP_TIME;
        reload_servo_timer = 1;
        return BUSY;
    }

    /* Phase 1 — monitor jump */
    if (dsic_on_track()) {
        cd6_wr(MOT_PLAYM_ACTIVE);
        return READY;
    }
    if (servo_timer == 0) {
        if (reload_servo_timer) {
            servo_timer        = HALF_SERVICE_JUMP_TIME;
            reload_servo_timer = 0;
            return BUSY;
        }
        player_error = RADIAL_ERROR;
        return CD_ERROR_STATE;
    }
    return BUSY;
}

/* =========================================================================
 * Service command dispatch table
 *
 * Indexed by (process_id - (MAX_LEGAL_NORMAL_ID + 1)), matching the opcodes:
 *   0 = ENTER_NORMAL_MODE_OPC
 *   1 = LASER_ON_OPC
 *   2 = LASER_OFF_OPC
 *   …
 *  13 = READ_DSIC2_OPC
 * ====================================================================== */

#define MAX_SVC_STEPS 5

typedef uint8_t (*svc_fn_t)(void);

static const svc_fn_t service_functions[14][MAX_SVC_STEPS] = {
/* 0  ENTER_NORMAL_MODE */ { mute_on,            turn_radial_off, stop_motor, turn_focus_laser_off, set_normal_mode  },
/* 1  LASER_ON          */ { switch_laser_on,    service_process_ready, NULL, NULL, NULL             },
/* 2  LASER_OFF         */ { turn_radial_off,    stop_motor, turn_focus_laser_off, service_process_ready, NULL      },
/* 3  FOCUS_ON          */ { turn_laser_focus_on,monitor_focussing, service_process_ready, NULL, NULL               },
/* 4  FOCUS_OFF         */ { turn_radial_off,    stop_motor, switch_focus_off, service_process_ready, NULL          },
/* 5  SPINDLE_MOTOR_ON  */ { turn_laser_focus_on,start_motor, service_process_ready, NULL, NULL                     },
/* 6  SPINDLE_MOTOR_OFF */ { turn_radial_off,    stop_motor, service_process_ready, NULL, NULL                      },
/* 7  RADIAL_ON         */ { turn_laser_focus_on,start_motor, radial_on, service_process_ready, NULL                },
/* 8  RADIAL_OFF        */ { turn_radial_off,    service_process_ready, NULL, NULL, NULL                            },
/* 9  MOVE_SLEDGE       */ { turn_radial_off,    move_sledge, service_process_ready, NULL, NULL                     },
/*10  JUMP_GROOVES      */ { service_jump,       service_process_ready, NULL, NULL, NULL                            },
/*11  WRITE_CD6         */ { write_cd6,          service_process_ready, NULL, NULL, NULL                            },
/*12  WRITE_DSIC2       */ { write_dsic2,        service_process_ready, NULL, NULL, NULL                            },
/*13  READ_DSIC2        */ { read_dsic2_fn,      service_process_ready, NULL, NULL, NULL                            },
};

/* =========================================================================
 * Public API
 * ====================================================================== */

uint8_t service(uint8_t service_cmd)
{
    if ((service_process >> 4) != service_cmd || !service_command_busy) {
        service_process    = (uint8_t)(service_cmd << 4);
        service_phase0     = 0;
        service_phase1     = 0;
        service_active     = 1;
        service_status     = BUSY;
        service_command_busy = 1;
    }
    if (service_status != BUSY)
        service_command_busy = 0;
    return service_status;
}

void execute_service_functions(void)
{
    if (!service_active) return;

    uint8_t cmd_idx  = service_process >> 4;
    uint8_t step_idx = service_process & 0x0Fu;

    if (cmd_idx >= 14u || step_idx >= MAX_SVC_STEPS) {
        service_status = PROCESS_READY;
        service_active = 0;
        return;
    }

    svc_fn_t fn = service_functions[cmd_idx][step_idx];
    if (fn == NULL) {
        service_status = PROCESS_READY;
        service_active = 0;
        return;
    }

    switch (fn()) {
    case PROCESS_READY:
        service_status = PROCESS_READY;
        service_active = 0;
        break;

    case READY:
        service_process++;   /* advance step index (low nibble) */
        service_phase0 = 0;
        service_phase1 = 0;
        break;

    case CD_ERROR_STATE:
        service_status = CD_ERROR_STATE;
        service_active = 0;
        /* Error recovery */
        if      (player_error == RADIAL_ERROR) { turn_radial_off(); }
        else if (player_error == MOTOR_ERROR)  { turn_radial_off(); stop_motor(); }
        else if (player_error == FOCUS_ERROR)  { turn_radial_off(); stop_motor(); switch_focus_off(); }
        break;

    case BUSY:
    default:
        /* Still working */
        break;
    }
}
