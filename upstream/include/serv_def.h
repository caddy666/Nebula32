/**
 * @file  serv_def.h
 * @brief Servo state-machine and CXD2500 mode definitions.
 *
 * Ported from the original Philips/Commodore firmware (1992-1993).
 */

#pragma once

/* -------------------------------------------------------------------------
 * Servo state-machine states
 * ---------------------------------------------------------------------- */
#define INIT_DSIC2               0
#define INIT_SLEDGE              1
#define CHECK_IN_SLEDGE          2
#define CHECK_OUT_SLEDGE         3
#define SERVO_IDLE               4
#define START_FOCUS              5
#define CHECK_FOCUS              6
#define DOUBLE_CHECK_FOCUS       7
#define START_TTM                8
#define TTM_SPEED_UP             9
#define CHECK_TTM               10
#define START_RADIAL            11
#define INIT_RADIAL             12
#define SERVO_MONITOR           13
#define RADIAL_RECOVER          14
#define FOCUS_RECOVER           15
#define SLEDGE_INSIDE_RECOVER   16
#define SLEDGE_OUTSIDE_RECOVER  17
#define STOP_SERVO              18
#define CHECK_STOP_SERVO        19
#define JUMP_SERVO              20
#define CHECK_JUMP              21
#define WAIT_SUBCODE            22
#define UPTO_N2                 23
#define DOWNTO_N1               24

/* -------------------------------------------------------------------------
 * CD6 (CXD2500) status pin selectors
 * ---------------------------------------------------------------------- */
#define SUBCODE_READY   0x20
#define MOT_STRT_1      0x21
#define MOT_STRT_2      0x22
#define MOT_STOP        0x23
#define PLL_LOCK        0x24
#define MOTOR_OVERFLOW  0x27

/* -------------------------------------------------------------------------
 * CD6 motor mode values (written to cd6_wr())
 * ---------------------------------------------------------------------- */
#define MOT_OFF_ACTIVE    0x18
#define MOT_BRM1_ACTIVE   0x19
#define MOT_BRM2_ACTIVE   0x1A
#define MOT_STRTM1_ACTIVE 0x1B
#define MOT_STRTM2_ACTIVE 0x1C
#define MOT_JMPM_ACTIVE   0x1D
#define MOT_JMPM1_ACTIVE  0x1E
#define MOT_PLAYM_ACTIVE  0x1F

/* -------------------------------------------------------------------------
 * Disc sizes
 * ---------------------------------------------------------------------- */
#define DISC_8CM   0
#define DISC_12CM  1

/* -------------------------------------------------------------------------
 * Jump status bit flags (returned by get_jump_status)
 * ---------------------------------------------------------------------- */
#define NO_HF_ON_TARGET         0x01
#define RADIAL_RECOVERY_DONE    0x02
#define FOCUS_RECOVERY_DONE     0x04
#define MOTOR_NOT_ON_SPEED      0x08

/* -------------------------------------------------------------------------
 * Speed selector default
 * ---------------------------------------------------------------------- */
#define N  1   /**< Default: single speed (N=1) */

/* -------------------------------------------------------------------------
 * CXD2500 register values
 * ---------------------------------------------------------------------- */
#define SPEED_CONTROL_N1    0xB3  /**< Single speed, 33 MHz crystal */
#define SPEED_CONTROL_N2    0xBB  /**< Double speed, 33 MHz crystal */
#define MOT_GAIN_8CM_N1     0x41  /**< Motor gain G=4.0  (8 cm disc, N1)  */
#define MOT_GAIN_12CM_N1    0x44  /**< Motor gain G=12.8 (12 cm disc, N1) */
#define MOT_GAIN_8CM_N2     0x43  /**< Motor gain G=8.0  (8 cm disc, N2)  */
#define MOT_GAIN_12CM_N2    0x46  /**< Motor gain G=12.8 (12 cm disc, N2) */

/* -------------------------------------------------------------------------
 * DAC / audio output modes (used with cd6_wr)
 * ---------------------------------------------------------------------- */
#define NORMAL_MODE   0x00
#define LEVEL_MODE    0x04
#define PEAK_MODE     0x08

#define DAC_OUTPUT_MODE  0x30
#define MOT_OUTPUT_MODE  0x31
#define EBU_OUTPUT_MODE  0x32
#define MUTE             0x33
#define FULL_SCALE       0x34
#define ATTENUATE        0x35
