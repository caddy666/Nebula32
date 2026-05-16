// =============================================================================
// test_vis_audio_stress.cpp — Pthreaded stress test for the vis_audio SPSC ring
// =============================================================================
//
// Simulates Core 1 (producer/ISR) and Core 0 (consumer/main loop) running
// concurrently, with nanosecond-scale timing jitter injected via nanosleep()
// to approximate a fluctuating DA clock divider causing sectors to arrive
// slightly early or late.
//
// Build: make stress_tests       (compiled with -fsanitize=thread, not UBSan)
// Run:   ./stress_tests
//
// TSan instruments every memory access and uses the acquire/release edges on
// s_wr and s_rd to build a happens-before graph.  Any access to s_ring[] that
// is not covered by that graph is reported as a data race.
//
// The test also checks functional correctness: every frame pulled by the
// consumer must contain only the known left-channel value pushed by the
// producer.  Corruption (a sample with the wrong value) indicates that the
// reader read a slot that the writer had not fully committed yet.
//
// Relevant production code: src/vis_audio.c (push_sector / get_samples)
// =============================================================================

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>

extern "C" {
#include "vis_audio.h"
#include "fft.h"   // FFT_SIZE
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

static constexpr int     PUSH_ITERS      = 4000;   // sectors pushed by producer
static constexpr int16_t LEFT_VAL        = 0x1234; // known left-channel marker
static constexpr int16_t RIGHT_VAL       = 0x5678; // right channel (must not appear)
static constexpr int     PROD_JITTER_NS  = 500;    // max producer timing jitter
static constexpr int     CONS_JITTER_NS  = 300;    // max consumer timing jitter

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static uint8_t g_sector[588 * 4];  // one raw CD-DA sector (interleaved stereo)

static std::atomic<int>  g_frames_consumed{0};
static std::atomic<int>  g_corruption_count{0};
static std::atomic<bool> g_producer_done{false};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void jitter(int max_ns)
{
    if (max_ns <= 0) return;
    struct timespec ts = { 0, (long)(rand() % (max_ns + 1)) };
    nanosleep(&ts, nullptr);
}

// ---------------------------------------------------------------------------
// Producer thread — mirrors Core 1 DMA ISR calling vis_audio_push_sector()
// ---------------------------------------------------------------------------

static void *producer_fn(void *)
{
    for (int i = 0; i < PUSH_ITERS; i++) {
        vis_audio_push_sector(g_sector);
        jitter(PROD_JITTER_NS);
    }
    g_producer_done.store(true, std::memory_order_release);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Consumer thread — mirrors Core 0 main loop calling vis_audio_get_samples()
// ---------------------------------------------------------------------------

static void *consumer_fn(void *)
{
    int16_t out[FFT_SIZE];

    for (;;) {
        if (vis_audio_get_samples(out)) {
            g_frames_consumed.fetch_add(1, std::memory_order_relaxed);

            // Every sample in the frame must equal LEFT_VAL.
            // A mismatch means a partially-written slot was visible to the
            // reader — i.e., the release/acquire pairing failed.
            for (int i = 0; i < FFT_SIZE; i++) {
                if (out[i] != LEFT_VAL) {
                    g_corruption_count.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            jitter(CONS_JITTER_NS);
        } else if (g_producer_done.load(std::memory_order_acquire)) {
            // Producer finished and ring is empty — we're done.
            break;
        } else {
            jitter(PROD_JITTER_NS);  // brief idle before next poll
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    srand(42);  // fixed seed → deterministic jitter sequence for CI

    // Build a sector with left=LEFT_VAL, right=RIGHT_VAL in every stereo pair.
    for (int i = 0; i < 588; i++) {
        g_sector[i*4+0] = (uint8_t)( LEFT_VAL        & 0xFF);
        g_sector[i*4+1] = (uint8_t)((LEFT_VAL  >> 8) & 0xFF);
        g_sector[i*4+2] = (uint8_t)( RIGHT_VAL        & 0xFF);
        g_sector[i*4+3] = (uint8_t)((RIGHT_VAL >> 8) & 0xFF);
    }

    pthread_t prod, cons;
    pthread_create(&prod, nullptr, producer_fn, nullptr);
    pthread_create(&cons, nullptr, consumer_fn, nullptr);
    pthread_join(prod, nullptr);
    pthread_join(cons, nullptr);

    int frames   = g_frames_consumed.load();
    int corrupts = g_corruption_count.load();

    printf("stress: pushed=%d sectors  consumed=%d frames  corruptions=%d\n",
           PUSH_ITERS, frames, corrupts);

    if (corrupts > 0) {
        fprintf(stderr, "FAIL: %d frame(s) contained unexpected sample values\n",
                corrupts);
        return 1;
    }

    printf("PASS\n");
    return 0;
}
