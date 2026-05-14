#include "vis_audio.h"
#include "fft.h"
#include <string.h>

/*
 * CD-DA sector: 588 stereo pairs × 4 bytes = 2352 bytes.
 * Left channel: bytes [0,1] of each pair → 588 int16_t samples per sector.
 * We accumulate these into ring slots of FFT_SIZE (256) samples each.
 */

#define STEREO_PAIRS  588          /* samples per sector */
#define BYTES_PER_PAIR 4

/* Ring buffer: VIS_AUDIO_RING slots of FFT_SIZE samples. */
static int16_t s_ring[VIS_AUDIO_RING][FFT_SIZE];

/* Write accumulator: partial slot being filled from sector pushes. */
static int16_t s_acc[FFT_SIZE];
static int     s_acc_pos = 0;     /* next write position in s_acc */

/* Ring read/write indices (wrap at VIS_AUDIO_RING). */
static volatile int s_wr = 0;     /* next slot to write (ISR) */
static volatile int s_rd = 0;     /* next slot to read  (main) */

void vis_audio_push_sector(const uint8_t *buf) {
    /* Walk the interleaved stereo buffer, extracting left-channel int16_t. */
    for (int i = 0; i < STEREO_PAIRS; i++) {
        /* Each stereo pair: [L_lo, L_hi, R_lo, R_hi] (little-endian) */
        int16_t left = (int16_t)((uint16_t)buf[i * BYTES_PER_PAIR]
                                | ((uint16_t)buf[i * BYTES_PER_PAIR + 1] << 8));
        s_acc[s_acc_pos++] = left;

        if (s_acc_pos >= FFT_SIZE) {
            /* Slot complete — commit to ring if not full */
            int next_wr = (s_wr + 1) % VIS_AUDIO_RING;
            if (next_wr != s_rd) {  /* ring not full */
                memcpy(s_ring[s_wr], s_acc, FFT_SIZE * sizeof(int16_t));
                s_wr = next_wr;
            }
            s_acc_pos = 0;
        }
    }
}

bool vis_audio_get_samples(int16_t *out) {
    if (s_rd == s_wr) return false;  /* no new frame */
    memcpy(out, s_ring[s_rd], FFT_SIZE * sizeof(int16_t));
    s_rd = (s_rd + 1) % VIS_AUDIO_RING;
    return true;
}
