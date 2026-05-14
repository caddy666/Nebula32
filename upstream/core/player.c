/**
 * @file  player.c
 * @brief Top-level player module.
 *
 * The player module owns the process dispatch table — a 2-D array of
 * (function, parameter) pairs that maps each command opcode to a sequence
 * of sub-module calls.  On each main-loop tick it advances one step in the
 * active sequence and checks the return value.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 * All 8051-specific constructs (bit type, LCALL, idat, rom) removed.
 */

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "player.h"
#include "driver.h"
#include "timer.h"
#include "serv_def.h"

/* =========================================================================
 * Forward declarations for sub-modules called from the process table.
 * These are implemented in servo.c, play.c, service.c and shock.c.
 * Stub implementations are provided at the bottom of this file so the
 * project links without those files present.
 * ====================================================================== */

/* Sub-module step functions — each returns BUSY / READY / ERROR / PROCESS_READY */
static byte start_stop(byte cmd);
static byte play(byte cmd);
static byte service(byte cmd);
static byte player_module_fn(byte cmd);

/* Sub-module "execute" functions called unconditionally each tick */
static void execute_start_stop_functions(void);
static void execute_play_functions(void);
static void execute_service_functions(void);
static void servo(void);
static void subcode_module(void);
static void shock_recover(void);

/* Servo helpers */
static uint8_t servo_to_service(void);
static void init_for_new_disc(void);
static void servo_init(void);
static void cd6_init(void);

/* =========================================================================
 * Module-level globals (shared with dispatcher/command handler via player.h)
 * ====================================================================== */

interface_field_t player_interface;
byte              player_error  = NO_ERROR;
byte              process_id    = IDLE_OPC;
byte              function_id   = 0;

static uint8_t    service_mode  = 0;  /**< 1 when in diagnostic service mode */

/* Busy flags polled by the servo/play/start-stop modules */
static uint8_t play_monitor              = 0;
static uint8_t play_command_busy         = 0;
static uint8_t start_stop_command_busy   = 0;
static uint8_t service_command_busy      = 0;

/* =========================================================================
 * Process dispatch table
 *
 * processes[process_id][function_id] = {function_pointer, parameter}
 *
 * Each row describes one command sequence.  The sequence advances through
 * function_id 0..4 until a step returns PROCESS_READY (sequence done) or
 * ERROR (triggers error-handling sequence).  A null function pointer with
 * parameter 0 terminates a row early.
 *
 * process_id = opcode + 1  (so 0 is reserved for ERROR_HANDLING_ID)
 * ====================================================================== */

#define MAX_STEPS  5

typedef struct {
    byte (*fn)(byte);  /**< Sub-module step function, or NULL = end-of-seq */
    byte  param;       /**< Parameter passed to fn()                       */
} process_step_t;

/* Convenience NULL terminator */
#define STEP_END   { NULL, 0 }

static const process_step_t processes[][MAX_STEPS] = {

/* [0] ERROR_HANDLING — stop disc and report */
{
    { player_module_fn, PLAYER_HANDLE_ERROR },
    { play,             PLAY_IDLE           },
    { start_stop,       SS_MOTOR_OFF        },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END
},

/* [1] TRAY_OUT_OPC  — not implemented in this hardware revision */
{ STEP_END, STEP_END, STEP_END, STEP_END, STEP_END },

/* [2] TRAY_IN_OPC  — not implemented */
{ STEP_END, STEP_END, STEP_END, STEP_END, STEP_END },

/* [3] START_UP_OPC — not implemented */
{ STEP_END, STEP_END, STEP_END, STEP_END, STEP_END },

/* [4] STOP_OPC */
{
    { play,             PLAY_IDLE           },
    { start_stop,       SS_STOP             },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END, STEP_END
},

/* [5] PLAY_TRACK_OPC — not implemented */
{ STEP_END, STEP_END, STEP_END, STEP_END, STEP_END },

/* [6] PAUSE_ON_OPC */
{
    { start_stop,       SS_IDLE             },
    { play,             PAUSE_ON            },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END, STEP_END
},

/* [7] PAUSE_OFF_OPC */
{
    { start_stop,       SS_IDLE             },
    { play,             PAUSE_OFF           },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END, STEP_END
},

/* [8] SEEK_OPC */
{
    { play,             PLAY_IDLE           },
    { start_stop,       SS_START_UP         },
    { play,             JUMP_TO_ADDRESS     },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END
},

/* [9] READ_TOC_OPC */
{
    { play,             PLAY_IDLE           },
    { start_stop,       SS_START_UP         },
    { play,             PLAY_READ_TOC       },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END
},

/* [10] READ_SUBCODE_OPC */
{
    { play,             PLAY_READ_SUBCODE   },
    STEP_END, STEP_END, STEP_END, STEP_END
},

/* [11] SINGLE_SPEED_OPC */
{
    { start_stop,       SS_IDLE                     },
    { play,             PLAY_PREPARE_SPEED_CHANGE   },
    { start_stop,       SS_SPEED_N1                 },
    { play,             PLAY_RESTORE_SPEED_CHANGE   },
    { player_module_fn, PLAYER_IDLE                 }
},

/* [12] DOUBLE_SPEED_OPC */
{
    { start_stop,       SS_IDLE                     },
    { play,             PLAY_PREPARE_SPEED_CHANGE   },
    { start_stop,       SS_SPEED_N2                 },
    { play,             PLAY_RESTORE_SPEED_CHANGE   },
    { player_module_fn, PLAYER_IDLE                 }
},

/* [13] SET_VOLUME_OPC — not implemented */
{ STEP_END, STEP_END, STEP_END, STEP_END, STEP_END },

/* [14] JUMP_TRACKS_OPC */
{
    { start_stop,       SS_IDLE             },
    { play,             PLAY_JUMP_TRACKS    },
    { player_module_fn, PLAYER_IDLE         },
    STEP_END, STEP_END
},

/* [15] ENTER_SERVICE_MODE_OPC */
{
    { play,             PLAY_IDLE           },
    { start_stop,       SS_IDLE             },
    { player_module_fn, SET_SERVICE_MODE    },
    STEP_END, STEP_END
}

};

/* Total number of rows = MAX_LEGAL_SERVICE_ID rows for service commands.
 * Service commands (process_id 16..28) are dispatched directly to service(). */

/* =========================================================================
 * player_module_fn — handles player-level commands
 * ====================================================================== */

static byte player_module_fn(byte cmd)
{
    if (cmd == PLAYER_IDLE) return PROCESS_READY;

    if (cmd == SET_SERVICE_MODE) {
        if (servo_to_service()) {
            service_mode = 1;
            return PROCESS_READY;
        }
        player_error = ILLEGAL_COMMAND;
        return CD_ERROR_STATE;
    }

    /* PLAYER_HANDLE_ERROR */
    switch (player_error) {
    case ILLEGAL_COMMAND:
    case ILLEGAL_PARAMETER:
    case SUBCODE_NOT_FOUND:
    case JUMP_ERROR:
        /* Non-fatal: no need to stop the disc */
        return PROCESS_READY;
    default:
        break;
    }

    return READY;  /* continue the error-handling sequence */
}

/* =========================================================================
 * call_function — execute the current process step
 * ====================================================================== */

static byte call_function(void)
{
    if (process_id > MAX_LEGAL_NORMAL_ID) {
        /* Service-mode command: delegate entirely to service() */
        return service((byte)(process_id - (MAX_LEGAL_NORMAL_ID + 1)));
    }

    if (function_id >= MAX_STEPS) {
        /* Walked off the end of the table — sequence is complete */
        return PROCESS_READY;
    }

    const process_step_t *step = &processes[process_id][function_id];

    if (step->fn == NULL) {
        /* Null sentinel reached before MAX_STEPS — sequence done */
        return PROCESS_READY;
    }

    return step->fn(step->param);
}

/* =========================================================================
 * player_init
 * ====================================================================== */

void player_init(void)
{
    timer_init();
    driver_init();
    reset_dsic2_cd6();
    servo_init();
    cd6_init();

    memset(&player_interface, 0, sizeof(player_interface));
    player_interface.a_command = IDLE_OPC;
    player_interface.p_status  = READY;

    process_id   = IDLE_OPC;
    function_id  = 0;
    player_error = NO_ERROR;
    service_mode = 0;

    init_for_new_disc();
}

/* =========================================================================
 * player — main tick, called once per main-loop iteration
 * ====================================================================== */

void player(void)
{
    /* ------------------------------------------------------------------
     * 1. Accept a new command from the interface if we are idle.
     * ---------------------------------------------------------------- */
    if (player_interface.a_command != IDLE_OPC) {
        process_id = (byte)(player_interface.a_command + 1u);
        player_interface.a_command = IDLE_OPC;
        player_interface.p_status  = BUSY;
        function_id              = 0;

        /* Reset sub-module busy flags */
        play_monitor             = TRUE;
        play_command_busy        = FALSE;
        start_stop_command_busy  = FALSE;
        service_command_busy     = FALSE;

        /* Validate opcode against current mode */
        if (process_id > MAX_LEGAL_NORMAL_ID) {
            /* Service-mode opcode */
            if (process_id > MAX_LEGAL_SERVICE_ID) {
                /* Completely out of range */
                player_interface.p_status = CD_ERROR_STATE;
                player_interface.param1   = ILLEGAL_COMMAND;
                process_id = IDLE_OPC;
            } else if (!service_mode) {
                /* Service command issued while in normal mode */
                player_error = ILLEGAL_COMMAND;
                process_id   = ERROR_HANDLING_ID;
            }
        } else {
            /* Normal-mode opcode */
            if (service_mode) {
                /* Normal command issued while in service mode */
                player_interface.p_status = CD_ERROR_STATE;
                player_interface.param1   = ILLEGAL_COMMAND;
                process_id = IDLE_OPC;
            }
        }
    }

    /* ------------------------------------------------------------------
     * 2. Advance the active process sequence.
     * ---------------------------------------------------------------- */
    if (process_id != IDLE_OPC) {
        switch (call_function()) {

        case PROCESS_READY:
            /* Sequence complete */
            if (process_id == (byte)(ENTER_NORMAL_MODE_OPC + 1u))
                service_mode = 0;

            if (process_id != ERROR_HANDLING_ID) {
                player_interface.p_status = READY;
            } else {
                player_interface.p_status = CD_ERROR_STATE;
                player_interface.param1   = player_error;
            }
            player_error = NO_ERROR;
            process_id   = IDLE_OPC;
            break;

        case READY:
            /* READ_SUBCODE (process_id 10) completes on a single READY */
            if (process_id == (byte)(READ_SUBCODE_OPC + 1u)) {
                player_interface.p_status = READY;
                player_error = NO_ERROR;
                process_id   = IDLE_OPC;
            } else {
                function_id++;  /* advance to next step in sequence */
            }
            break;

        case CD_ERROR_STATE:
            if (service_mode) {
                player_interface.p_status = CD_ERROR_STATE;
                player_interface.param1   = player_error;
                player_error = NO_ERROR;
                process_id   = IDLE_OPC;
            } else {
                /* Trigger error-handling sequence */
                process_id  = ERROR_HANDLING_ID;
                function_id = 0;
            }
            break;

        case BUSY:
        default:
            /* Still working — do nothing, retry next tick */
            break;
        }
    }

    /* ------------------------------------------------------------------
     * 3. Run background sub-modules.
     * ---------------------------------------------------------------- */
    if (!service_mode) {
        execute_start_stop_functions();
        execute_play_functions();
        /* NOTE: servo() must run directly before subcode_module() so that
         * subcode data is fresh for execute_play_functions() next tick. */
        servo();
        subcode_module();
        shock_recover();
    } else {
        execute_service_functions();
    }
}

/* =========================================================================
 * Sub-module stubs
 *
 * These provide link-time definitions for the functions referenced above.
 * Replace each stub with a real implementation in its own .c file as the
 * hardware bring-up progresses.
 * ====================================================================== */

/* =========================================================================
 * Real sub-module implementations — provided by their own .c files.
 * The static wrappers above are replaced by these external declarations.
 * ====================================================================== */

/* start_stop and play — declared in their respective files */
static byte start_stop(byte cmd) { extern uint8_t start_stop(uint8_t); return start_stop(cmd); }
static byte play(byte cmd) { extern uint8_t play(uint8_t); return play(cmd); }
static byte service(byte cmd)    { extern uint8_t service(uint8_t);   return service(cmd); }

/* Background per-tick executors */
static void execute_start_stop_functions(void)
{
    extern void execute_start_stop_functions(void);
    execute_start_stop_functions();
}
static void execute_play_functions(void)
{
    extern void execute_play_functions(void);
    execute_play_functions();
}
static void execute_service_functions(void)
{
    extern void execute_service_functions(void);
    execute_service_functions();
}

/* Servo tick */
static void servo(void)
{
    extern void servo(void);
    servo();
}

/* Subcode tick */
static void subcode_module(void)
{
    extern void subcode_module(void);
    subcode_module();
}

/* Shock recovery tick */
static void shock_recover(void)
{
    extern void shock_recover(void);
    shock_recover();
}

/* Servo helpers */
static uint8_t servo_to_service(void)
{
    extern uint8_t servo_to_service(void);
    return (int)servo_to_service();
}

static void init_for_new_disc(void)
{
    extern void init_for_new_disc(void);
    init_for_new_disc();
}

static void servo_init(void)
{
    extern void servo_init(void);
    servo_init();
}

static void cd6_init(void)
{
    extern void cd6_init(void);
    cd6_init();
}

/* play() is now implemented in core/play.c */
