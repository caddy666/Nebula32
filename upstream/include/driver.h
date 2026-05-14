/**
 * @file  driver.h
 * @brief Hardware driver API: CXD2500BQ, DSIC2, subcode reader, I/O sense.
 */

#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------------- */

/** Configure all GPIO pins and leave them in their safe default state. */
void driver_init(void);

/** Reset the DSIC2 and CXD2500 ICs with the required power-on timing. */
void reset_dsic2_cd6(void);

/* -------------------------------------------------------------------------
 * CXD2500BQ (referred to as CD6 in the original source)
 * ---------------------------------------------------------------------- */

/** Send a raw 8-bit command word to the CXD2500BQ. */
void cxd2500_wr(uint8_t data);

/** Send a combined audio-control + opcode 0x0A word. */
void audio_cxd2500(uint8_t data);

/**
 * @brief Write a high-level motor/audio mode to the CXD2500BQ.
 * @param mode  One of the MOT_* / DAC_* / MUTE / FULL_SCALE / ATTENUATE
 *              constants from serv_def.h.
 */
void cd6_wr(uint8_t mode);

/* -------------------------------------------------------------------------
 * DSIC2 servo controller
 * ---------------------------------------------------------------------- */

/** Shift 8 bits MSB-first into the DSIC2 and latch. */
void wr_dsic2(uint8_t data);

/** Read 8 bits MSB-first from the DSIC2. */
uint8_t rd_dsic2(void);

/* -------------------------------------------------------------------------
 * Subcode Q-channel reader
 * ---------------------------------------------------------------------- */

/** 10-byte Q-channel subcode buffer populated by cd6_read_subcode(). */
extern uint8_t Q_buffer[10];

/**
 * @brief  Read one Q-channel subcode frame from the CXD2500 serial output.
 * @return 1 if a valid frame was captured, 0 if SCOR edge was not present
 *         or QDA was not asserted.
 */
int cd6_read_subcode(void);

/* -------------------------------------------------------------------------
 * Analogue / sense inputs
 * ---------------------------------------------------------------------- */

/** @return Non-zero if HF (high-frequency) signal indicates disc present. */
int hf_present(void);

/** @return Non-zero if the drive door is closed. */
int door_closed(void);

/* -------------------------------------------------------------------------
 * SCOR counter helpers (called from servo / play modules)
 * ---------------------------------------------------------------------- */

/** Set up the SCOR down-counter with an initial count. */
void init_scor_counter(uint8_t count);

/** @return Non-zero if the SCOR counter has reached zero. */
int zero_scor_counter(void);

/** Increment the SCOR counter (used during seek/jump). */
void increment_scor_counter(void);

/** Enable the SCOR falling-edge interrupt. */
void enable_scor_counter(void);

/* -------------------------------------------------------------------------
 * Status polling (replaces 8051 status_cd6 bit reads)
 * ---------------------------------------------------------------------- */

/**
 * @brief  Read a CXD2500 status line by type.
 * @param  status_type  One of SUBCODE_READY / MOT_STRT_1 / MOT_STOP etc.
 * @return 1 if the status line is asserted, 0 otherwise.
 */
int status_cd6(uint8_t status_type);

/* -------------------------------------------------------------------------
 * Level-meter helpers
 * ---------------------------------------------------------------------- */
extern uint8_t peak_level_low;
extern uint8_t peak_level_high;
extern uint8_t audio_cntrl;

/** Set the peak-level-meter mode in the audio shadow register. */
void set_level_meter_mode(uint8_t mode);

/* -------------------------------------------------------------------------
 * Brake / motor area helpers
 * ---------------------------------------------------------------------- */
extern uint8_t hex_abs_min;
extern uint8_t simulation_timer;

/** @return Brake table value for current disc position and speed. */
uint8_t get_area(void);
