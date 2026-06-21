#pragma once
// Stub for the pioasm-generated da_output.pio.h.
// The real file is produced at build time in CMAKE_CURRENT_BINARY_DIR.
// For host-native tests we only need the symbol definitions; the SM is
// never actually started so instruction correctness doesn't matter here.

#include "hardware/pio.h"

static const uint16_t _da_output_insns[] = { 0 };
static const pio_program_t da_output_program = {
    .instructions = _da_output_insns,
    .length       = 1,
    .origin       = -1,
};

static inline void da_output_program_init(PIO pio, uint sm, uint offset,
                                           uint base_pin, bool double_speed) {
    (void)pio; (void)sm; (void)offset; (void)base_pin; (void)double_speed;
}
