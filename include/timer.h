/**
 * @file  timer.h
 * @brief Software timer subsystem — 8 ms tick via Pico hardware repeating timer.
 */

#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Timer IDs (index into the timers[] array)
 * ---------------------------------------------------------------------- */
typedef enum {
    TIMER_SERVO = 0,
    TIMER_MODULE,
    TIMER_COMMO,
    TIMER_KICK_BRAKE,
    TIMER_PROGRESS,
    TIMER_SIMULATION,
    TIMER_SEARCH,
    TIMER_PLAY,
    TIMER_COUNT
} timer_id_t;

/* -------------------------------------------------------------------------
 * Global timer array — decremented every 8 ms by the hardware ISR.
 * Write a count to start; reads 0 when expired.
 * ---------------------------------------------------------------------- */
extern volatile uint8_t timers[TIMER_COUNT];

/* -------------------------------------------------------------------------
 * Shared state set by the SCOR falling-edge interrupt
 * ---------------------------------------------------------------------- */
extern volatile uint8_t scor_counter;
extern volatile uint8_t scor_edge;

/* Delay byte used by the original blocking delay() */
extern volatile uint8_t delay_byte;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/** Initialise the 8 ms repeating timer and the SCOR GPIO interrupt. */
void timer_init(void);

/** Blocking delay — decrements delay_byte * ~500 µs per count. */
void delay(void);

/** Blocking delay in units of 500 µs. */
void delay_us_500x(uint8_t n);
