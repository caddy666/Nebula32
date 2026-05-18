// =============================================================================
// test_fft.cpp — 256-point Q15 radix-2 FFT and spectrum analyser tests
//
// Intent: verify that src/fft.c produces numerically correct output and that
// the peak-hold/decay pipeline behaves as specified.  All tests run on the
// host without audio hardware; sample buffers are synthesised directly.
//
// Key invariants under test:
//   - fft_init() is safe to call multiple times (idempotent).
//   - Zero input: all NUM_BARS spectrum bins must be 0x00 after fft_run().
//   - Non-zero input: at least one spectrum bin must be non-zero.
//   - Spectrum values are bounded to [0, 255] (uint8_t range).
//   - Peak values are >= the corresponding spectrum bin on the same frame.
//   - Peaks decay toward zero across successive frames with no new input.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "fft.h"
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static bool all_zero(const uint8_t *arr, int n)
{
    for (int i = 0; i < n; i++) {
        if (arr[i] != 0) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Test group
 * ---------------------------------------------------------------------- */
TEST_GROUP(Fft)
{
    void setup() {
        fft_init();
    }
};

/* fft_init must not crash */
TEST(Fft, InitDoesNotCrash)
{
    fft_init();   /* second call is also fine (idempotent-style) */
}

/* Zero input: all spectrum bins must be zero (or near-zero → we check 0) */
TEST(Fft, ZeroInputProducesZeroSpectrum)
{
    int16_t samples[FFT_SIZE] = {0};
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    fft_process(samples, spectrum, peaks, waveform);

    CHECK_TRUE(all_zero(spectrum, NUM_BARS));
}

/* Spectrum values must always fit in [0, 255] */
TEST(Fft, SpectrumValuesInRange)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    /* Fill with maximum amplitude sawtooth to stress-test clipping. */
    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = (int16_t)(i * 256 - 32768);
    }

    fft_process(samples, spectrum, peaks, waveform);

    for (int i = 0; i < NUM_BARS; i++) {
        CHECK_TRUE(spectrum[i] <= 255);
    }
}

/* Peak hold: peaks[b] >= spectrum[b] after processing with non-zero signal. */
TEST(Fft, PeakHoldGteSpectrum)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = (int16_t)32767;  /* DC signal */
    }

    fft_process(samples, spectrum, peaks, waveform);

    for (int i = 0; i < NUM_BARS; i++) {
        CHECK_TRUE(peaks[i] >= spectrum[i]);
    }
}

/* Peak decay: peaks strictly decrease when spectrum is zero for multiple frames. */
TEST(Fft, PeakDecay)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    /* First, build up peaks with a strong signal. */
    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = (int16_t)32767;
    }
    fft_process(samples, spectrum, peaks, waveform);

    /* Check at least one bar has a non-zero peak to make the test meaningful. */
    bool any_nonzero = false;
    for (int i = 0; i < NUM_BARS; i++) {
        if (peaks[i] > 0) { any_nonzero = true; break; }
    }
    CHECK_TRUE(any_nonzero);

    /* Record peaks before decay. */
    uint8_t peaks_before[NUM_BARS];
    memcpy(peaks_before, peaks, NUM_BARS);

    /* Now process zero signal for several frames to trigger decay. */
    int16_t zeros[FFT_SIZE] = {0};
    int decay_frames = 20;   /* Enough frames for PEAK_DECAY to reduce values */
    for (int f = 0; f < decay_frames; f++) {
        fft_process(zeros, spectrum, peaks, waveform);
    }

    /* At least some previously-nonzero peaks must have decayed. */
    bool some_decayed = false;
    for (int i = 0; i < NUM_BARS; i++) {
        if (peaks_before[i] > 0 && peaks[i] < peaks_before[i]) {
            some_decayed = true;
            break;
        }
    }
    CHECK_TRUE(some_decayed);
}

/* Multiple frames without crash */
TEST(Fft, MultipleFramesNoCrash)
{
    int16_t samples[FFT_SIZE] = {0};
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    for (int f = 0; f < 50; f++) {
        /* Vary the signal each frame */
        for (int i = 0; i < FFT_SIZE; i++) {
            samples[i] = (int16_t)(f * i * 17);
        }
        fft_process(samples, spectrum, peaks, waveform);
    }
    /* Reaching here without crashing is the test. */
    CHECK_TRUE(true);
}

/* DC signal (all samples = INT16_MAX) should put significant energy in bar 0. */
TEST(Fft, DcSignalEnergeyInLowBars)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = 32767;
    }

    fft_process(samples, spectrum, peaks, waveform);

    /* The lowest-frequency bar (index 0) should be non-zero for a DC input. */
    CHECK_TRUE(spectrum[0] > 0);
}

/* Gap 20: non-zero input must populate the waveform output buffer.
 * fft_process() writes the (possibly windowed) samples into waveform[] for
 * the oscilloscope display effect.  At least one element must be non-zero
 * for a full-amplitude input. */
TEST(Fft, NonZeroInput_WaveformPopulated)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = 16000;
    }
    fft_process(samples, spectrum, peaks, waveform);

    bool any_nonzero = false;
    for (int i = 0; i < FFT_SIZE; i++) {
        if (waveform[i] != 0) { any_nonzero = true; break; }
    }
    CHECK_TRUE(any_nonzero);
}

/* Gap 21: one zero-signal frame must reduce each peak by exactly PEAK_DECAY.
 * PEAK_DECAY=2, so if peaks_before[i] > 2 then peaks_after[i] == peaks_before[i]-2.
 * This pins the decay rate constant so a change in fft.c is caught immediately. */
TEST(Fft, PeakDecayOneFrame_DecreasesByPeakDecay)
{
    int16_t samples[FFT_SIZE];
    uint8_t spectrum[NUM_BARS] = {0};
    uint8_t peaks[NUM_BARS]    = {0};
    int16_t waveform[FFT_SIZE] = {0};

    /* Drive peaks up with a strong DC signal. */
    for (int i = 0; i < FFT_SIZE; i++) {
        samples[i] = 32767;
    }
    fft_process(samples, spectrum, peaks, waveform);

    uint8_t peaks_before[NUM_BARS];
    memcpy(peaks_before, peaks, NUM_BARS);

    /* Single zero-signal frame. */
    int16_t zeros[FFT_SIZE] = {0};
    fft_process(zeros, spectrum, peaks, waveform);

    /* Every bar with peak_before > PEAK_DECAY must have decayed by exactly PEAK_DECAY. */
    bool any_checked = false;
    for (int i = 0; i < NUM_BARS; i++) {
        if (peaks_before[i] > PEAK_DECAY) {
            LONGS_EQUAL((long)(peaks_before[i] - PEAK_DECAY), (long)peaks[i]);
            any_checked = true;
        }
    }
    CHECK_TRUE(any_checked);
}
