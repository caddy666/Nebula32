/**
 * @file  dispatcher.c
 * @brief Dispatcher — arbitrates COMMO transmits and routes received commands.
 *
 * The Dispatcher runs once per main-loop iteration.  It:
 *   1. Checks whether any pending COMMO transmit has completed.
 *   2. Checks for new status updates to send.
 *   3. Routes newly received commands to the command handler.
 *   4. Handles duplicate / checksum-error command responses.
 *
 * Ported from the original Philips/Commodore 1993 firmware with all 8051
 * constructs removed.
 */

#include <stdint.h>

#include "defs.h"
#include "commo.h"
#include "sts_q_id.h"
#include "cmd_hndl.h"

/* =========================================================================
 * Module state
 * ====================================================================== */

/** Mode of the packet transmit that is pending or in progress. */
static uint8_t s_suspend_transmit      = NO_UPDATE;

/** Non-zero once a command has been reported to the command handler. */
static uint8_t s_report_for_free_buf   = 0;

/** Save register for a command that could not be reported immediately. */
static uint8_t s_cmd_still_to_report   = COMMO_NO_COMMAND;

/** Non-zero when the current transmit came from the status/Q/ID module. */
static uint8_t s_acknowledge_update    = 0;

/* =========================================================================
 * Request_packet_transmit
 *
 * Attempt to queue a packet for COMMO transmit.
 * Returns TRUE if accepted, FALSE if the COMMO interface was busy.
 * ====================================================================== */

static uint8_t Request_packet_transmit(uint8_t mode)
{
    switch (mode) {

    case NO_UPDATE:
        return COMMO_TRUE;

    case STATUS_UPDATE:
        if (SEND_STRING(SEND_STRING_COMPLETE,
                        Get_sts_q_id_ptr(),
                        STATUS_PACKET_LENGTH) == COMMO_TRUE) {
            s_acknowledge_update = 1;
            return COMMO_TRUE;
        }
        return COMMO_FALSE;

    case Q_READY:
        if (SEND_STRING(SEND_STRING_COMPLETE,
                        Get_sts_q_id_ptr(),
                        Q_PACKET_LENGTH) == COMMO_TRUE) {
            s_acknowledge_update = 1;
            return COMMO_TRUE;
        }
        return COMMO_FALSE;

    case ID_READY:
        if (SEND_STRING(SEND_STRING_COMPLETE,
                        Get_sts_q_id_ptr(),
                        ID_PACKET_LENGTH) == COMMO_TRUE) {
            s_acknowledge_update = 1;
            return COMMO_TRUE;
        }
        return COMMO_FALSE;

    default:
        return COMMO_FALSE;
    }
}

/* =========================================================================
 * Dispatcher — called once per main-loop iteration
 * ====================================================================== */

void Dispatcher(void)
{
    /* ------------------------------------------------------------------
     * 1. Check if a previous COMMO transmit has completed.
     * ---------------------------------------------------------------- */
    if (SEND_STRING_READY() <= COMMO_READY_WITH_ERROR) {
        /* Previous transmit is done (with or without error) */

        if (s_acknowledge_update) {
            /* Packet came from status/Q/ID module — clear the update flag */
            Clear_update();
            s_suspend_transmit   = NO_UPDATE;
            s_acknowledge_update = 0;
        } else {
            if (s_suspend_transmit != NO_UPDATE) {
                /* Retry the suspended packet transmit */
                Request_packet_transmit(s_suspend_transmit);
            }
        }
    }

    /* ------------------------------------------------------------------
     * 2. Check for a new status update to send.
     * ---------------------------------------------------------------- */
    if (s_suspend_transmit == NO_UPDATE &&
        Get_update_status() != NO_UPDATE)
    {
        s_suspend_transmit = Get_update_status();
    }

    /* ------------------------------------------------------------------
     * 3. Release the COMMO command buffer once the handler is ready.
     * ---------------------------------------------------------------- */
    if (s_report_for_free_buf && Cmd_acception_status() == COMMO_TRUE) {
        if (FREE_CMD_BUFFER() == COMMO_TRUE) {
            s_report_for_free_buf = 0;
        }
    }

    /* ------------------------------------------------------------------
     * 4. Poll for a newly received command from the COMMO interface.
     * ---------------------------------------------------------------- */
    if (s_cmd_still_to_report == COMMO_NO_COMMAND) {
        s_cmd_still_to_report = NEW_CMD_RECEIVED();
    }

    switch (s_cmd_still_to_report) {

    case COMMO_NO_COMMAND:
        break;

    case COMMO_NEW_COMMAND:
        if (New_command() == COMMO_TRUE) {
            s_cmd_still_to_report = COMMO_NO_COMMAND;
            s_report_for_free_buf = 1;
        }
        break;

    case COMMO_SAME_COMMAND:
        /* Host repeated the same command — echo status back */
        if (Get_update_status() == NO_UPDATE) {
            Store_command(GET_BUFFER(0));
            Store_update_status(STATUS_UPDATE);
            s_suspend_transmit    = STATUS_UPDATE;
            s_report_for_free_buf = 1;
            s_cmd_still_to_report = COMMO_NO_COMMAND;
        }
        break;

    case COMMO_CMD_ERROR:
        /* Checksum error — report error status to host */
        if (Get_update_status() == NO_UPDATE) {
            Store_command(GET_BUFFER(0));
            Store_error_condition(CHECKSUM_ERROR);
            Store_update_status(STATUS_UPDATE);
            s_suspend_transmit    = STATUS_UPDATE;
            s_report_for_free_buf = 1;
            s_cmd_still_to_report = COMMO_NO_COMMAND;
        }
        break;

    default:
        s_cmd_still_to_report = COMMO_NO_COMMAND;
        break;
    }
}
