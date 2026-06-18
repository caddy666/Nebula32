/**
 * @file  player.h
 * @brief Top-level player module API.
 */

#pragma once
#include "defs.h"

/** Shared interface structure between host comms and the player. */
extern interface_field_t player_interface;

/** Current player error code. */
extern byte player_error;

/** Current process index (0xFF = idle). */
extern byte process_id;

/** Current function step within the active process. */
extern byte function_id;

/** Initialise all sub-modules (timers, drivers, servo, CD6). */
void player_init(void);

/**
 * @brief  Main player tick — call once per main-loop iteration.
 *
 * Dispatches the current process/function step, then runs the servo,
 * subcode, and shock-recovery modules when not in service mode.
 */
void player(void);
