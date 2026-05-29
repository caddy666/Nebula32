#pragma once
#include <stdint.h>
#include <stdlib.h>
/* Stub for pico/rand.h — returns rand() on the host test build */
static inline uint32_t get_rand_32(void) { return (uint32_t)rand(); }
