#pragma once
// =============================================================================
// rotary.h — Rotary Encoder Driver
// =============================================================================
//
// Debounced quadrature rotary encoder driver with push-button support.
// Used to scroll through disc images on the CD32 ODE without needing a
// computer — just turn the knob and press to select.
//
// HARDWARE:
//   Any standard 20-detent incremental rotary encoder with common ground.
//   Recommended: Alps EC11, Bourns PEC11, or cheap KY-040 module.
//
//   Encoder connects directly to RP2350B GPIOs (no I2C expander):
//     Encoder CLK (A) → GPIO 12 (PIN_ENC_A)   — internal pull-up
//     Encoder DT  (B) → GPIO 15 (PIN_ENC_B)   — internal pull-up
//     Encoder SW  (push-button) → GPIO 18 (PIN_ENC_SW)  — active low, pull-up
//     Logger toggle button      → GPIO 19 (PIN_ENC_LOG) — active low, pull-up
//
//   GPIO 44/45/46 are reserved for the COMMO bus (IF_CLK/IF_DATA/IF_DIR).
//
// ALGORITHM:
//   Uses a 2-bit Gray-code state machine to decode quadrature pulses.
//   The state machine detects both half-steps (each detent generates one
//   full quadrature cycle = 4 transitions, but we count every 2 transitions
//   = one "click" per detent).
//
//   Full quadrature cycle:
//     CLK: ─┐  ┌─┐  ┌─
//     DT:  ──┐  ┌─┐  ┌
//     Steps: AB→A→0→B→AB (CW)
//            AB→B→0→A→AB (CCW)
//
// DECODE / POLLING:
//   Quadrature is decoded in an edge IRQ on ENC_A/ENC_B (both edges) so no
//   detent is dropped on a fast spin.  The push-buttons are debounced by
//   polling inside rotary_poll() (1 ms throttle) from the Core 0 main loop;
//   rotary_poll() also drains the event ring.
//
// THREAD SAFETY:
//   Encoder state is written only in IRQ context.  Encoder events and button
//   events share one SPSC ring; the button (thread-side) push masks IRQs
//   briefly so the ring head is consistent against the IRQ-side producer.
//   32-bit aligned count reads are atomic on RP2350's Cortex-M33.
// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// Debounce timing
// ---------------------------------------------------------------------------
// Software debounce window for the push-button (milliseconds).
// Turns within this window are ignored (mechanical contact bounce).
#define ROTARY_BTN_DEBOUNCE_MS   50

// Minimum time between encoder transitions to be considered valid (µs).
// Below this threshold, transitions are treated as bounce.
#define ROTARY_ENC_DEBOUNCE_US   500

// ---------------------------------------------------------------------------
// Event types returned by rotary_poll()
// ---------------------------------------------------------------------------
typedef enum {
    ROTARY_NONE       = 0,  // No event pending
    ROTARY_CW         = 1,  // Turned clockwise (one detent)
    ROTARY_CCW        = 2,  // Turned counter-clockwise (one detent)
    ROTARY_PRESS      = 3,  // Button pressed (falling edge, debounced)
    ROTARY_LONG_PRESS = 4,  // Button held for > ROTARY_LONG_PRESS_MS
    ROTARY_LOG_PRESS  = 5,  // Logger toggle button (GPIO 19 / PIN_ENC_LOG) pressed
} rotary_event_t;

// Long press threshold in milliseconds
#define ROTARY_LONG_PRESS_MS  800

// ---------------------------------------------------------------------------
// Acceleration: faster turning = bigger jumps through the list
// ---------------------------------------------------------------------------
// When turns arrive faster than ROTARY_ACCEL_THRESHOLD_MS apart,
// the step size multiplies by ROTARY_ACCEL_FACTOR for that turn.
#define ROTARY_ACCEL_THRESHOLD_MS  100  // Turns within 100 ms = "fast spin"
#define ROTARY_ACCEL_FACTOR          5  // Jump 5 entries per detent when fast

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise GPIO pins, pull-ups, and IRQ handlers.
// Call once from main() before using any other rotary_ functions.
void rotary_init(void);

// Poll for the next pending event.
// Returns ROTARY_NONE if no event is waiting.
// Events are queued in a small ring buffer; oldest event returned first.
// 'steps_out' receives the step count for CW/CCW events (normally 1,
// but >1 when acceleration kicks in for fast spinning).
rotary_event_t rotary_poll(int *steps_out);

// Returns true if the button is currently held down (useful for combo checks).
bool rotary_button_held(void);

