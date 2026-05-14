/* Minimal stub replacing hardware/gpio.h for host-native unit tests.
 *
 * gpio_put is NOT inlined here — it lives in gpio_sim.c where it also
 * maintains an operation log and injects COMMO DATA bits on CLK edges.
 * All other functions are static inline and operate on the shared
 * gpio_levels / gpio_dirs arrays defined in gpio_sim.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef unsigned int uint;

#define GPIO_IN  0
#define GPIO_OUT 1
#define GPIO_IRQ_EDGE_FALL  0x4u

#define NUM_GPIOS 30

/* Defined in gpio_sim.c */
extern uint32_t gpio_levels[NUM_GPIOS];
extern uint32_t gpio_dirs[NUM_GPIOS];
void gpio_put(uint32_t pin, uint32_t v);

typedef void (*gpio_irq_callback_t)(uint gpio, uint32_t events);

static inline void gpio_init(uint32_t pin) {
    gpio_levels[pin] = 0;
    gpio_dirs[pin]   = 0;
}
static inline void gpio_set_dir(uint32_t pin, int dir) {
    gpio_dirs[pin] = (uint32_t)dir;
}
static inline void gpio_pull_up(uint32_t pin) {
    /* Simulate a pull-up: hold the line high when nothing drives it. */
    gpio_levels[pin] = 1;
}
static inline uint32_t gpio_get(uint32_t pin) {
    return gpio_levels[pin];
}
static inline void gpio_set_irq_enabled_with_callback(
    uint pin, uint32_t events, bool enabled, gpio_irq_callback_t cb)
{
    (void)pin; (void)events; (void)enabled; (void)cb;
}
