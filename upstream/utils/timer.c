/**
 * @file  timer.c
 * @brief Software timer subsystem for the CD32 Pico2 firmware.
 *
 * Replaces the 8051 T0/T1 hardware timers with a Pico repeating_timer
 * that fires every 8 ms and decrements a bank of 8 software counters.
 *
 * The SCOR (subcode clock) falling-edge interrupt is also set up here.
 */

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <stdint.h>

#include "timer.h"
#include "gpio_map.h"
#include <stddef.h>

/* =========================================================================
 * Software timer array — one entry per timer_id_t
 * ====================================================================== */
volatile uint8_t timers[TIMER_COUNT] = {0};

/* =========================================================================
 * SCOR interrupt state
 * ====================================================================== */
volatile uint8_t scor_counter = 0;
volatile uint8_t scor_edge    = 0;

/* =========================================================================
 * Delay byte (legacy blocking delay helper)
 * ====================================================================== */
volatile uint8_t delay_byte = 0;

/* =========================================================================
 * Private: 8 ms repeating timer ISR
 * ====================================================================== */
static struct repeating_timer s_hw_timer;

static bool timer_callback(struct repeating_timer *t)
{
    (void)t;
    for (int i = 0; i < TIMER_COUNT; i++) {
        if (timers[i]) timers[i]--;
    }
    return true;  /* keep repeating */
}

/* =========================================================================
 * Private: SCOR GPIO interrupt (falling edge)
 * ====================================================================== */
static void scor_gpio_callback(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;

    scor_edge = 1;

    if (scor_counter > 0)
        scor_counter--;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void timer_init(void)
{
    /* Start the 8 ms repeating timer (negative period = us, scheduled from
     * the END of the previous callback to avoid drift). */
    add_repeating_timer_us(-8000, timer_callback, NULL, &s_hw_timer);

    /* Set up the SCOR falling-edge interrupt.
     * The GPIO direction and pull-up are configured in driver_init(). */
    gpio_set_irq_enabled_with_callback(
        PIN_SCOR,
        GPIO_IRQ_EDGE_FALL,
        true,
        &scor_gpio_callback
    );
}

/**
 * @brief Blocking delay. Each count ≈ 500 µs.
 *
 * Matches the original firmware's delay() which polled a down-counter
 * decremented by the 8051 timer ISR.  On Pico we use sleep_us directly.
 */
void delay(void)
{
    do {
        sleep_us(500);
    } while (--delay_byte);
}

void delay_us_500x(uint8_t n)
{
    sleep_us((uint32_t)n * 500u);
}
