#include "fft.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Tables stored in SRAM (computed once at init). */
static int16_t tw_cos[FFT_SIZE / 2]; /* cos(2πk/N) in Q15 */
static int16_t tw_sin[FFT_SIZE / 2]; /* sin(2πk/N) in Q15 */
static int16_t hann[FFT_SIZE];       /* Hann window in Q15 */
static uint8_t bitrev[FFT_SIZE];     /* bit-reversed indices for N=256 */

/* Working FFT buffers (no heap, fixed SRAM). */
static int16_t fr[FFT_SIZE]; /* real part */
static int16_t fi[FFT_SIZE]; /* imag part */

/* Log-spaced bin boundaries: bin_map[b] is the first FFT bin for bar b. */
static uint8_t bin_map[NUM_BARS + 1];

void fft_init(void) {
    /* Twiddle factors */
    for (int k = 0; k < FFT_SIZE / 2; k++) {
        float angle = 2.0f * (float)M_PI * k / FFT_SIZE;
        tw_cos[k] = (int16_t)(cosf(angle) * 32767.0f);
        tw_sin[k] = (int16_t)(sinf(angle) * 32767.0f);
    }

    /* Hann window */
    for (int n = 0; n < FFT_SIZE; n++) {
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (FFT_SIZE - 1)));
        hann[n] = (int16_t)(w * 32767.0f);
    }

    /* Bit-reversal table for N=256 (8-bit reversal) */
    for (int i = 0; i < FFT_SIZE; i++) {
        uint8_t x = (uint8_t)i, rev = 0;
        for (int j = 0; j < 8; j++) { rev = (rev << 1) | (x & 1); x >>= 1; }
        bitrev[i] = rev;
    }

    /* Log-spaced bar boundaries: bins 1..FFT_BINS mapped across NUM_BARS. */
    float log_lo = logf(1.0f);
    float log_hi = logf((float)FFT_BINS);
    for (int b = 0; b <= NUM_BARS; b++) {
        float f = expf(log_lo + (log_hi - log_lo) * b / NUM_BARS);
        uint8_t bin = (uint8_t)(f + 0.5f);
        if (bin < 1) bin = 1;
        if (bin >= FFT_BINS) bin = FFT_BINS - 1;
        bin_map[b] = bin;
    }
}

/* Q15 multiply: (a * b) >> 15, result in Q15. */
static inline int16_t q15_mul(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> 15);
}

/* Fast magnitude approximation: max + 0.4*min  (error < 3.6%) */
static inline uint32_t magnitude(int16_t re, int16_t im) {
    uint32_t a = (uint32_t)abs((int)re);
    uint32_t b = (uint32_t)abs((int)im);
    if (a < b) { uint32_t t = a; a = b; b = t; }
    return a + ((b * 51) >> 7); /* 51/128 ≈ 0.398 */
}

void fft_process(const int16_t *samples,
                 uint8_t *spectrum, uint8_t *peaks,
                 int16_t *waveform) {

    /* Apply Hann window and copy into working buffers (bit-reversed order). */
    for (int i = 0; i < FFT_SIZE; i++) {
        int16_t windowed = q15_mul(samples[i], hann[i]);
        waveform[i]     = windowed;
        fr[bitrev[i]]   = windowed >> 1; /* pre-scale for first stage */
        fi[bitrev[i]]   = 0;
    }

    /* Cooley-Tukey DIT FFT — 8 stages for N=256. */
    for (int stage = 0; stage < 8; stage++) {
        int half    = 1 << stage;
        int step    = half * 2;
        int tw_step = FFT_SIZE >> (stage + 1);

        for (int group = 0; group < FFT_SIZE; group += step) {
            for (int k = 0; k < half; k++) {
                int top = group + k;
                int bot = top + half;
                int tw  = k * tw_step;

                int16_t tr = q15_mul(tw_cos[tw], fr[bot]) - q15_mul(tw_sin[tw], fi[bot]);
                int16_t ti = q15_mul(tw_cos[tw], fi[bot]) + q15_mul(tw_sin[tw], fr[bot]);

                /* Butterfly with >>1 to prevent overflow in next stage. */
                int16_t ar = fr[top] >> 1, ai = fi[top] >> 1;
                int16_t br = tr >> 1,      bi = ti >> 1;

                fr[top] = ar + br;
                fi[top] = ai + bi;
                fr[bot] = ar - br;
                fi[bot] = ai - bi;
            }
        }
    }

    /* Compute magnitudes per display bar (log-spaced). */
    for (int b = 0; b < NUM_BARS; b++) {
        uint32_t mx = 0;
        int blo = bin_map[b], bhi = bin_map[b + 1];
        if (bhi <= blo) bhi = blo + 1;
        for (int k = blo; k < bhi && k < FFT_BINS; k++) {
            uint32_t m = magnitude(fr[k], fi[k]);
            if (m > mx) mx = m;
        }
        /* After 8 stages each with >>1, max Q15 magnitude 32767 → 127. Scale ×2. */
        uint32_t val = mx << 1;
        if (val > 255) val = 255;
        spectrum[b] = (uint8_t)val;

        /* Peak hold with decay. */
        if (spectrum[b] >= peaks[b]) {
            peaks[b] = spectrum[b];
        } else if (peaks[b] > PEAK_DECAY) {
            peaks[b] -= PEAK_DECAY;
        } else {
            peaks[b] = 0;
        }
    }
}
