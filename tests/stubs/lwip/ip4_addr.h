#pragma once
#include <stdint.h>

typedef struct { uint32_t addr; } ip4_addr_t;

static inline const char *ip4addr_ntoa(const ip4_addr_t *a)
    { (void)a; return "0.0.0.0"; }
