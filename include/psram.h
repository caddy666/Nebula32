#pragma once
// =============================================================================
// psram.h — Optional QSPI PSRAM support (RP2350 CS1, e.g. APS6404L 8 MB)
//
// Compile with -DBUILD_WITH_PSRAM=1 (set via CMake option BUILD_WITH_PSRAM)
// to enable.  Without the flag every function is a no-op inline that
// optimises away completely — standard Pico 2 / Pico 2 W builds are unaffected.
//
// PSRAM appears at 0x11000000 (right after the 16 MB XIP flash window).
// A simple bump allocator is provided; freed blocks are not reclaimed but
// 8 MB is large enough that wolfSSL session churn and the sector cache
// permanent allocation never exhaust it.
//
// Callers must test the return value of psram_alloc() — NULL means
// allocation failed (out of PSRAM or PSRAM not present).
// =============================================================================
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Number of sector cache slots when PSRAM is available.
// 512 × ~2364 bytes ≈ 1.15 MB — roughly 44 s of read-ahead at 2× speed.
#define SECTOR_BUFFER_COUNT_PSRAM  512u

#ifdef BUILD_WITH_PSRAM

#define PSRAM_BASE       0x11000000UL          // CS1 PSRAM base (RP2350 memory map)
#define PSRAM_SIZE_BYTES (8u * 1024u * 1024u)  // 8 MB

// Call once near the top of main(), before any psram_alloc() call.
// Configures the RP2350 QMI hardware for CS1.  Chip-specific timing
// constants are in src/psram.c — adjust for your PSRAM chip.
void  psram_fw_init(void);

// Bump allocator over the PSRAM region.
// Returns NULL on exhaustion.  Returned pointer is 8-byte aligned.
// Zero-initialises the returned block.
void *psram_alloc(size_t n);

// Mark a block free.  Bump allocator — space is not reclaimed.
void  psram_free(void *p);

// Realloc: reuses the block if new size fits; otherwise allocates, copies,
// and marks old block freed.  Returns NULL on failure (original untouched).
void *psram_realloc(void *p, size_t n);

// True after psram_fw_init() succeeds.
bool  psram_available(void);

#else  // !BUILD_WITH_PSRAM — everything resolves to nothing at compile time

static inline void  psram_fw_init(void)               {}
static inline void *psram_alloc(size_t n)              { (void)n; return NULL; }
static inline void  psram_free(void *p)                { (void)p; }
static inline void *psram_realloc(void *p, size_t n)   { (void)p; (void)n; return NULL; }
static inline bool  psram_available(void)              { return false; }

#endif  // BUILD_WITH_PSRAM
