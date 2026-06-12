#include "vis_audio.h"
#include "fft.h"
#include "sram_attr.h"
#include <string.h>
#include <stdatomic.h>

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

/*
 * SPSC ring indices — _Atomic with acquire/release ordering.
 * Writer (ISR/Core 1) owns s_wr; reader (Core 0) owns s_rd.
 * Release store on s_wr: ensures slot data is visible before the index advances.
 * Acquire load on s_wr: ensures slot data is visible before the reader copies it.
 * volatile alone is insufficient — it prevents register caching but does not
 * order the memcpy stores relative to the index update on a weakly-ordered CPU.
 */
static _Atomic int s_wr = 0;     /* next slot to write (ISR)  */
static _Atomic int s_rd = 0;     /* next slot to read  (main) */

void __not_in_flash_func(vis_audio_push_sector)(const uint8_t *buf) {
    /* Walk the interleaved stereo buffer, extracting left-channel int16_t. */
    for (int i = 0; i < STEREO_PAIRS; i++) {
        /* Each stereo pair: [L_lo, L_hi, R_lo, R_hi] (little-endian) */
        int16_t left = (int16_t)((uint16_t)buf[i * BYTES_PER_PAIR]
                                | ((uint16_t)buf[i * BYTES_PER_PAIR + 1] << 8));
        s_acc[s_acc_pos++] = left;

        if (s_acc_pos >= FFT_SIZE) {
            /* Slot complete — commit to ring if not full */
            int wr      = atomic_load_explicit(&s_wr, memory_order_relaxed);
            int rd      = atomic_load_explicit(&s_rd, memory_order_acquire);
            int next_wr = (wr + 1) & (VIS_AUDIO_RING - 1);
            if (next_wr != rd) {  /* ring not full */
                memcpy(s_ring[wr], s_acc, FFT_SIZE * sizeof(int16_t));
                /* Release: data must be visible before s_wr advances */
                atomic_store_explicit(&s_wr, next_wr, memory_order_release);
            }
            s_acc_pos = 0;
        }
    }
}

bool vis_audio_get_samples(int16_t *out) {
    int rd = atomic_load_explicit(&s_rd, memory_order_relaxed);
    /* Acquire: pairs with writer's release store — slot data visible after this */
    int wr = atomic_load_explicit(&s_wr, memory_order_acquire);
    if (rd == wr) return false;  /* no new frame */
    memcpy(out, s_ring[rd], FFT_SIZE * sizeof(int16_t));
    /* Release: slot is visibly free before the writer re-uses it */
    atomic_store_explicit(&s_rd, (rd + 1) & (VIS_AUDIO_RING - 1), memory_order_release);
    return true;
}
