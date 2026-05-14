#pragma once
#include <stdint.h>

/*
 * Fixed-point 256-point radix-2 DIT FFT.
 *
 * All arithmetic is Q15 (int16_t, values in -32768..32767).
 * One right-shift per butterfly stage prevents overflow; absolute amplitude
 * is not preserved but relative magnitude across bins is accurate.
 *
 * Memory: ~2.5 KB for twiddle factors, Hann window, and working buffers.
 */

#define FFT_SIZE   256
#define FFT_BINS   (FFT_SIZE / 2)  /* 128 unique positive-frequency bins */
#define NUM_BARS   32              /* display bars (log-spaced) */
#define PEAK_DECAY 2              /* units subtracted per frame from peaks */

/*
 * Call once at startup (uses software float, initialisation only).
 * Computes twiddle factors and Hann window table.
 */
void fft_init(void);

/*
 * Process one frame: apply Hann window, run FFT, compute magnitudes,
 * map to NUM_BARS log-spaced bars, apply peak hold.
 *
 * samples   – int16_t[FFT_SIZE], Q15 signed audio samples
 * spectrum  – uint8_t[NUM_BARS] out: current bar heights 0..255
 * peaks     – uint8_t[NUM_BARS] in/out: peak-hold values (caller keeps between calls)
 * waveform  – int16_t[FFT_SIZE] out: copy of windowed samples for oscilloscope
 */
void fft_process(const int16_t *samples,
                 uint8_t *spectrum, uint8_t *peaks,
                 int16_t *waveform);
