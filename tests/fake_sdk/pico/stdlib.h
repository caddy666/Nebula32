/* Minimal stub replacing pico/stdlib.h for host-native unit tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

typedef unsigned int uint;

static inline void stdio_init_all(void) {}
static inline void sleep_us(uint32_t us) { (void)us; }
static inline void sleep_ms(uint32_t ms) { (void)ms; }
