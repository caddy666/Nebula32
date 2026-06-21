#pragma once
/* Stub for pioasm-generated subcode_encoder.pio.h.
 * The real file is produced at cmake build time.
 * For host-native tests we only need the program symbol; the SM is never
 * started so instruction correctness is not verified here. */

#include "hardware/pio.h"

static const uint16_t _sub_enc_insns[] = { 0 };
static const pio_program_t subcode_encoder_program = {
    .instructions = _sub_enc_insns,
    .length       = 1,
    .origin       = -1,
};

/* No-op init — GPIO configuration is tested via subcode_pulse_sector_clocks(). */
static inline void subcode_encoder_program_init(PIO pio, uint sm,
                                                 uint offset,
                                                 uint subcode_pin,
                                                 uint32_t target_bitrate_hz) {
    (void)pio; (void)sm; (void)offset; (void)subcode_pin;
    (void)target_bitrate_hz;
}
