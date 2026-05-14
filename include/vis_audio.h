#pragma once
#include <stdint.h>
#include <stdbool.h>
/*
 * Visualiser audio snoop.
 *
 * Snoops the DA DMA ping-pong buffers: each time a sector finishes streaming,
 * the left-channel PCM samples are extracted and pushed into a ring buffer.
 * The main loop calls vis_audio_get_samples() to pull FFT_SIZE samples for
 * frequency analysis without allocating any extra DMA channel or IRQ.
 *
 * Only meaningful when playing CD-DA audio sectors.  For data sectors the
 * caller should stop calling vis_audio_push_sector() (or accept that the
 * "visualiser" will show noise — which it will blank in the main loop check).
 */

/* Number of ring-buffer slots; each slot holds FFT_SIZE left-channel samples. */
#define VIS_AUDIO_RING 4

/*
 * Push one sector's worth of audio into the ring buffer.
 * Call from the DMA IRQ handler after a CD-DA sector finishes transferring.
 * buf must be SECTOR_RAW_SIZE bytes of raw interleaved stereo 16-bit PCM.
 * Safe to call from ISR (write-pointer only, single writer).
 */
void vis_audio_push_sector(const uint8_t *buf);

/*
 * Pull FFT_SIZE int16_t left-channel samples into 'out'.
 * Returns true if a new frame was available; false if no new data since the
 * last call (caller should skip FFT and keep showing the previous spectrum).
 */
bool vis_audio_get_samples(int16_t *out);
