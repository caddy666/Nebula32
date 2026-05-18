#pragma once
/* hardware/gpio.h — host-native test stub.
 *
 * Each TU gets its own static gpio_levels[]/gpio_dirs[] so tests can inspect
 * GPIO state directly after calling inline functions (e.g. subcode_pulse_sector_clocks).
 * No shared state across TUs; no linking required.
 *
 * uint is defined in hardware/pio.h which is always included before this. */

#include <stdint.h>
#include <stdbool.h>

#ifndef NUM_GPIOS
#define NUM_GPIOS 30
#endif

static uint32_t gpio_levels[NUM_GPIOS] __attribute__((unused));
static uint32_t gpio_dirs[NUM_GPIOS]   __attribute__((unused));

#define GPIO_IN  0
#define GPIO_OUT 1
#define GPIO_FUNC_GPCK 8
#define GPIO_IRQ_EDGE_FALL 0x4u

typedef void (*gpio_irq_callback_t)(unsigned, uint32_t);

static inline void gpio_init(uint32_t pin) {
    gpio_levels[pin] = 0;
    gpio_dirs[pin]   = 0;
}
static inline void gpio_set_dir(uint32_t pin, int dir) {
    gpio_dirs[pin] = (uint32_t)dir;
}
static inline void gpio_put(uint32_t pin, uint32_t val) {
    gpio_levels[pin] = val;
}
static inline uint32_t gpio_get(uint32_t pin) {
    return gpio_levels[pin];
}
static inline void gpio_pull_up(uint32_t pin)   { gpio_levels[pin] = 1; }
static inline void gpio_set_function(uint32_t pin, int fn) {
    (void)pin; (void)fn;
}
static inline void gpio_set_irq_enabled_with_callback(
    unsigned pin, uint32_t events, bool enabled, gpio_irq_callback_t cb) {
    (void)pin; (void)events; (void)enabled; (void)cb;
}
