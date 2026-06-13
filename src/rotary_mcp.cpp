// =============================================================================
// rotary_mcp.cpp — Rotary Encoder via MCP23017 I2C GPIO Expander
// =============================================================================
//
// The encoder (CLK, DT, SW) connects to MCP23017 port A pins GPA0–GPA2.
// The MCP23017 INTA output connects to Pico GPIO MCP23017_INT_PIN (active-low).
//
// HARDWARE CONNECTIONS (MCP23017 GPA side):
//   GPA0 ← encoder CLK (A)
//   GPA1 ← encoder DT  (B)
//   GPA2 ← encoder SW  (push-button, active low)
//   INTA → GPIO MCP23017_INT_PIN on Pico (open-drain, pull up on Pico side)
//
// I2C BUS:
//   i2c1 must be initialised by main() before calling rotary_init().
//   SDA = GPIO 26 (PIN_MCP23017_SDA), SCL = GPIO 27 (PIN_MCP23017_SCL) @ 400 kHz.
//
// INTERRUPT STRATEGY:
//   The GPIO IRQ on MCP23017_INT_PIN only sets a flag (no I2C in IRQ context).
//   Actual I2C reads are deferred to rotary_poll(), called from the main loop.
//   This is safe for a slow HID device like a disc selector.
// =============================================================================

// C++ library headers (must precede extern "C" block)
#include "mcp23017.h"

// All public symbols use C linkage so main.c can call them without mangling
extern "C" {
#include "rotary.h"
#include "logger.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include <string.h>
#include <stdio.h>
}

// MCP23017 is on I2C1 (GPIO 26/27). GPIO 29 is now WiFi RM2 SPI CLK.

// ---------------------------------------------------------------------------
// MCP23017 GPA pin assignments
// ---------------------------------------------------------------------------
#define MCP_PIN_CLK  0   // GPA0 = encoder CLK (A)
#define MCP_PIN_DT   1   // GPA1 = encoder DT  (B)
#define MCP_PIN_SW   2   // GPA2 = push-button  (active low)
#define MCP_PIN_LOG  8   // GPB0 = logger toggle button (active low)

// ---------------------------------------------------------------------------
// Event ring buffer
// ---------------------------------------------------------------------------
#define EVENT_QUEUE_SIZE  16

typedef struct {
    rotary_event_t type;
    int            steps;
} rotary_queued_event_t;

static rotary_queued_event_t s_event_queue[EVENT_QUEUE_SIZE];
static volatile int s_eq_head = 0;
static volatile int s_eq_tail = 0;

static void eq_push(rotary_event_t type, int steps) {
    int next = (s_eq_head + 1) % EVENT_QUEUE_SIZE;
    if (next != s_eq_tail) {
        s_event_queue[s_eq_head].type  = type;
        s_event_queue[s_eq_head].steps = steps;
        s_eq_head = next;
    }
}

// ---------------------------------------------------------------------------
// Quadrature state machine
// ---------------------------------------------------------------------------
// [old_state*4 + new_state] → +1 (CW), -1 (CCW), 0 (invalid/bounce)
static const int8_t s_quad_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

static volatile uint8_t  s_quad_state  = 0;
static volatile int32_t  s_count       = 0;
static volatile int32_t  s_half_steps  = 0;
static absolute_time_t   s_last_turn_time;

// ---------------------------------------------------------------------------
// Button state — encoder push-button
// ---------------------------------------------------------------------------
static bool            s_btn_pressed    = false;
static absolute_time_t s_btn_press_time;
static bool            s_btn_last_level = true;   // idle = high (active-low btn)

// Logger toggle button (GPB0) state
static bool            s_log_pressed    = false;
static absolute_time_t s_log_press_time;
static bool            s_log_last_level = true;   // idle = high (active-low btn)

// ---------------------------------------------------------------------------
// MCP23017 instance and deferred-read flag
// ---------------------------------------------------------------------------
static Mcp23017 *s_mcp = nullptr;

// Last polled time — throttle I2C reads to every 5 ms
static absolute_time_t s_last_poll_time;
static bool s_poll_init = false;

// ---------------------------------------------------------------------------
// rotary_init
// ---------------------------------------------------------------------------
extern "C" void rotary_init(void) {
    printf("[ROT] MCP23017 encoder init on I2C1 0x%02X (GPA0=CLK GPA1=DT GPA2=SW)\n",
           MCP23017_I2C_ADDR);

    // Static instance on I2C1 — lives for the duration of the program
    static Mcp23017 mcp_inst(i2c1, MCP23017_I2C_ADDR);
    s_mcp = &mcp_inst;

    s_mcp->setup(false, false);      // no interrupt mirroring needed (polled)
    s_mcp->set_io_direction(0xFFFF); // all pins input
    s_mcp->set_pullup(0x0107);       // pull-ups on GPA0/1/2 + GPB0 (logger button)

    // Prime decoder from current levels
    s_mcp->update_and_get_input_values();
    bool clk_init    = s_mcp->get_last_input_pin_value(MCP_PIN_CLK);
    bool dt_init     = s_mcp->get_last_input_pin_value(MCP_PIN_DT);
    s_btn_last_level = s_mcp->get_last_input_pin_value(MCP_PIN_SW);
    s_quad_state     = ((uint8_t)clk_init << 1) | (uint8_t)dt_init;
    s_last_turn_time = get_absolute_time();
    s_last_poll_time = get_absolute_time();
    s_poll_init      = true;

    LOG_INFO_MSG("ROT ", "MCP23017 polled rotary ready (I2C1 0x%02X)", MCP23017_I2C_ADDR);
    printf("[ROT] Ready (polled)\n");
}

// ---------------------------------------------------------------------------
// rotary_poll — dequeue one event (I2C read deferred here, not in IRQ)
// ---------------------------------------------------------------------------
extern "C" rotary_event_t rotary_poll(int *steps_out) {
    // Throttle I2C reads: poll MCP23017 at most every 5 ms
    if (s_poll_init && s_mcp != nullptr) {
        absolute_time_t now_poll = get_absolute_time();
        int64_t elapsed_ms = absolute_time_diff_us(s_last_poll_time, now_poll) / 1000;
        if (elapsed_ms >= 5) {
            s_last_poll_time = now_poll;

            s_mcp->update_and_get_input_values();
            bool clk = s_mcp->get_last_input_pin_value(MCP_PIN_CLK);
            bool dt  = s_mcp->get_last_input_pin_value(MCP_PIN_DT);
            bool sw  = s_mcp->get_last_input_pin_value(MCP_PIN_SW);  // false = pressed

            // ---- Quadrature decoder ----
            uint8_t new_state = ((uint8_t)clk << 1) | (uint8_t)dt;
            int8_t  delta     = s_quad_table[s_quad_state * 4 + new_state];
            s_quad_state      = new_state;

            if (delta != 0) {
                s_half_steps += delta;
                if (s_half_steps >= 2 || s_half_steps <= -2) {
                    int direction = (s_half_steps > 0) ? 1 : -1;
                    s_half_steps  = 0;
                    s_count      += direction;

                    absolute_time_t now2 = get_absolute_time();
                    int64_t gap_ms = absolute_time_diff_us(s_last_turn_time, now2) / 1000;
                    s_last_turn_time = now2;

                    int steps = (gap_ms < ROTARY_ACCEL_THRESHOLD_MS && gap_ms > 0)
                                ? ROTARY_ACCEL_FACTOR : 1;
                    eq_push((direction > 0) ? ROTARY_CW : ROTARY_CCW, steps);
                }
            }

            // ---- Button state machine ----
            if (sw != s_btn_last_level) {
                absolute_time_t now3 = get_absolute_time();
                s_btn_last_level = sw;

                if (!sw) {
                    s_btn_pressed    = true;
                    s_btn_press_time = now3;
                } else {
                    if (s_btn_pressed) {
                        int64_t held_ms = absolute_time_diff_us(s_btn_press_time, now3) / 1000;
                        s_btn_pressed   = false;
                        if (held_ms >= ROTARY_LONG_PRESS_MS) {
                            eq_push(ROTARY_LONG_PRESS, 1);
                        } else if (held_ms >= ROTARY_BTN_DEBOUNCE_MS) {
                            eq_push(ROTARY_PRESS, 1);
                        }
                    }
                }
            }

            // ---- Logger toggle button (GPB0) ----
            bool log_sw = s_mcp->get_last_input_pin_value(MCP_PIN_LOG);
            if (log_sw != s_log_last_level) {
                absolute_time_t now4 = get_absolute_time();
                s_log_last_level = log_sw;
                if (!log_sw) {
                    s_log_pressed    = true;
                    s_log_press_time = now4;
                } else if (s_log_pressed) {
                    int64_t held_ms = absolute_time_diff_us(s_log_press_time, now4) / 1000;
                    s_log_pressed   = false;
                    if (held_ms >= ROTARY_BTN_DEBOUNCE_MS) {
                        eq_push(ROTARY_LOG_PRESS, 1);
                    }
                }
            }
        }
    }

    // Dequeue one event
    if (s_eq_tail == s_eq_head) {
        if (steps_out) *steps_out = 0;
        return ROTARY_NONE;
    }
    rotary_queued_event_t ev = s_event_queue[s_eq_tail];
    s_eq_tail = (s_eq_tail + 1) % EVENT_QUEUE_SIZE;
    if (steps_out) *steps_out = ev.steps;
    return ev.type;
}

extern "C" bool rotary_button_held(void) {
    return s_btn_pressed;
}

extern "C" int32_t rotary_get_count(void) {
    return s_count;
}

extern "C" void rotary_reset_count(void) {
    s_count      = 0;
    s_half_steps = 0;
}
