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
//   KY-040 module (most common, includes resistors):
//     CLK  → GPIO 15  (Encoder A / CLK)
//     DT   → GPIO 16  (Encoder B / DT)
//     SW   → GPIO 17  (Push-button, active low)
//     VCC  → 3V3
//     GND  → GND
//
//   Bare encoder (no module):
//     Add 10 kΩ pull-up resistors to 3.3 V on CLK, DT, and SW pins.
//     Add 100 nF capacitors from each pin to GND for hardware debounce.
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
// INTERRUPT HANDLING:
//   Both CLK and DT are monitored via GPIO edge interrupts.
//   The push-button SW uses a separate IRQ with software debounce.
//   All IRQs are shared on RP2350 GPIO IRQ (single callback).
//
// THREAD SAFETY:
//   The encoder state variables are written only in IRQ context and read
//   in Core 0 main loop.  Atomic reads of volatile counts are safe on
//   RP2350's Cortex-M33 (single-cycle 32-bit reads are atomic).
// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// GPIO pin assignments (match CMakeLists.txt)
// ---------------------------------------------------------------------------
#ifndef ROTARY_CLK_PIN
#define ROTARY_CLK_PIN  15
#endif
#ifndef ROTARY_DT_PIN
#define ROTARY_DT_PIN   16
#endif
#ifndef ROTARY_SW_PIN
#define ROTARY_SW_PIN   17
#endif

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
    ROTARY_LOG_PRESS  = 5,  // Logger toggle button (MCP23017 GPB0) pressed
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

// Returns the raw accumulated count of CW turns minus CCW turns since init.
// Useful for absolute position tracking.
int32_t rotary_get_count(void);

// Reset the accumulated count to zero.
void rotary_reset_count(void);

