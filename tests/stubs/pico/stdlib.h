#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
/* ARM memory barrier — no-op on x86 host */
#ifndef __dmb
#define __dmb() do {} while(0)
#endif
static inline void tight_loop_contents(void) {}
static inline void sleep_ms(uint32_t ms) { (void)ms; }
static inline void sleep_us(uint64_t us) { (void)us; }
