#pragma once
// =============================================================================
// da_output.h — CD Digital Audio / Data serial output (PIO + DMA)
// =============================================================================
//
// Drives the DA_DATA / DA_BCLK / DA_LRCLK lines (GPIO 0-2) using the
// da_output PIO program on PIO0 SM0.
//
// Signal format: 24-bit I2S (MSB first), matching CXD2545Q output
//   BCLK  = 2.1168 MHz at 1× speed  (sys_clk / 32 / 2)
//   LRCLK = 44.1 kHz                (BCLK / 48, 24 BCLK cycles per channel)
//
// The DMA engine feeds uint32_t words with one channel per word:
//   [31:16] = 16-bit PCM sample, [15:0] = zero padding.
// PIO autopull threshold = 24: outputs bits [31:8] (16 data + 8 zeros) and
// silently discards [7:0] on each pull.
//
// Speed control:
//   1× CD speed: clkdiv = 32   → BCLK = 2,117,112 Hz
//   2× CD speed: clkdiv = 16   → BCLK = 4,234,225 Hz
//
// sys_clk MUST be 135,475,200 Hz (set by main.c) before calling da_output_init().
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"
#include "sector_cache.h"

// PIO resource used by this module
#define DA_PIO           pio0
#define DA_SM            0
#define DA_DATA_BASE_PIN 0    // GPIO 0 = DA_DATA; GPIO 1 = BCLK; GPIO 2 = LRCLK

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

// Load the da_output PIO program onto DA_PIO SM0 and start the state machine.
// Call once from main() after set_sys_clock_khz(135475, true).
// double_speed: false = 1× CD rate, true = 2× CD rate.
void da_output_init(bool double_speed);

// ---------------------------------------------------------------------------
// Speed control (2× CD support)
// ---------------------------------------------------------------------------

// Enable or disable the visualiser audio snoop.
// Set true when playing CD-DA audio tracks; false for data sectors.
void da_set_audio_mode(bool is_audio);

// Change the DA output clock rate at runtime.
// Safe to call while the SM is running (adjusts PIO clkdiv only).
// double_speed: false = 1× (BCLK 1.4112 MHz), true = 2× (BCLK 2.8224 MHz).
void da_set_double_speed(bool double_speed);

// Returns the current speed setting.
bool da_is_double_speed(void);

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

// Begin streaming sectors from 'cache' starting at 'start_lba' via DMA.
// Each 2352-byte sector is packed into 1176 × 32-bit words and pushed to the
// PIO TX FIFO.  The DA SM clocks these bits out as the I2S stream.
void da_start_play(sector_cache_t *cache, uint32_t start_lba);

// Halt the DMA transfer immediately (mid-sector).  The PIO SM keeps running
// but the TX FIFO will starve (PIO will hold the last bit level).
void da_stop(void);

// Suspend DMA without resetting position (call da_resume() to continue).
void da_pause(void);

// Resume a paused transfer from where it stopped.
void da_resume(void);

// Returns true if the DMA channel is currently active.
bool da_is_playing(void);

// Returns the approximate LBA currently being streamed (next sector to fetch).
// Valid during play and pause; 0 when stopped.
uint32_t da_get_current_lba(void);

// ---------------------------------------------------------------------------
// DRQ signalling
// ---------------------------------------------------------------------------

// Returns true (and clears the flag) when a sector has just been delivered
// to the PIO FIFO and a DRIVE_STATUS_DRQ packet should be sent to the host.
// Set by the DMA ISR; cleared here.  Safe to call from Core 0 poll loop.
bool da_drq_pending(void);
