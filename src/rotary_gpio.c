// =============================================================================
// rotary_gpio.c — Rotary Encoder via direct GPIO (no MCP23017 I2C expander)
// =============================================================================
//
// HARDWARE CONNECTIONS (Core2350B0):
//   GPIO 12 (PIN_ENC_A)   ← encoder CLK / A
//   GPIO 15 (PIN_ENC_B)   ← encoder DT  / B
//   GPIO 18 (PIN_ENC_SW)  ← encoder push-button (active low)
//   GPIO 19 (PIN_ENC_LOG) ← logger toggle button (active low)
//
// All four pins have internal pull-ups enabled.  Encoder A/B lines must be
// connected to the Pico with short traces (no buffer needed — GPIO thresholds
// are clean for mechanical detents at normal rotational speeds).
//
// POLLING:
//   rotary_poll() is called from the Core 0 main loop every ~200 µs.
//   A 5 ms throttle on the encoder and a 50 ms debounce on the buttons give
//   the same response characteristics as the MCP23017 polled implementation,
//   with lower latency and no I2C overhead.
// =============================================================================

#include "rotary.h"
#include "logger.h"
#include "gpio_map.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/sync.h"   // save_and_disable_interrupts — guard ring vs IRQ producer
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>       // _Atomic ring indices — memory ordering in the type

// ---------------------------------------------------------------------------
// Event ring buffer
// ---------------------------------------------------------------------------
#define EVENT_QUEUE_SIZE  16

typedef struct {
    rotary_event_t type;
    int            steps;
} rotary_queued_event_t;

static rotary_queued_event_t s_event_queue[EVENT_QUEUE_SIZE];

// SPSC ring indices — _Atomic with acquire/release ordering (same pattern as
// vis_audio.c).  Producer (encoder IRQ; buttons via eq_push_safe) owns s_eq_head;
// consumer (Core 0 rotary_poll) owns s_eq_tail.  The release store on the head
// publishes the slot's payload before the index advances; the consumer's acquire
// load of the head pairs with it so the payload is visible before it is read.
// Encoding the ordering in the type makes the cross-context contract auditable
// instead of relying on a `volatile` + comment.
static _Atomic int s_eq_head = 0;
static _Atomic int s_eq_tail = 0;

// Single-producer push.  Called directly from the encoder GPIO IRQ.
static void eq_push(rotary_event_t type, int steps) {
    int head = atomic_load_explicit(&s_eq_head, memory_order_relaxed);  // producer owns head
    int tail = atomic_load_explicit(&s_eq_tail, memory_order_acquire);  // observe consumer frees
    int next = (head + 1) % EVENT_QUEUE_SIZE;
    if (next != tail) {                          // ring not full
        s_event_queue[head].type  = type;
        s_event_queue[head].steps = steps;
        // Release: payload stores must be visible before the head advances.
        atomic_store_explicit(&s_eq_head, next, memory_order_release);
    }
}

// IRQ-safe push for the button paths, which run in Core 0 thread context.  The
// encoder now pushes from IRQ context, so the ring briefly has two producers;
// disabling interrupts around the thread-side push keeps the head update atomic
// w.r.t. the encoder IRQ (the IRQ side can never be preempted by the thread).
static void eq_push_safe(rotary_event_t type, int steps) {
    uint32_t save = save_and_disable_interrupts();
    eq_push(type, steps);
    restore_interrupts(save);
}

// ---------------------------------------------------------------------------
// Quadrature state machine (identical to the MCP23017 implementation)
// [old_state*4 + new_state] → +1 (CW), -1 (CCW), 0 (invalid/bounce)
// ---------------------------------------------------------------------------
static const int8_t s_quad_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

static volatile uint8_t  s_quad_state  = 0;
static volatile int32_t  s_half_steps  = 0;
static absolute_time_t   s_last_turn_time;

// ---------------------------------------------------------------------------
// Button state — encoder push-button
// ---------------------------------------------------------------------------
static bool            s_btn_pressed    = false;
static absolute_time_t s_btn_press_time;
static bool            s_btn_last_level = true;  // idle = high (active-low)

// Logger toggle button state
static bool            s_log_pressed    = false;
static absolute_time_t s_log_press_time;
static bool            s_log_last_level = true;  // idle = high (active-low)

// Button polling throttle — buttons are read at most every 1 ms.  (The encoder
// quadrature lines are now decoded in the GPIO IRQ below, not polled.)
static absolute_time_t s_last_poll_time;
static bool            s_poll_init = false;

// ---------------------------------------------------------------------------
// Encoder quadrature GPIO IRQ
// ---------------------------------------------------------------------------
// Fires on every edge of ENC_A or ENC_B.  Decoding in the IRQ (rather than at a
// 1 ms poll) means no detent is ever missed during a fast spin: at the poll rate
// a fast turn could move both A and B between samples, which the Gray-code table
// reads as an invalid double-transition (== 0) and silently drops.  The encoder
// lines are slow and bounce-limited, so there is no IRQ-storm risk.
//
// Flash safety: flash erase/program disables IRQs on this core for the whole
// operation, so this handler (and its read of the flash-resident s_quad_table)
// can never run during a flash window — no SRAM pinning required.
static void _enc_gpio_irq(uint gpio, uint32_t events) {
    (void)gpio; (void)events;

    bool clk = gpio_get(PIN_ENC_A);
    bool dt  = gpio_get(PIN_ENC_B);

    uint8_t new_state = ((uint8_t)clk << 1) | (uint8_t)dt;
    int8_t  delta     = s_quad_table[s_quad_state * 4 + new_state];
    s_quad_state      = new_state;

    if (delta == 0) return;

    s_half_steps += delta;
    if (s_half_steps >= 2 || s_half_steps <= -2) {
        int direction = (s_half_steps > 0) ? 1 : -1;
        s_half_steps  = 0;

        absolute_time_t now = get_absolute_time();
        int64_t gap_ms = absolute_time_diff_us(s_last_turn_time, now) / 1000;
        s_last_turn_time = now;

        int steps = (gap_ms < ROTARY_ACCEL_THRESHOLD_MS && gap_ms > 0)
                    ? ROTARY_ACCEL_FACTOR : 1;
        eq_push((direction > 0) ? ROTARY_CW : ROTARY_CCW, steps);
    }
}

// ---------------------------------------------------------------------------
// rotary_init
// ---------------------------------------------------------------------------
void rotary_init(void) {
    printf("[ROT] Direct GPIO encoder init (A=GPIO%d B=GPIO%d SW=GPIO%d LOG=GPIO%d)\n",
           PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_ENC_LOG);

    gpio_init(PIN_ENC_A);   gpio_set_dir(PIN_ENC_A,   GPIO_IN); gpio_pull_up(PIN_ENC_A);
    gpio_init(PIN_ENC_B);   gpio_set_dir(PIN_ENC_B,   GPIO_IN); gpio_pull_up(PIN_ENC_B);
    gpio_init(PIN_ENC_SW);  gpio_set_dir(PIN_ENC_SW,  GPIO_IN); gpio_pull_up(PIN_ENC_SW);
    gpio_init(PIN_ENC_LOG); gpio_set_dir(PIN_ENC_LOG, GPIO_IN); gpio_pull_up(PIN_ENC_LOG);

    // Prime decoder from current pin levels
    bool clk_init    = gpio_get(PIN_ENC_A);
    bool dt_init     = gpio_get(PIN_ENC_B);
    s_btn_last_level = gpio_get(PIN_ENC_SW);
    s_log_last_level = gpio_get(PIN_ENC_LOG);
    s_quad_state     = ((uint8_t)clk_init << 1) | (uint8_t)dt_init;
    s_last_turn_time = get_absolute_time();
    s_last_poll_time = get_absolute_time();
    s_poll_init      = true;

    gpio_set_irq_enabled_with_callback(PIN_ENC_A,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &_enc_gpio_irq);
    gpio_set_irq_enabled(PIN_ENC_B,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    LOG_INFO_MSG("ROT ", "direct GPIO rotary ready (A=%d B=%d SW=%d LOG=%d)",
                 PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_ENC_LOG);
    printf("[ROT] Ready\n");
}

// ---------------------------------------------------------------------------
// rotary_poll — debounce buttons and dequeue events
// ---------------------------------------------------------------------------
// The encoder is decoded in _enc_gpio_irq(); this function only polls the two
// buttons (1 ms throttle) and drains the shared event ring.
rotary_event_t rotary_poll(int *steps_out) {
    if (s_poll_init) {
        absolute_time_t now_poll = get_absolute_time();
        int64_t elapsed_us = absolute_time_diff_us(s_last_poll_time, now_poll);
        if (elapsed_us >= 1000) {   // 1 ms button throttle
            s_last_poll_time = now_poll;

            bool sw  = gpio_get(PIN_ENC_SW);   // false = pressed (active-low)
            bool log = gpio_get(PIN_ENC_LOG);  // false = pressed (active-low)

            // ---- Encoder push-button ----
            if (sw != s_btn_last_level) {
                absolute_time_t now3 = get_absolute_time();
                s_btn_last_level = sw;
                if (!sw) {
                    s_btn_pressed    = true;
                    s_btn_press_time = now3;
                } else if (s_btn_pressed) {
                    int64_t held_ms = absolute_time_diff_us(s_btn_press_time, now3) / 1000;
                    s_btn_pressed   = false;
                    if (held_ms >= ROTARY_LONG_PRESS_MS) {
                        eq_push_safe(ROTARY_LONG_PRESS, 1);
                    } else if (held_ms >= ROTARY_BTN_DEBOUNCE_MS) {
                        eq_push_safe(ROTARY_PRESS, 1);
                    }
                }
            }

            // ---- Logger toggle button ----
            if (log != s_log_last_level) {
                absolute_time_t now4 = get_absolute_time();
                s_log_last_level = log;
                if (!log) {
                    s_log_pressed    = true;
                    s_log_press_time = now4;
                } else if (s_log_pressed) {
                    int64_t held_ms = absolute_time_diff_us(s_log_press_time, now4) / 1000;
                    s_log_pressed   = false;
                    if (held_ms >= ROTARY_BTN_DEBOUNCE_MS) {
                        eq_push_safe(ROTARY_LOG_PRESS, 1);
                    }
                }
            }
        }
    }

    // Dequeue one event
    int tail = atomic_load_explicit(&s_eq_tail, memory_order_relaxed);  // consumer owns tail
    // Acquire: pairs with the producer's release store on s_eq_head so the
    // slot payload is visible before we read it.
    int head = atomic_load_explicit(&s_eq_head, memory_order_acquire);
    if (tail == head) {
        if (steps_out) *steps_out = 0;
        return ROTARY_NONE;
    }
    rotary_queued_event_t ev = s_event_queue[tail];
    // Release: the slot is visibly free before the producer may reuse it.
    atomic_store_explicit(&s_eq_tail, (tail + 1) % EVENT_QUEUE_SIZE, memory_order_release);
    if (steps_out) *steps_out = ev.steps;
    return ev.type;
}

bool rotary_button_held(void) {
    return s_btn_pressed;
}
