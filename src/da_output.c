// =============================================================================
// da_output.c — CD DA serial output via PIO + DMA (ping-pong double-buffer)
// =============================================================================
//
// Two DMA channels alternate so the PIO TX FIFO never starves:
//   ch0 streams s_buf[0] → FIFO, chains to ch1 when done
//   ch1 streams s_buf[1] → FIFO, chains to ch0 when done
//   IRQ fires on each completion; handler refills the idle buffer with the
//   next sector from the sector cache before the active buffer empties.
//
// Latency budget at 1× speed:
//   One sector = 588 stereo pairs × 48 bits = 28,224 bit-clocks (24-bit I2S)
//   At BCLK 2.1168 MHz → 13.33 ms per sector
//   PIO TX FIFO holds 8 × 32-bit words = 256 bits → ~121 µs cushion
//   ISR + sector expand (588 pairs → 1176 words) at 135 MHz << 10 µs  ✓
// =============================================================================

#include "da_output.h"
#include "vis_audio.h"
#include "subcode.h"
#include "disc_image.h"
#include "logger.h"

#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include <string.h>

#include "pico/assert.h"
#include "da_output.pio.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// 24-bit I2S: two 32-bit words per stereo pair (one for L, one for R)
// 588 stereo pairs × 2 words = 1176 words per sector
#define SECTOR_DMA_WORDS  (588 * 2)

// Subcode encoder state machine (PIO0 SM1) — see PIO resource table in CLAUDE.md
#define SUBCODE_PIO  pio0
#define SUBCODE_SM   1

// End-of-disc behaviour.
// Define EOD_OPTION_B to stream silence until Akiko sends STOP_OPC (more compatible
// with titles that poll Q-channel to detect lead-out before issuing STOP).
// Comment it out to revert to Option A (immediate DMA abort when last sector delivered).
#define EOD_OPTION_B

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static uint   s_offset        = 0;
static bool   s_initialised   = false;
static bool   s_double_speed  = false;
static bool   s_playing       = false;
static bool   s_paused        = false;

static int    s_dma_ch        = -1;   // channel 0: transfers s_buf[0]
static int    s_dma_ch2       = -1;   // channel 1: transfers s_buf[1]

// Ping-pong DMA buffers in 24-bit I2S word format (one uint32_t per channel).
// Each word: [31:16] = 16-bit PCM sample, [15:0] = zero padding.
// The raw sector bytes are staged through s_raw before being expanded here.
static uint32_t       s_buf[2][SECTOR_DMA_WORDS];

// Pre-zeroed silence sector in SRAM (BSS — NOT const, which would land in flash
// and reintroduce the XIP hazard for the DMA read).  At end-of-disc (Option B)
// the DMA ISR points the finished channel here instead of calling flash-resident
// memset() on a live buffer: keeps the ISR completely free of flash access
// (safe even mid flash-erase) and drops a per-sector 2352-byte clear.
static uint32_t       s_silence_buf[SECTOR_DMA_WORDS];

static uint8_t        s_raw[SECTOR_RAW_SIZE]; // scratch for sector_cache_get
static uint32_t       s_buf_lba[2];             // LBA currently loaded in each buffer
static sector_cache_t *s_cache      = NULL;
static uint32_t        s_next_lba   = 0;
static bool            s_audio_mode = false; // true = CD-DA; enables vis_audio snoop
static bool            s_drq_pending = false; // set by ISR via __atomic_store RELEASE
static bool            s_eod_reached = false; // set by ISR at end-of-disc (Option B); jukebox edge
static uint32_t        s_clkdiv_fixed = 32U * 256U; // 16.8 fixed-point: nominal 32×256 at 1×

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
static void __isr _dma_irq_handler(void);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float _clkdiv(bool dbl) {
    // sys_clk = 135,475,200 Hz
    // clkdiv 32 → SM clock = 4,234,225 Hz → BCLK = 2,117,112 Hz (1× CD) ✓
    // clkdiv 16 → SM clock = 8,468,450 Hz → BCLK = 4,234,225 Hz (2× CD)
    // 96 SM cycles per stereo pair → LRCLK = 4,234,225/96 ≈ 44,107 Hz ✓
    return dbl ? 16.0F : 32.0F;
}

// expand_to_i2s24 — convert 2352-byte raw sector to 1176 uint32_t I2S words.
// Input:  588 stereo pairs as little-endian int16_t pairs [L_lo,L_hi,R_lo,R_hi].
// Output: one uint32_t per channel: [31:16]=sample, [15:0]=0 (zero-padded slot).
// The PIO's autopull=24 outputs bits [31:8] (16 data + 8 zero padding) and
// silently discards the remaining [7:0] of each 32-bit word.
static void __not_in_flash_func(expand_to_i2s24)(const uint8_t *raw, uint32_t *out) {
    for (int i = 0; i < 588; i++) {
        uint16_t l = (uint16_t)raw[i * 4 + 0] | ((uint16_t)raw[i * 4 + 1] << 8);
        uint16_t r = (uint16_t)raw[i * 4 + 2] | ((uint16_t)raw[i * 4 + 3] << 8);
        out[i * 2 + 0] = (uint32_t)l << 16;  // left:  sample in [31:16]
        out[i * 2 + 1] = (uint32_t)r << 16;  // right: sample in [31:16]
    }
}

static void _configure_dma_channel(int ch, int chain_to_ch, uint32_t *buf) {
    dma_channel_config cfg = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(DA_PIO, DA_SM, true));
    // No bswap: expand_to_i2s24() builds uint32_t words with the sample
    // already in bits [31:16].  The PIO shifts MSB first → sample MSB first. ✓
    channel_config_set_chain_to(&cfg, (uint)chain_to_ch);
    dma_channel_configure(ch, &cfg,
                          (volatile void *)&DA_PIO->txf[DA_SM], // write: PIO TX FIFO
                          buf,                                   // read:  I2S word buffer
                          SECTOR_DMA_WORDS,
                          false);  // don't start yet
}

// ---------------------------------------------------------------------------
// _push_subcode — build and queue Q-channel data for 'lba' into subcode PIO
// ---------------------------------------------------------------------------
// Called from the DMA ISR once per sector: builds the 12-byte Q-channel block
// for the sector that just started streaming and pushes it into the subcode
// encoder PIO FIFO.  Skips silently if FIFO full (previous sector's subcode
// stays displayed — briefly wrong but preferable to blocking the ISR).
static void __not_in_flash_func(_push_subcode)(uint32_t lba) {
    if (!s_cache || !s_cache->disc) return;
    const track_t *trk = disc_find_track(s_cache->disc, lba);
    if (!trk) return;
    bool is_data = (trk->type != TRACK_TYPE_AUDIO);
    uint8_t qbuf[QCHANNEL_SIZE];
    subcode_build_q_position(trk->number, 1, is_data, trk->start_lba, lba, qbuf);
    subcode_pulse_sector_clocks();
    subcode_push_to_pio(SUBCODE_PIO, SUBCODE_SM, qbuf);
}

// ---------------------------------------------------------------------------
// da_output_init
// ---------------------------------------------------------------------------
void da_output_init(bool double_speed) {
    s_double_speed = double_speed;

    s_offset = pio_add_program(DA_PIO, &da_output_program);
    da_output_program_init(DA_PIO, DA_SM, s_offset,
                           DA_DATA_BASE_PIN, double_speed);

    s_dma_ch  = dma_claim_unused_channel(true);
    s_dma_ch2 = dma_claim_unused_channel(true);
    hard_assert(s_dma_ch  >= 0, "DA DMA ch0 unavailable");
    hard_assert(s_dma_ch2 >= 0, "DA DMA ch1 unavailable");

    pio_sm_set_enabled(DA_PIO, DA_SM, true);

    // C2PO (GPIO 3) low = no C2 errors; EMPH (GPIO 4) low = no pre-emphasis.
    // These are driven as plain GPIO outputs; the PIO does not touch them.
    gpio_init(DA_C2PO_PIN); gpio_set_dir(DA_C2PO_PIN, GPIO_OUT); gpio_put(DA_C2PO_PIN, 0);
    gpio_init(DA_EMPH_PIN); gpio_set_dir(DA_EMPH_PIN, GPIO_OUT); gpio_put(DA_EMPH_PIN, 0);

    s_initialised = true;

    printf("[DA] init %s speed, BCLK=%lu Hz\n",
           double_speed ? "2x" : "1x",
           (unsigned long)(135475200UL / (unsigned)(double_speed ? 16 : 32) / 2));
    LOG_INFO_MSG("DA", "init %s speed", double_speed ? "2x" : "1x");
}

// ---------------------------------------------------------------------------
// da_set_double_speed — change PIO clock divider at runtime
// ---------------------------------------------------------------------------
void da_set_double_speed(bool double_speed) {
    if (!s_initialised) return;
    if (s_double_speed == double_speed) return;

    s_double_speed = double_speed;
    pio_sm_set_clkdiv(DA_PIO, DA_SM, _clkdiv(double_speed));

    // Update subcode encoder rate to match DA speed (176400 samples/s at 1×,
    // 352800 at 2×; each sample takes 32 SM clock cycles in the PIO program).
    {
        float sub_clkdiv = (float)clock_get_hz(clk_sys) /
                           ((double_speed ? 176400.0F * 2.0F : 176400.0F) * 32.0F);
        pio_sm_set_clkdiv(SUBCODE_PIO, SUBCODE_SM, sub_clkdiv);
    }

    printf("[DA] speed → %s (clkdiv=%.0f)\n",
           double_speed ? "2x" : "1x", (double)_clkdiv(double_speed));
    LOG_INFO_MSG("DA", "speed → %s", double_speed ? "2x" : "1x");
}

bool da_is_double_speed(void) { return s_double_speed; }

// ---------------------------------------------------------------------------
// da_start_play — begin DMA streaming from start_lba
// ---------------------------------------------------------------------------
void da_start_play(sector_cache_t *cache, uint32_t start_lba) {
    if (!s_initialised) return;

    da_stop();  // abort any ongoing transfer before reconfiguring

    s_cache    = cache;
    s_next_lba = start_lba;
    s_playing  = true;
    s_paused   = false;
    __atomic_store_n(&s_eod_reached, false, __ATOMIC_RELEASE);  // fresh play → clear EOD edge

    // Pre-fill both ping-pong buffers
    uint32_t bytes;
    s_buf_lba[0] = s_next_lba;
    if (!sector_cache_get(s_cache, s_next_lba, s_raw, &bytes) || bytes == 0) {
        LOG_WARN_MSG("DA sector miss or SD error LBA=%lu at play start", (unsigned long)s_next_lba);
        s_playing = false;
        return;
    }
    expand_to_i2s24(s_raw, s_buf[0]);
    s_next_lba++;
    sector_cache_release_before(s_cache, s_next_lba);  // free just-consumed slot

    s_buf_lba[1] = s_next_lba;
    if (sector_cache_get(s_cache, s_next_lba, s_raw, &bytes) && bytes > 0) {
        expand_to_i2s24(s_raw, s_buf[1]);
        s_next_lba++;
        sector_cache_release_before(s_cache, s_next_lba);
    } else {
        memset(s_buf[1], 0, sizeof(s_buf[1]));  // silence pad if cache not yet full
    }

    // Wire the two DMA channels so each chains to the other when it finishes.
    // ch0 → buf[0] → FIFO, then auto-triggers ch1
    // ch1 → buf[1] → FIFO, then auto-triggers ch0
    _configure_dma_channel(s_dma_ch,  s_dma_ch2, s_buf[0]);
    _configure_dma_channel(s_dma_ch2, s_dma_ch,  s_buf[1]);

    // IRQ fires on each channel completion so the handler can refill the idle buffer
    dma_channel_set_irq0_enabled(s_dma_ch,  true);
    dma_channel_set_irq0_enabled(s_dma_ch2, true);
    irq_set_exclusive_handler(DMA_IRQ_0, _dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Push Q-channel for the first sector (buf[0]) before DMA starts so the
    // subcode PIO has data ready the instant the first sector begins streaming.
    _push_subcode(s_buf_lba[0]);

    // Kick off the first transfer; ch1 auto-starts when ch0 finishes
    dma_channel_start(s_dma_ch);

    printf("[DA] DMA play start LBA=%lu (%s)\n",
           (unsigned long)start_lba, s_double_speed ? "2x" : "1x");
    LOG_INFO_MSG("DA", "DMA play LBA=%lu", (unsigned long)start_lba);
}

// ---------------------------------------------------------------------------
// _dma_irq_handler — called on each sector completion
// ---------------------------------------------------------------------------
//
// By the time this fires, chain_to has already started the OTHER channel,
// so we have a full sector's worth of time (~13 ms at 1×) to refill the
// buffer that just finished before chain_to fires it again.
//
// Design choice — end-of-disc behaviour:
//   Option A (stop):    abort both channels, s_playing=false.
//                       commo_bridge polls da_is_playing() and sends STOP status.
//   Option B (silence): fill the completed buffer with zeros, keep streaming.
//                       akiko times out and sends STOP command itself.
//   Option C (loop):    rewind s_next_lba to start_lba and continue seamlessly.
//
// Currently implements Option A.  Change the else-branch below to switch.
//
static void __isr __not_in_flash_func(_dma_irq_handler)(void) {
    bool ch0_done = dma_channel_get_irq0_status(s_dma_ch);
    bool ch1_done = dma_channel_get_irq0_status(s_dma_ch2);

    // Identify the channel that just completed and the buffer it used
    int done_ch  = ch0_done ? s_dma_ch  : s_dma_ch2;
    int done_buf = ch0_done ? 0 : 1;

    dma_channel_acknowledge_irq0(done_ch);
    (void)ch1_done;  // one of ch0/ch1 always set; no other DMA uses IRQ0

    if (!s_playing || s_paused) return;
    if (!s_cache) return;

    // chain_to has already started the OTHER channel.  Push Q-channel subcode
    // for that sector NOW so the subcode PIO has data from the first bit-clock.
    _push_subcode(s_buf_lba[done_buf ^ 1]);

    uint32_t bytes;
    if (sector_cache_get(s_cache, s_next_lba, s_raw, &bytes)) {
        if (bytes > 0) {
            if (__atomic_load_n(&s_audio_mode, __ATOMIC_RELAXED)) {
                vis_audio_push_sector(s_raw);
            }
            expand_to_i2s24(s_raw, s_buf[done_buf]);
        } else {
            // SD error sentinel (valid slot, bytes=0): output silence for this sector
            memset(s_buf[done_buf], 0, SECTOR_DMA_WORDS * sizeof(uint32_t));
        }
        s_buf_lba[done_buf] = s_next_lba;
        s_next_lba++;
        sector_cache_release_before(s_cache, s_next_lba);  // free just-consumed slot
        dma_channel_set_read_addr(done_ch, s_buf[done_buf], false);
        dma_channel_set_trans_count(done_ch, SECTOR_DMA_WORDS, false);
        __atomic_store_n(&s_drq_pending, true, __ATOMIC_RELEASE);  // signal commo_bridge_poll
    } else {
#ifdef EOD_OPTION_B
        // Option B: stream silence until Akiko sends STOP_OPC.
        // Titles that poll Q-channel to detect lead-out need the drive to keep
        // running; da_stop() / STOP_OPC will abort cleanly via the s_playing guard above.
        // Point the finished channel at the pre-zeroed SRAM silence buffer — no
        // memset() in the ISR (memset is flash-resident; calling it here would
        // fault if a flash erase/program were in flight).
        s_buf_lba[done_buf] = s_next_lba;   // hold at EOD — don't advance
        dma_channel_set_read_addr(done_ch, s_silence_buf, false);
        dma_channel_set_trans_count(done_ch, SECTOR_DMA_WORDS, false);
        __atomic_store_n(&s_drq_pending, true, __ATOMIC_RELEASE);
        __atomic_store_n(&s_eod_reached, true, __ATOMIC_RELEASE);  // jukebox: signal EOD edge
        LOG_INFO_MSG("DA", "end of disc at LBA=%lu — streaming silence", (unsigned long)s_next_lba);
#else
        // Option A: abort both channels immediately (comment #define EOD_OPTION_B above to use).
        // chain_to has already re-triggered the other channel; disabling IRQ and
        // calling abort here is the only way to stop the loop — setting s_playing
        // alone does not prevent the other channel from firing another IRQ.
        dma_channel_set_irq0_enabled(s_dma_ch,  false);
        dma_channel_set_irq0_enabled(s_dma_ch2, false);
        irq_set_enabled(DMA_IRQ_0, false);
        dma_channel_abort(s_dma_ch);
        dma_channel_abort(s_dma_ch2);
        s_playing = false;
        LOG_INFO_MSG("DA", "end of disc at LBA=%lu", (unsigned long)s_next_lba);
#endif
    }
}

// ---------------------------------------------------------------------------
// da_stop
// ---------------------------------------------------------------------------
void da_stop(void) {
    if (!s_initialised) return;
    if (s_dma_ch  >= 0) { dma_channel_set_irq0_enabled(s_dma_ch,  false); dma_channel_abort(s_dma_ch);  }
    if (s_dma_ch2 >= 0) { dma_channel_set_irq0_enabled(s_dma_ch2, false); dma_channel_abort(s_dma_ch2); }
    irq_set_enabled(DMA_IRQ_0, false);
    // Reset PIO SM so residual words from the aborted DMA are not clocked out
    // to Akiko at the start of the next sector — that would corrupt the CD sync
    // pattern (0x00 FF*10 0x00) and break Akiko's framing for the next play.
    pio_sm_set_enabled(DA_PIO, DA_SM, false);
    pio_sm_clear_fifos(DA_PIO, DA_SM);
    pio_sm_restart(DA_PIO, DA_SM);
    pio_sm_exec(DA_PIO, DA_SM, pio_encode_jmp(s_offset));
    pio_sm_set_enabled(DA_PIO, DA_SM, true);
    pio_sm_clear_fifos(SUBCODE_PIO, SUBCODE_SM);
    s_playing = false;
    s_paused  = false;
    printf("[DA] stopped\n");
    LOG_INFO_MSG("DA", "stopped");
}

// ---------------------------------------------------------------------------
// da_pause / da_resume
// ---------------------------------------------------------------------------
void da_pause(void) {
    if (!s_playing || s_paused) return;
    dma_channel_set_irq0_enabled(s_dma_ch,  false);
    dma_channel_set_irq0_enabled(s_dma_ch2, false);
    irq_set_enabled(DMA_IRQ_0, false);
    dma_channel_abort(s_dma_ch);
    dma_channel_abort(s_dma_ch2);
    // Drain PIO TX FIFO: words already queued would clock out as stale I2S
    // frames on resume, corrupting the first audio samples Akiko receives.
    pio_sm_set_enabled(DA_PIO, DA_SM, false);
    pio_sm_clear_fifos(DA_PIO, DA_SM);
    pio_sm_restart(DA_PIO, DA_SM);
    pio_sm_exec(DA_PIO, DA_SM, pio_encode_jmp(s_offset));
    pio_sm_set_enabled(DA_PIO, DA_SM, true);
    s_paused = true;
    printf("[DA] paused at LBA=%lu\n", (unsigned long)s_next_lba);
    LOG_INFO_MSG("DA", "paused LBA=%lu", (unsigned long)s_next_lba);
}

void da_resume(void) {
    if (!s_playing || !s_paused || !s_cache) return;

    // Resume from the earliest of the two loaded buffers — that's the last sector
    // that was actually handed to DMA.  Using s_next_lba-2 is wrong if the cache
    // advanced past the buffer boundary; the buf_lba[] slots hold the truth.
    uint32_t resume_lba = (s_buf_lba[0] < s_buf_lba[1]) ? s_buf_lba[0] : s_buf_lba[1];
    s_next_lba = resume_lba;
    // Flush and reseek so Core 1 prefetch resumes from the correct position;
    // without this the cache may still be fetching sectors well ahead of
    // resume_lba and the first sector_cache_get() below will return stale data.
    sector_cache_seek(s_cache, resume_lba);

    // No spin-wait here: if Core 1 hasn't prefetched yet the sector_cache_get()
    // calls below will miss and silence-pad the first sector(s).  The DMA ISR
    // will refill with real data on the next completion cycle (~13 ms at 1×).
    uint32_t bytes;
    s_buf_lba[0] = s_next_lba;
    if (sector_cache_get(s_cache, s_next_lba, s_raw, &bytes) && bytes > 0) {
        expand_to_i2s24(s_raw, s_buf[0]);
        s_next_lba++;
        sector_cache_release_before(s_cache, s_next_lba);
    } else {
        memset(s_buf[0], 0, sizeof(s_buf[0]));
    }
    s_buf_lba[1] = s_next_lba;
    if (sector_cache_get(s_cache, s_next_lba, s_raw, &bytes) && bytes > 0) {
        expand_to_i2s24(s_raw, s_buf[1]);
        s_next_lba++;
        sector_cache_release_before(s_cache, s_next_lba);
    } else {
        memset(s_buf[1], 0, sizeof(s_buf[1]));
    }

    _configure_dma_channel(s_dma_ch,  s_dma_ch2, s_buf[0]);
    _configure_dma_channel(s_dma_ch2, s_dma_ch,  s_buf[1]);

    dma_channel_set_irq0_enabled(s_dma_ch,  true);
    dma_channel_set_irq0_enabled(s_dma_ch2, true);
    irq_set_enabled(DMA_IRQ_0, true);

    _push_subcode(s_buf_lba[0]);
    // Clear s_paused only now — DMA is armed and IRQ is enabled.
    // Setting it earlier would allow a stale deferred IRQ to enter the ISR
    // body before channels are reconfigured, calling set_read_addr on a
    // not-yet-configured channel.
    s_paused = false;
    dma_channel_start(s_dma_ch);

    printf("[DA] resumed at LBA=%lu\n", (unsigned long)resume_lba);
    LOG_INFO_MSG("DA", "resumed LBA=%lu", (unsigned long)resume_lba);
}

void da_set_audio_mode(bool is_audio) {
    __atomic_store_n(&s_audio_mode, is_audio, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// da_nudge_clkdiv_to_m17sine — trim BCLK to match the CD32 reference clock
// ---------------------------------------------------------------------------
// Target clkdiv: sys_clk × speed_factor / m17sine_hz
//   1× speed:  speed_factor = 4  (BCLK = M17SINE/8, clkdiv=32 nominal)
//   2× speed:  speed_factor = 2  (BCLK = M17SINE/4, clkdiv=16 nominal)
//
// The 16.8 fixed-point representation of clkdiv is computed by multiplying
// the rational result by 256, then splitting: int_part = result>>8, frac = result&0xFF.
//
// Clamp to ±0.5% of the nominal clkdiv to avoid runaway on a bad reading.
void da_nudge_clkdiv_to_m17sine(uint32_t m17sine_hz) {
    // Reject implausible readings (should be ~16.9344 MHz ± a few hundred ppm)
    if (m17sine_hz < 16800000U || m17sine_hz > 17100000U) return;

    uint32_t sys_hz    = clock_get_hz(clk_sys);
    uint32_t factor    = s_double_speed ? 2U : 4U;

    // Compute clkdiv × 256 with 64-bit intermediate to avoid overflow
    uint64_t div_256   = ((uint64_t)sys_hz * factor * 256U) / m17sine_hz;

    // Nominal clkdiv×256 for sanity-clamp (32×256=8192 at 1×, 16×256=4096 at 2×)
    uint32_t nom_256   = (uint32_t)_clkdiv(s_double_speed) * 256U;
    uint32_t half_pct  = nom_256 / 200U;  // 0.5% of nominal
    if (div_256 < nom_256 - half_pct || div_256 > nom_256 + half_pct) return;

    uint16_t div_int   = (uint16_t)(div_256 >> 8);
    uint8_t  div_frac  = (uint8_t)(div_256 & 0xFF);

    pio_sm_set_clkdiv_int_frac(DA_PIO, DA_SM, div_int, div_frac);
    s_clkdiv_fixed = (uint32_t)div_256;
}

uint32_t da_get_clkdiv_fixed(void) {
    return s_clkdiv_fixed;
}

bool da_is_playing(void) {
    return s_playing && !s_paused;
}

uint32_t da_get_current_lba(void) {
    return (s_playing || s_paused) ? s_next_lba : 0;
}

uint32_t da_get_resume_lba(void) {
    if (!s_playing && !s_paused) return 0;
    return (s_buf_lba[0] < s_buf_lba[1]) ? s_buf_lba[0] : s_buf_lba[1];
}

bool da_drq_pending(void) {
    if (!__atomic_load_n(&s_drq_pending, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&s_drq_pending, false, __ATOMIC_RELEASE);
    return true;
}

// Test-and-clear the end-of-disc edge.  Set once per sector while the ISR streams
// silence at EOD (Option B); returns true exactly when the main-loop jukebox poll
// observes a fresh end-of-disc.  Cleared here and on every da_start_play().
bool da_take_eod_event(void) {
    return __atomic_exchange_n(&s_eod_reached, false, __ATOMIC_ACQ_REL);
}
