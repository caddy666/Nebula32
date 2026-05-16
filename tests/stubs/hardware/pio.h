#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct pio_hw_t { int _d; } pio_hw_t;
typedef pio_hw_t *PIO;
typedef unsigned int uint;

typedef struct {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin;
} pio_program_t;

// Each TU gets its own static instance — fine for stubs since we only
// care that a non-NULL pointer is passed, not that it's the same object.
static pio_hw_t _pio0_inst __attribute__((unused));
static pio_hw_t _pio1_inst __attribute__((unused));
#define pio0 (&_pio0_inst)
#define pio1 (&_pio1_inst)

// g_stub_last_clkdiv — lets tests inspect which clkdiv da_output.c used.
// Defined once in run_tests.c; the static-inline below writes to it.
extern float g_stub_last_clkdiv;

// g_stub_pio_put_count / g_stub_pio_fifo_full_after — PIO TX FIFO simulation.
// Set fifo_full_after to N to make pio_sm_is_tx_fifo_full() return true once
// N words have been pushed (N==-1 means never full, the default).
// Each defining TU provides static definitions; only TUs that call the inline
// functions below generate references, so linkage stays self-contained.
extern int g_stub_pio_put_count;
extern int g_stub_pio_fifo_full_after;

static inline uint pio_add_program(PIO p, const pio_program_t *prog)
    { (void)p; (void)prog; return 0; }
static inline void pio_sm_set_clkdiv(PIO p, uint sm, float div)
    { (void)p; (void)sm; g_stub_last_clkdiv = div; }
static inline void pio_sm_set_enabled(PIO p, uint sm, bool en)
    { (void)p; (void)sm; (void)en; }
static inline bool pio_sm_is_tx_fifo_full(PIO p, uint sm)
    { (void)p; (void)sm;
      return g_stub_pio_fifo_full_after >= 0 &&
             g_stub_pio_put_count >= g_stub_pio_fifo_full_after; }
static inline void pio_sm_put(PIO p, uint sm, uint32_t data)
    { (void)p; (void)sm; (void)data; g_stub_pio_put_count++; }
static inline bool pio_sm_is_rx_fifo_empty(PIO p, uint sm)
    { (void)p; (void)sm; return true; }
static inline uint32_t pio_sm_get_blocking(PIO p, uint sm)
    { (void)p; (void)sm; return 0; }
static inline bool pio_interrupt_get(PIO p, uint n)
    { (void)p; (void)n; return false; }
static inline void pio_interrupt_clear(PIO p, uint n)
    { (void)p; (void)n; }
