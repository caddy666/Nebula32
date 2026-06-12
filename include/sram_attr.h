// =============================================================================
// sram_attr.h — SRAM function-placement attribute, host-build compatible
// =============================================================================
// Functions on the DA DMA ISR path must execute from SRAM so the ISR never
// stalls on an XIP flash cache miss.  Firmware builds get the real
// __not_in_flash_func() macro from pico/platform.h; the host test harness
// (plain gcc/g++, no pico-sdk) compiles the same sources, so it falls back
// to a no-op there.

#pragma once

#if defined(__arm__) || defined(__thumb__)
#include "pico.h"
#else
#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) func_name
#endif
#endif
