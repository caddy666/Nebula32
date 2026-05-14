/* Minimal stub replacing pico/time.h for host-native unit tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct repeating_timer {
    int64_t  delay_us;
    void    *user_data;
} repeating_timer_t;

/* repeating_timer and repeating_timer_t are the same type via typedef. */
#define repeating_timer repeating_timer  /* suppress duplicate-tag warning */

typedef bool (*repeating_timer_callback_t)(repeating_timer_t *t);

static inline bool add_repeating_timer_us(
    int64_t delay_us,
    repeating_timer_callback_t callback,
    void *user_data,
    repeating_timer_t *out)
{
    (void)delay_us; (void)callback; (void)user_data; (void)out;
    return true;
}
