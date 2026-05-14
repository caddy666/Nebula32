/**
 * @file  cmd_hndl.c
 * @brief Command handler — translates COMMO opcodes to player_interface commands.
 *
 * The command handler sits between the COMMO/Dispatcher layer and the player
 * module.  It receives a decoded opcode + parameters from the Dispatcher,
 * validates them, and writes them into player_interface.
 */

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "cmd_hndl.h"
#include "commo.h"
#include "player.h"
#include "sts_q_id.h"

/* =========================================================================
 * Internal state
 * ====================================================================== */

static uint8_t s_state          = IDLE;
static uint8_t s_pending_cmd    = NO_CMD;
static uint8_t s_pending_p1     = 0;
static uint8_t s_pending_p2     = 0;
static uint8_t s_pending_p3     = 0;
static uint8_t s_cmd_accepted   = 0;   /**< Non-zero: player accepted the cmd */
static uint8_t s_handler_ready  = 1;   /**< Non-zero: can accept a new command */

/* =========================================================================
 * Init
 * ====================================================================== */

void Init_command_handler(void)
{
    s_state         = IDLE;
    s_pending_cmd   = NO_CMD;
    s_cmd_accepted  = 0;
    s_handler_ready = 1;
}

/* =========================================================================
 * command_handler — called once per main-loop iteration
 * ====================================================================== */

void command_handler(void)
{
    /* If a command is pending and the player interface is free, dispatch it */
    if (s_pending_cmd != NO_CMD &&
        player_interface.a_command == IDLE_OPC &&
        player_interface.p_status  == READY)
    {
        player_interface.a_command = s_pending_cmd;
        player_interface.param1    = s_pending_p1;
        player_interface.param2    = s_pending_p2;
        player_interface.param3    = s_pending_p3;

        s_pending_cmd   = NO_CMD;
        s_cmd_accepted  = 1;
        s_handler_ready = 0;
    }

    /* If the player has finished (READY or ERROR), mark the handler free */
    if (!s_handler_ready) {
        if (player_interface.p_status == READY ||
            player_interface.p_status == CD_ERROR_STATE)
        {
            s_handler_ready = 1;
            s_cmd_accepted  = 0;

            /* Trigger a status update to be sent back to the host */
            Store_update_status(STATUS_UPDATE);
        }
    }
}

/* =========================================================================
 * New_command — called by the Dispatcher when COMMO_NEW_COMMAND is received
 * ====================================================================== */

uint8_t New_command(void)
{
    if (!s_handler_ready) return COMMO_FALSE;   /* busy */

    /* Copy the command and parameters from the COMMO buffer */
    s_pending_cmd = GET_BUFFER(0);
    s_pending_p1  = GET_BUFFER(1);
    s_pending_p2  = GET_BUFFER(2);
    s_pending_p3  = GET_BUFFER(3);

    s_handler_ready = 0;
    return COMMO_TRUE;
}

uint8_t Cmd_acception_status(void)
{
    return s_handler_ready ? COMMO_TRUE : COMMO_FALSE;
}

uint8_t Command_handler_ready(void)
{
    return s_handler_ready ? COMMO_TRUE : COMMO_FALSE;
}
