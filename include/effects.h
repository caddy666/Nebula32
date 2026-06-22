#pragma once
#include <stdint.h>
#include "fft.h"

/*
 * Amiga demoscene-style visual effects for the CD32 ODE 240x240 ST7789 display.
 * Each effect renders directly to the display via display_hline(), so only
 * one 240-pixel line buffer (~480 bytes) is needed in SRAM at any time.
 *
 * Effects:
 *  0 – SPECTRUM   Frequency bars with Amiga "copper" gradient (fire colours)
 *  1 – SCOPE      Phosphor-style oscilloscope waveform on black
 *  2 – RASTER     Classic Amiga copper raster bars scrolling sinusoidally
 *  3 – COMBO      Raster bars background + spectrum overlay
 *  4 – SPACEBALLS Chunky 4×4 breakdancer (State of the Art 1992 homage)
 *  5 – JUGGLER    Procedural Eric-Graham-style juggling stick figure
 */

#define DISP_W     240
#define DISP_H     240
#define NUM_EFFECTS 6

/* Pack R,G,B (0-255 each) into byte-swapped RGB565 (RAMCTRL bswap enabled). */
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((uint16_t)(r & 0xF8U) << 8) | ((uint16_t)(g & 0xFCU) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

typedef struct {
    const uint8_t *spectrum; /* NUM_BARS current bar heights 0-255  */
    const uint8_t *peaks;    /* NUM_BARS peak-hold values 0-255     */
    const int16_t *waveform; /* FFT_SIZE raw windowed samples (Q15) */
    uint32_t       frame;    /* incrementing frame counter          */
    uint8_t        mode;     /* 0..NUM_EFFECTS-1                    */
} EffectCtx;

/* Render one complete frame (all DISP_H scanlines) of the current effect. */
void effects_render(const EffectCtx *ctx);
