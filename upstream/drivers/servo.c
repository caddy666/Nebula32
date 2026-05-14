/**
 * @file  servo.c
 * @brief Spindle / focus / radial / sledge servo state machine.
 *
 * Controls the CXD2500BQ (via cd6_wr) and DSIC2 (via wr_dsic2/rd_dsic2)
 * to perform disc spin-up, focus acquisition, radial lock, and all jump
 * operations.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 * All 8051-specific constructs removed; `bit` → `uint8_t`; `idat` removed.
 */

#include <stdlib.h>    /* abs() */
#include <stdint.h>

#include "defs.h"
#include "serv_def.h"
#include "dsic2.h"
#include "driver.h"
#include "timer.h"

/* =========================================================================
 * External references — provided by driver.c / timer.c / subcode.c
 * ====================================================================== */
extern volatile uint8_t timers[];   /* software timer array, see timer.h */
#define servo_timer      timers[TIMER_SERVO]
#define kick_brake_timer timers[TIMER_KICK_BRAKE]

extern uint8_t  player_error;
extern uint8_t  hex_abs_min;
extern uint8_t  simulation_timer;
extern volatile uint8_t scor_edge;

/* From subcode.c */
extern void    start_subcode_reading(void);
extern uint8_t i_can_read_subcode;

/* =========================================================================
 * Servo module state
 * ====================================================================== */
static uint8_t servo_exec_state;      /**< BUSY / READY / CD_ERROR_STATE  */
static uint8_t servo_state;           /**< Current state-machine state     */
static uint8_t servo_requested_state; /**< Next state set by API callers   */
static uint8_t servo_retries;         /**< Retry counter                   */

static uint16_t off_track_value;      /**< Last sampled DSIC2 off-track     */

int_hl_t grooves;                     /**< Requested jump in grooves        */

static uint8_t n2_speed_req;          /**< 1 = double speed requested       */
static uint8_t rad_recover_in_jump;
static uint8_t foc_recover_in_jump;
static uint8_t no_efm_in_jump;

uint8_t initialized;                  /**< Radial init done for this disc   */
uint8_t motor_started;                /**< Spindle motor has been turned on */
uint8_t motor_on_speed;               /**< Spindle at ≥75% nominal speed    */
uint8_t reload_servo_timer;           /**< Timer reload flag                */
uint8_t disc_size;                    /**< DISC_8CM or DISC_12CM            */
uint8_t disc_size_known;

static uint8_t kick;   /**< 1 = kick phase active during long jump */
static uint8_t brk;    /**< 1 = brake phase active during long jump */

/* n1_speed: 1 = single speed, 0 = double speed (mirrors the driver flag) */
extern int n1_speed;   /* defined in driver.c */

/* =========================================================================
 * Private: DSIC2 status readers
 * ====================================================================== */

static uint8_t dsic_in_focus(void)
{
    uint8_t v = rd_dsic2();
    return (v & 0x01u) ? 1u : 0u;  /* bit 0 = FE (focus error: 0=in focus) */
}

static uint8_t dsic_on_track(void)
{
    uint8_t v = rd_dsic2();
    return (v & 0x02u) ? 0u : 1u;  /* bit 1 = TE (tracking error: 0=on track) */
}

static int16_t read_off_track_value(void)
{
    /* Read the 16-bit signed off-track counter from two consecutive DSIC2 reads */
    uint8_t  hi = rd_dsic2();
    uint8_t  lo = rd_dsic2();
    return (int16_t)(((uint16_t)hi << 8) | lo);
}

static uint8_t sledge_switch(void)
{
    /* The sledge home switch is wired to a DSIC2 status bit.
     * Returns CLOSED (0) when sledge is at home position. */
    uint8_t v = rd_dsic2();
    return (v & 0x04u) ? OPEN : CLOSED;
}

/* =========================================================================
 * Private: sledge and radial helpers
 * ====================================================================== */

static void sledge_in(void)
{
    wr_dsic2(SRSLEDGE);
    wr_dsic2(SLEDGE_UOUT_IN);
}

static void sledge_out(void)
{
    wr_dsic2(SRSLEDGE);
    wr_dsic2(SLEDGE_UOUT_OUT);
}

void sledge_off(void)
{
    wr_dsic2(SRSLEDGE);
    wr_dsic2(SLEDGE_UOUT_OFF);
}

void rad_start(void)
{
    /* Begin radial gain/offset initialisation */
    wr_dsic2(0x21);   /* DSIC2 radial start command */
}

void rad_hold(void)
{
    /* End radial initialisation and switch to hold mode */
    wr_dsic2(0x22);   /* DSIC2 radial hold command */
}

static void wr_dsic2_array(const uint8_t *arr, uint8_t len)
{
    while (len--) wr_dsic2(*arr++);
}

/* =========================================================================
 * Private: motor gain setting
 * ====================================================================== */

static void set_motor_gain(void)
{
    if (disc_size == DISC_8CM)
        cd6_wr(n1_speed ? MOT_GAIN_8CM_N1 : MOT_GAIN_8CM_N2);
    else
        cd6_wr(n1_speed ? MOT_GAIN_12CM_N1 : MOT_GAIN_12CM_N2);
}

/* =========================================================================
 * Public servo helpers (called from service.c)
 * ====================================================================== */

uint8_t switch_laser_on(void)
{
    wr_dsic2(PRESET);
    wr_dsic2(LASER_ON);
    return READY;
}

static void switch_laser_off(void)
{
    wr_dsic2(PRESET);
    wr_dsic2(LASER_OFF);
}

uint8_t switch_focus_off(void)
{
    /* Switch focus servo off, laser stays on */
    static const uint8_t focus_off_array[] = { 0x11, 0x00 };  /* DSIC2 focus off sequence */
    wr_dsic2_array(focus_off_array, sizeof(focus_off_array));
    return READY;
}

uint8_t turn_radial_off(void)
{
    rad_hold();
    sledge_off();
    return READY;
}

uint8_t turn_focus_laser_off(void)
{
    switch_focus_off();
    switch_laser_off();
    return READY;
}

uint8_t turn_laser_focus_on(void)
{
    switch_laser_on();
    /* Enable focus servo */
    static const uint8_t focus_on_array[] = { 0x10, 0x01 };   /* DSIC2 focus on sequence */
    wr_dsic2_array(focus_on_array, sizeof(focus_on_array));
    return READY;
}

void jump_servo_state(void);   /* forward declaration for service.c use */

/* =========================================================================
 * Private: active brake guard
 * ====================================================================== */

static uint8_t active_brake_ok(void)
{
    if (!disc_size) return 0;
    if (!door_closed()) return 0;
    return 1;
}

/* =========================================================================
 * Private: jump helpers
 * ====================================================================== */

static uint8_t calc_kick(void)
{
    int time;
    uint8_t offset;

    if      (get_area() == 1) offset = 6;
    else if (get_area() == 2) offset = (off_track_value > 4000) ? 6 : 1;
    else    offset = n1_speed ? 0 : ((off_track_value > 10000) ? 9 : 0);

    time = (int)(off_track_value >> 3);
    time = time * 3;
    time = time >> 7;
    time += n1_speed ? offset : (int)(offset << 2);
    return (uint8_t)time;
}

static uint8_t calc_brake(void)
{
    int time;
    if (n1_speed) {
        if      (get_area() == 1) time = 6;
        else if (get_area() == 2) time = 1;
        else                      time = 0;
    } else {
        if      (get_area() == 1) time = 16;
        else if (get_area() == 2) time = 3;
        else                      time = 0;
    }
    return (uint8_t)time;
}

static void jump_short(void)
{
    wr_dsic2(SRCOMM3);
    wr_dsic2(grooves.b.high);
    wr_dsic2(grooves.b.low);
    wr_dsic2(RAD_STAT3);
}

static void jump_long(int8_t brake)
{
    wr_dsic2(SRCOMM5);
    wr_dsic2((uint8_t)brake);
    wr_dsic2(SLEDGE_UOUT_JMP);
    wr_dsic2(grooves.b.high);
    wr_dsic2(grooves.b.low);
    wr_dsic2(RAD_STAT5);
}

/* =========================================================================
 * State functions
 * ====================================================================== */

static void init_dsic2_state(void)
{
    /* Reset DSIC2 with preset sequence */
    wr_dsic2(PRESET);
    wr_dsic2(0x00);
    servo_state = INIT_SLEDGE;
}

static void init_sledge_state(void)
{
    if (sledge_switch() == CLOSED) {
        sledge_out();
        servo_timer = SLEDGE_OUT_TIME;
        servo_state = CHECK_OUT_SLEDGE;
    } else {
        sledge_in();
        servo_timer       = HALF_SLEDGE_IN_TIME;
        reload_servo_timer = 1;
        servo_state        = CHECK_IN_SLEDGE;
    }
}

static void check_in_sledge_state(void)
{
    if (servo_timer == 0) {
        if (reload_servo_timer) {
            servo_timer       = HALF_SLEDGE_IN_TIME;
            reload_servo_timer = 0;
        } else {
            sledge_off();
            servo_state             = SERVO_IDLE;
            servo_requested_state   = SERVO_IDLE;
            player_error            = SLEDGE_ERROR;
            servo_exec_state        = CD_ERROR_STATE;
        }
    } else {
        if (sledge_switch() == CLOSED) {
            sledge_off();
            sledge_out();
            servo_timer = SLEDGE_OUT_TIME;
            servo_state = CHECK_OUT_SLEDGE;
        }
    }
}

static void check_out_sledge_state(void)
{
    if (servo_timer == 0) {
        sledge_off();
        servo_state           = SERVO_IDLE;
        servo_requested_state = SERVO_IDLE;
        player_error          = SLEDGE_ERROR;
        servo_exec_state      = CD_ERROR_STATE;
    } else {
        if (sledge_switch() == OPEN) {
            sledge_off();
            servo_state = SERVO_IDLE;
        }
    }
}

static void servo_idle_state(void)
{
    if (servo_requested_state == START_FOCUS) {
        servo_state           = START_FOCUS;
        servo_requested_state = SERVO_MONITOR;
    } else if (servo_requested_state == JUMP_SERVO) {
        servo_requested_state = SERVO_IDLE;
        servo_exec_state      = CD_ERROR_STATE;
        player_error          = ILLEGAL_COMMAND;
    } else if (servo_exec_state != CD_ERROR_STATE) {
        servo_exec_state = READY;
    }
}

static void start_focus_state(void)
{
    turn_laser_focus_on();
    servo_timer   = FOCUS_TIME_OUT;
    servo_state   = CHECK_FOCUS;
    servo_retries = MAX_RETRIES;
}

static void check_focus_state(void)
{
    if (servo_timer == 0) {
        turn_focus_laser_off();
        player_error          = FOCUS_ERROR;
        servo_exec_state      = CD_ERROR_STATE;
        servo_state           = STOP_SERVO;
        servo_requested_state = SERVO_IDLE;
    } else if (dsic_in_focus()) {
        servo_timer = TIME_DOUBLE_FOCUS_CHECK;
        servo_state = DOUBLE_CHECK_FOCUS;
    }
}

static void double_check_focus_state(void)
{
    if (servo_timer == 0) {
        servo_state = dsic_in_focus() ? START_TTM : FOCUS_RECOVER;
    }
}

static void start_ttm_state(void)
{
    cd6_wr(MOT_STRTM1_ACTIVE);
    servo_timer   = SPEEDUP_TIME;
    servo_state   = TTM_SPEED_UP;
    motor_started = 1;
}

static void ttm_speedup_state(void)
{
    if (dsic_in_focus()) {
        if (servo_timer == 0) {
            cd6_wr(MOT_STRTM2_ACTIVE);
            servo_state = CHECK_TTM;
        }
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void check_ttm_state(void)
{
    if (dsic_in_focus()) {
        if (status_cd6(MOT_STRT_1)) {
            set_motor_gain();
            motor_on_speed = 1;
            if (!hf_present()) {
                servo_state           = STOP_SERVO;
                servo_requested_state = SERVO_IDLE;
                servo_exec_state      = CD_ERROR_STATE;
                player_error          = HF_DETECTOR_ERROR;
            } else {
                servo_state = START_RADIAL;
            }
        }
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void start_radial_state(void)
{
    servo_timer = 0;
    if (dsic_in_focus()) {
        if (!initialized) {
            rad_start();
            servo_timer = RAD_INITIALIZE_TIME;
        }
        servo_state = INIT_RADIAL;
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void init_radial_state(void)
{
    if (dsic_in_focus()) {
        if (servo_timer == 0) {
            rad_hold();
            initialized    = 1;
            grooves.val    = -10;
            servo_state    = JUMP_SERVO;
        }
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void upto_n2_state(void)
{
    if (servo_timer < UPTO_N2_TIME) {
        cd6_wr(MOT_PLAYM_ACTIVE);
        start_subcode_reading();
        servo_state = WAIT_SUBCODE;
        servo_timer = SUBCODE_TIME_OUT;
    }
}

static void downto_n1_state(void)
{
    if (!n1_speed) {
        if (!status_cd6(MOT_STRT_2) || servo_timer == 0) {
            cd6_wr(SPEED_CONTROL_N1);
            cd6_wr(MOT_STRTM2_ACTIVE);
            n1_speed = 1;
            set_motor_gain();
        }
    } else {
        if (status_cd6(MOT_STRT_1)) {
            cd6_wr(MOT_PLAYM_ACTIVE);
            start_subcode_reading();
            servo_state = WAIT_SUBCODE;
            servo_timer = SUBCODE_TIME_OUT;
        }
    }
}

static void servo_monitor_state(void)
{
    if (dsic_in_focus()) {
        if (dsic_on_track() && (i_can_read_subcode || servo_timer != 0)) {
            if (i_can_read_subcode) {
                i_can_read_subcode = 0;
                servo_timer        = SUBCODE_MONITOR_TIMEOUT;
                servo_retries      = MAX_RETRIES;
            }
            if (servo_requested_state == JUMP_SERVO) {
                servo_state           = JUMP_SERVO;
                servo_requested_state = SERVO_MONITOR;
            } else if (!n1_speed && !n2_speed_req) {
                servo_state = DOWNTO_N1;
                cd6_wr(MOT_BRM2_ACTIVE);
                servo_timer = N2_TO_N1_BRAKE_TIME;
            } else if (n1_speed && n2_speed_req) {
                servo_state = UPTO_N2;
                cd6_wr(SPEED_CONTROL_N2);
                cd6_wr(MOT_STRTM2_ACTIVE);
                n1_speed = 0;
                set_motor_gain();
            } else {
                servo_requested_state = SERVO_MONITOR;
                servo_exec_state      = READY;
            }
        } else {
            servo_state = RADIAL_RECOVER;
        }
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void radial_recover_state(void)
{
    servo_retries--;
    if (servo_retries == 0) {
        if (!hf_present())
            player_error = HF_DETECTOR_ERROR;
        else if (dsic_in_focus() && dsic_on_track())
            player_error = SUBCODE_TIMEOUT_ERROR;
        else
            player_error = RADIAL_ERROR;

        servo_exec_state      = CD_ERROR_STATE;
        servo_state           = STOP_SERVO;
        servo_requested_state = SERVO_IDLE;
    } else {
        turn_radial_off();
        if (sledge_switch() == CLOSED) {
            sledge_out();
            servo_timer = R_REC_OUT_SLEDGE;
            servo_state = SLEDGE_OUTSIDE_RECOVER;
        } else {
            servo_state = INIT_RADIAL;
            servo_timer = 0;
        }
    }
}

static void focus_recover_state(void)
{
    turn_radial_off();
    cd6_wr(MOT_OFF_ACTIVE);
    servo_retries--;
    if (servo_retries == 0) {
        servo_exec_state      = CD_ERROR_STATE;
        servo_state           = STOP_SERVO;
        servo_requested_state = SERVO_IDLE;
        player_error          = no_efm_in_jump ? HF_DETECTOR_ERROR : FOCUS_ERROR;
    } else {
        servo_timer = F_REC_IN_SLEDGE;
        servo_state = SLEDGE_INSIDE_RECOVER;
        sledge_in();
    }
}

static void sledge_inside_recover_state(void)
{
    if (servo_timer == 0 || sledge_switch() == CLOSED) {
        sledge_off();
        servo_timer = FOCUS_TIME_OUT;
        servo_state = CHECK_FOCUS;
    }
}

static void sledge_outside_recover_state(void)
{
    if (dsic_in_focus()) {
        if (servo_timer == 0) {
            sledge_off();
            servo_state = INIT_RADIAL;
            servo_timer = 0;
        }
    } else {
        servo_state = FOCUS_RECOVER;
    }
}

static void stop_servo_state(void)
{
    servo_state = CHECK_STOP_SERVO;
    if (motor_started) {
        if (motor_on_speed && dsic_on_track() && dsic_in_focus() && active_brake_ok()) {
            cd6_wr(MOT_BRM2_ACTIVE);
            servo_timer = STOP_TIME_OUT;
        } else {
            turn_radial_off();
            turn_focus_laser_off();
            cd6_wr(MOT_OFF_ACTIVE);
            servo_timer        = MOT_OFF_STOP_TIME;
            reload_servo_timer = 1;
            initialized        = 0;
            motor_on_speed     = (uint8_t)!n1_speed;
            if (!active_brake_ok()) {
                servo_timer   = 0;
                motor_started = 0;
            }
        }
    } else {
        turn_focus_laser_off();
        servo_timer        = 0;
        reload_servo_timer = 0;
    }
}

static void check_stop_servo_state(void)
{
    if (servo_requested_state == START_FOCUS) {
        servo_state           = START_FOCUS;
        servo_requested_state = SERVO_MONITOR;
        turn_radial_off();
        cd6_wr(MOT_OFF_ACTIVE);
        motor_on_speed = 0;
    } else if (motor_started) {
        if (initialized) {
            if (status_cd6(MOT_STOP) || servo_timer == 0) {
                turn_radial_off();
                turn_focus_laser_off();
                cd6_wr(MOT_OFF_ACTIVE);
                uint8_t elapsed = (uint8_t)STOP_TIME_OUT - servo_timer;
                servo_timer        = (elapsed & 0x80u) ? 255u : (uint8_t)(elapsed << 1);
                reload_servo_timer = 1;
                motor_started      = 0;
            }
        } else {
            if (servo_timer == 0) {
                servo_timer = MOT_OFF_STOP_TIME;
                if (reload_servo_timer)      reload_servo_timer = 0;
                else if (motor_on_speed)     motor_on_speed     = 0;
                else                         motor_started      = 0;
            }
        }
    } else {
        if (servo_timer == 0) {
            if (reload_servo_timer) {
                servo_timer        = EXTRA_STOP_DELAY;
                reload_servo_timer = 0;
            } else {
                servo_state = INIT_SLEDGE;
                cd6_wr(SPEED_CONTROL_N1);
                n1_speed       = 1;
                set_motor_gain();
                initialized    = 0;
                motor_on_speed = 0;
                if (servo_requested_state == STOP_SERVO)
                    servo_exec_state = READY;
            }
        }
    }
}

void jump_servo_state(void)
{
    int8_t brake_dist;

    kick = 0;
    brk  = 0;

    off_track_value = (uint16_t)abs(grooves.val);

    if (off_track_value < (uint16_t)MAX) {
        jump_short();
    } else if (off_track_value <= (uint16_t)BRAKE_2_DIS_MAX) {
        brake_dist = (int8_t)(-(int8_t)(((off_track_value + 16u) >> 5u))) - 1;
        cd6_wr(MOT_OFF_ACTIVE);
        jump_long(brake_dist);
    } else {
        brake_dist = (int8_t)((int)BRAKE_DIS_MAX / -16);
        if (grooves.val > 0) {
            kick_brake_timer = calc_kick();
            cd6_wr(MOT_STRTM1_ACTIVE);
            kick = 1;
        } else {
            brk              = 1;
            kick_brake_timer = calc_brake();
            cd6_wr(MOT_BRM2_ACTIVE);
        }
        jump_long(brake_dist);
    }

    servo_state = CHECK_JUMP;
    servo_timer = SKATING_DELAY_CHECK;
}

static void check_jump_state(void)
{
    if (dsic_in_focus()) {
        if (hf_present()) {
            if ((kick || brk) && kick_brake_timer == 0) {
                cd6_wr(MOT_OFF_ACTIVE);
                kick = 0;
                brk  = 0;
            }
            if (dsic_on_track()) {
                cd6_wr(MOT_PLAYM_ACTIVE);
                start_subcode_reading();
                servo_state = WAIT_SUBCODE;
                servo_timer = SUBCODE_TIME_OUT;
            } else if (servo_timer == 0) {
                uint16_t new_ot = (uint16_t)abs(read_off_track_value());
                if (new_ot <= off_track_value) {
                    if ((int)(off_track_value - new_ot) < 10) {
                        servo_state        = RADIAL_RECOVER;
                        rad_recover_in_jump = 1;
                    } else {
                        off_track_value = new_ot;
                    }
                } else if (new_ot > 200u) {
                    servo_state         = RADIAL_RECOVER;
                    rad_recover_in_jump = 1;
                }
                servo_timer = SKATING_SAMPLE_TIME;
            }
        } else {
            no_efm_in_jump = 1;
            servo_state    = FOCUS_RECOVER;
            kick = 0; brk = 0;
        }
    } else {
        foc_recover_in_jump = 1;
        servo_state         = FOCUS_RECOVER;
        kick = 0; brk = 0;
    }
}

static void wait_subcode_state(void)
{
    if (!status_cd6(SUBCODE_READY)) {
        servo_state = SERVO_MONITOR;
        servo_timer = SUBCODE_MONITOR_TIMEOUT;
    } else if (servo_timer == 0) {
        servo_state         = RADIAL_RECOVER;
        rad_recover_in_jump = 1;
    }
}

/* =========================================================================
 * State dispatch table
 * ====================================================================== */

typedef void (*servo_fn_t)(void);

static const servo_fn_t servo_functions_array[] = {
    init_dsic2_state,            /* INIT_DSIC2             = 0  */
    init_sledge_state,           /* INIT_SLEDGE            = 1  */
    check_in_sledge_state,       /* CHECK_IN_SLEDGE        = 2  */
    check_out_sledge_state,      /* CHECK_OUT_SLEDGE       = 3  */
    servo_idle_state,            /* SERVO_IDLE             = 4  */
    start_focus_state,           /* START_FOCUS            = 5  */
    check_focus_state,           /* CHECK_FOCUS            = 6  */
    double_check_focus_state,    /* DOUBLE_CHECK_FOCUS     = 7  */
    start_ttm_state,             /* START_TTM              = 8  */
    ttm_speedup_state,           /* TTM_SPEED_UP           = 9  */
    check_ttm_state,             /* CHECK_TTM              = 10 */
    start_radial_state,          /* START_RADIAL           = 11 */
    init_radial_state,           /* INIT_RADIAL            = 12 */
    servo_monitor_state,         /* SERVO_MONITOR          = 13 */
    radial_recover_state,        /* RADIAL_RECOVER         = 14 */
    focus_recover_state,         /* FOCUS_RECOVER          = 15 */
    sledge_inside_recover_state, /* SLEDGE_INSIDE_RECOVER  = 16 */
    sledge_outside_recover_state,/* SLEDGE_OUTSIDE_RECOVER = 17 */
    stop_servo_state,            /* STOP_SERVO             = 18 */
    check_stop_servo_state,      /* CHECK_STOP_SERVO       = 19 */
    jump_servo_state,            /* JUMP_SERVO             = 20 */
    check_jump_state,            /* CHECK_JUMP             = 21 */
    wait_subcode_state,          /* WAIT_SUBCODE           = 22 */
    upto_n2_state,               /* UPTO_N2                = 23 */
    downto_n1_state,             /* DOWNTO_N1              = 24 */
};

/* =========================================================================
 * Public API
 * ====================================================================== */

void servo_init(void)
{
    servo_requested_state = SERVO_IDLE;
    servo_state           = INIT_DSIC2;
    servo_exec_state      = BUSY;
    n1_speed              = 1;
    disc_size             = DISC_12CM;
    n2_speed_req          = (N == 2) ? 1u : 0u;
}

void servo_start(void)
{
    servo_requested_state = START_FOCUS;
    servo_exec_state      = BUSY;
    rad_recover_in_jump   = 0;
    foc_recover_in_jump   = 0;
    no_efm_in_jump        = 0;
}

void servo_reinit_sledge(void)
{
    servo_requested_state = SERVO_IDLE;
    servo_state           = INIT_SLEDGE;
    servo_exec_state      = BUSY;
}

void servo_n1(void) { n2_speed_req = 0; servo_exec_state = BUSY; }
void servo_n2(void) { n2_speed_req = 1; servo_exec_state = BUSY; }

void servo_stop(void)
{
    if (servo_requested_state != STOP_SERVO &&
        servo_requested_state != SERVO_IDLE)
    {
        servo_requested_state = STOP_SERVO;
        servo_state           = STOP_SERVO;
    }
    servo_exec_state = BUSY;
}

void servo_jump(int jump_size)
{
    grooves.val         = -jump_size;
    servo_retries       = MAX_RETRIES;
    servo_requested_state = JUMP_SERVO;
    servo_exec_state    = BUSY;
    rad_recover_in_jump = 0;
    foc_recover_in_jump = 0;
    no_efm_in_jump      = 0;
}

uint8_t get_servo_process_state(void) { return servo_exec_state; }

uint8_t get_jump_status(void)
{
    uint8_t s = 0;
    if (no_efm_in_jump)      s |= NO_HF_ON_TARGET;
    if (rad_recover_in_jump) s |= RADIAL_RECOVERY_DONE;
    if (foc_recover_in_jump) s |= FOCUS_RECOVERY_DONE;
    return s;
}

uint8_t servo_tracking(void)
{
    return (servo_requested_state == SERVO_MONITOR ||
            servo_requested_state == JUMP_SERVO) ? 1u : 0u;
}

uint8_t servo_to_service(void)
{
    return (servo_state == SERVO_IDLE ||
            servo_state == SERVO_MONITOR) ? 1u : 0u;
}

void servo(void)
{
    servo_functions_array[servo_state]();
}
