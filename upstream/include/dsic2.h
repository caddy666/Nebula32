/**
 * @file  dsic2.h
 * @brief DSIC2 (Sony CXA1372) servo IC register and constant definitions.
 *
 * The DSIC2 is a focus/radial/sledge servo controller.  Commands are sent
 * to it via the 3-wire SICL/SIDA/SILD bus implemented in driver.c.
 */

#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * DSIC2 command opcodes
 * ---------------------------------------------------------------------- */
#define PRESET      0x00   /**< Preset / reset                       */
#define LASER_ON    0x01   /**< Enable laser diode                   */
#define LASER_OFF   0x00   /**< Disable laser diode                  */

/* Short-jump command (3-word packet: cmd, high, low, stat) */
#define SRCOMM3     0x40

/* Long-jump command (6-word packet: cmd, brake, speed, high, low, stat) */
#define SRCOMM5     0x50

/* Sledge command (2-word packet: cmd, power/direction) */
#define SRSLEDGE    0x60

/* -------------------------------------------------------------------------
 * DSIC2 radial status selectors (written after jump commands)
 * ---------------------------------------------------------------------- */
#define RAD_STAT3   0x02   /**< Radial status after short jump       */
#define RAD_STAT5   0x03   /**< Radial status after long jump        */

/* -------------------------------------------------------------------------
 * Sledge direction/power bytes
 * ---------------------------------------------------------------------- */
#define SLEDGE_UOUT_IN   0x80   /**< Move sledge toward lead-in (inward)  */
#define SLEDGE_UOUT_OUT  0x7F   /**< Move sledge toward lead-out          */
#define SLEDGE_UOUT_OFF  0x00   /**< Stop sledge motor                    */
#define SLEDGE_UOUT_JMP  0x40   /**< Sledge speed for jump operations     */

/* -------------------------------------------------------------------------
 * Radial servo helper commands
 * ---------------------------------------------------------------------- */
#define RAD_INITIALIZE_TIME   38   /**< ~300 ms (38 × 8 ms ticks)         */

/* -------------------------------------------------------------------------
 * Timing constants (in 8 ms ticks unless noted)
 * ---------------------------------------------------------------------- */
#define FOCUS_TIME_OUT         50   /**< ~400 ms to find focus             */
#define TIME_DOUBLE_FOCUS_CHECK 3   /**< ~24 ms double-check delay         */
#define SPEEDUP_TIME           25   /**< ~200 ms motor mode-1 acceleration */
#define NOMINAL_SPEED_TIME    100   /**< ~800 ms wait for 75% speed        */
#define MAX_RETRIES             3   /**< Servo retry count                 */
#define HALF_SLEDGE_IN_TIME    63   /**< ~500 ms max time to home sledge   */
#define SLEDGE_OUT_TIME        13   /**< ~100 ms sledge out pulse          */
#define SUBCODE_TIME_OUT       25   /**< ~200 ms subcode timeout           */
#define SUBCODE_MONITOR_TIMEOUT 25  /**< ~200 ms between subcode frames    */
#define SKATING_DELAY_CHECK     3   /**< ~24 ms before sampling off-track  */
#define SKATING_SAMPLE_TIME     3   /**< ~24 ms between off-track samples  */
#define STOP_TIME_OUT         125   /**< ~1 s spindle brake timeout        */
#define MOT_OFF_STOP_TIME      63   /**< ~500 ms motor-off stop phase      */
#define EXTRA_STOP_DELAY       13   /**< ~100 ms extra delay after stop    */
#define N2_TO_N1_BRAKE_TIME    13   /**< ~100 ms N2→N1 brake time         */
#define F_REC_IN_SLEDGE        13   /**< ~100 ms sledge-in for focus rec.  */
#define R_REC_OUT_SLEDGE        5   /**< ~40 ms sledge-out for radial rec. */
#define UPTO_N2_TIME           10   /**< ~80 ms check for N2 speed         */
#define SUBCODE_TIMEOUT_VALUE  50   /**< ~400 ms subcode timeout (strtstop)*/
#define TRACKS_INTO_LEADIN    (-588)  /**< Jump distance back into lead-in   */
#define TRACKS_OUTOF_LEADIN    100    /**< Jump distance out of lead-in      */

/* -------------------------------------------------------------------------
 * Jump distance thresholds (in grooves)
 * ---------------------------------------------------------------------- */
#define MAX              150    /**< Short-jump maximum (actuator only)    */
#define BRAKE_2_DIS_MAX 3000    /**< Long-jump short-brake threshold       */
#define BRAKE_DIS_MAX   3000    /**< Long-jump full-brake distance         */
