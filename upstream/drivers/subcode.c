/**
 * @file  subcode.c
 * @brief CD6 (CXD2500BQ) initialisation and subcode Q-channel decoder.
 *
 * Manages the subcode_reading flag, calls the hardware cd6_read_subcode()
 * driver, and converts BCD time fields to hex in-place.
 *
 * Ported from the original Philips/Commodore 8051 firmware (1992-1993).
 */

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "serv_def.h"
#include "dsic2.h"
#include "driver.h"

/* Subcode Q-channel structure overlaid on Q_buffer[10] */
/* Indices match the original firmware's struct subcode_frame */
#define QB_CONAD   0
#define QB_TNO     1
#define QB_INDEX   2
#define QB_RMIN    3
#define QB_RSEC    4
#define QB_RFRM    5
#define QB_ZERO    6
#define QB_AMIN    7
#define QB_ASEC    8
#define QB_AFRM    9

/* =========================================================================
 * Module state
 * ====================================================================== */

static uint8_t subcode_reading     = 0;
static uint8_t new_subcode_request = 0;

uint8_t i_can_read_subcode = 0;  /**< Set when a valid frame is decoded */

/* cd_disc: 1 = pressed CD, 0 = CDR */
static uint8_t cd_disc = 1;

/* =========================================================================
 * BCD helpers (local wrappers over utils/maths.c implementations)
 * ====================================================================== */

static uint8_t bcd_to_hex(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10u) + (bcd & 0x0Fu));
}

static void bcd_to_hex_time_buf(uint8_t *buf)
{
    buf[0] = bcd_to_hex(buf[0]);
    buf[1] = bcd_to_hex(buf[1]);
    buf[2] = bcd_to_hex(buf[2]);
}

/* =========================================================================
 * Public API
 * ====================================================================== */

/**
 * @brief  Initialise the CXD2500BQ to default N=1 settings.
 *
 * Sends the startup register sequence from the original firmware.
 * Call once during player_init().
 */
void cd6_init(void)
{
    cd6_wr(SPEED_CONTROL_N1);
    cd6_wr(DAC_OUTPUT_MODE);
    cd6_wr(MOT_OUTPUT_MODE);
    /* EBU_OUTPUT_MODE is a no-op on this hardware revision */
    cd6_wr(MOT_GAIN_12CM_N1);
}

void start_subcode_reading(void)
{
    subcode_reading     = 1;
    new_subcode_request = 1;
    /* Clear any pending SCOR edge from before we started listening */
    extern volatile uint8_t scor_edge;
    scor_edge = 0;
}

void stop_subcode_reading(void)
{
    subcode_reading = 0;
}

uint8_t give_peak_level_low(void)  { return peak_level_low;  }
uint8_t give_peak_level_high(void) { return peak_level_high; }

uint8_t is_cd_disc(void) { return cd_disc; }

/**
 * @brief  Test whether the current Q_buffer contents match the requested
 *         area / address type.
 */
uint8_t is_subcode(uint8_t mode)
{
    if (new_subcode_request) return FALSE;

    uint8_t conad_lo = Q_buffer[QB_CONAD] & 0x0Fu;
    uint8_t tno      = Q_buffer[QB_TNO];
    uint8_t rmin     = Q_buffer[QB_RMIN];

    switch (mode) {
    case ALL_SUBCODES:
        return TRUE;

    case ABS_TIME:
        if (conad_lo == 0x01) {
            if (tno != 0) return TRUE;
            /* tno==0: in lead-in; only valid for CDR, not CD */
            return (uint8_t)((rmin > 90u || cd_disc) ? FALSE : TRUE);
        }
        return FALSE;

    case CATALOG_NR:
        return (uint8_t)(conad_lo == 0x02 ? TRUE : FALSE);

    case ISRC_NR:
        return (uint8_t)(conad_lo == 0x03 ? TRUE : FALSE);

    case FIRST_LEADIN_AREA:
        if (conad_lo == 0x01 && tno == 0)
            return (uint8_t)((rmin > 90u || cd_disc) ? TRUE : FALSE);
        return FALSE;

    case LEADIN_AREA:
        /* address=1 or address=5 */
        return (uint8_t)(((Q_buffer[QB_CONAD] & 0x03u) == 0x01 && tno == 0)
                         ? TRUE : FALSE);

    case PROGRAM_AREA:
        return (uint8_t)((conad_lo == 0x01 && tno != 0 && tno != 0xAA)
                         ? TRUE : FALSE);

    case LEADOUT_AREA:
        return (uint8_t)((conad_lo == 0x01 && tno == 0xAAu) ? TRUE : FALSE);

    default:
        return FALSE;
    }
}

/**
 * @brief  Copy the current absolute disc time to *p.
 *
 * If tno != 0 the track absolute time (a_time) is used; otherwise the
 * relative time (r_time) is used — this is the lead-in convention.
 */
void move_abstime(cd_time_t *p)
{
    if (Q_buffer[QB_TNO] != 0) {
        p->min = Q_buffer[QB_AMIN];
        p->sec = Q_buffer[QB_ASEC];
        p->frm = Q_buffer[QB_AFRM];
    } else {
        p->min = Q_buffer[QB_RMIN];
        p->sec = Q_buffer[QB_RSEC];
        p->frm = Q_buffer[QB_RFRM];
    }
}

/**
 * @brief  Check for a new SCOR edge and decode the Q-channel frame.
 *
 * Must be called once per main-loop iteration.  When a new frame is
 * available it is decoded in-place inside Q_buffer and BCD values are
 * converted to hex.
 */
void subcode_module(void)
{
    if (!subcode_reading) return;

    if (cd6_read_subcode()) {
        new_subcode_request = 0;
        i_can_read_subcode  = 1;

        uint8_t conad_lo = Q_buffer[QB_CONAD] & 0x0Fu;

        if (conad_lo == 0x01) {
            /* Address mode 1 — standard time-code frame */
            if (Q_buffer[QB_TNO] != 0xAAu)
                Q_buffer[QB_TNO] = bcd_to_hex(Q_buffer[QB_TNO]);

            /* Convert relative time in-place */
            bcd_to_hex_time_buf(&Q_buffer[QB_RMIN]);

            if (Q_buffer[QB_INDEX] == 0xA0u || Q_buffer[QB_INDEX] == 0xA1u) {
                /* Special TOC entries: only convert AMIN */
                Q_buffer[QB_AMIN] = bcd_to_hex(Q_buffer[QB_AMIN]);
            } else {
                /* Normal track frame: convert absolute time */
                bcd_to_hex_time_buf(&Q_buffer[QB_AMIN]);
                if (Q_buffer[QB_INDEX] != 0xA2u)
                    Q_buffer[QB_INDEX] = bcd_to_hex(Q_buffer[QB_INDEX]);
            }

            /* Update brake-table position index */
            if (Q_buffer[QB_TNO] != 0x00u)
                hex_abs_min = Q_buffer[QB_AMIN];
            else
                hex_abs_min = 0;

        } else if (conad_lo == 0x05) {
            /* Address mode 5 — multi-session TOC pointer */
            if (Q_buffer[QB_TNO]   == 0x00u &&
                Q_buffer[QB_INDEX] == 0xB0u &&
                Q_buffer[QB_RMIN]  != 0xFFu)
            {
                bcd_to_hex_time_buf(&Q_buffer[QB_RMIN]);
            }
        }
    }
}
