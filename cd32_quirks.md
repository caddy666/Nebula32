# CD32 ODE — Known Quirks and Spec Deviations

This document records intentional deviations from the CD-ROM specification,
hardware-specific oddities, and known limitations that are not bugs but that
a future developer might mistake for one.

---

## Quirks of the CD32 and Its Hardware

These are properties of the actual CD32 console, the CXD2545Q DSP, or the
original 8051 drive firmware that the ODE must replicate faithfully.

### DA Frame Width: 48 Bits, Not 32

**Files:** `pio/da_output.pio`, `src/da_output.c`

The CXD2545Q transmits I2S frames of **48 bits** (24-bit left + 24-bit right
per LRCLK period), confirmed by 100M-row logic-analyser capture. The LRCLK/BCLK
half-period ratio is 48, not the 32 (16+16) that standard I2S implies.

The PIO program clocks 96 BCLK edges per LRCLK period (48 per half), and the
DMA buffer packs 24-bit samples with the lower 24 bits of each 32-bit word
zero. `expand_to_i2s24()` in `da_output.c` handles this expansion.

Most I2S documentation and tooling assumes 32-bit frames (16+16). The CXD2545Q
is unusual in sending 24+24.

---

### SCOR Counter Pre-Decrement

**File:** `upstream/drivers/driver.c`, `init_scor_counter()`

```c
void init_scor_counter(uint8_t count)
{
    scor_counter = count;
    if (scor_counter > 0) scor_counter--;
}
```

The counter is decremented immediately after being set. This is intentional:
the original 8051 firmware decremented the counter in the SCOR interrupt
handler, and one interrupt had already fired by the time the counter was
initialised. The pre-decrement reproduces that off-by-one timing offset.
Removing it shifts seek timing by one subcode frame (~13.3 ms).

---

### `delay()` Blocks Instead of Yielding

**File:** `upstream/utils/timer.c`

```c
void delay(void) {
    if (!delay_byte) return;
    do { sleep_us(500); } while (--delay_byte);
}
```

The original 8051 `delay()` decremented `delay_byte` in a hardware timer ISR
running at 2 kHz (every 500 µs), allowing other ISR work to interleave between
counts. The Pico replacement calls `sleep_us(500)` directly, blocking the
calling core for the full duration. The per-count granularity is identical, and
no code path sets `delay_byte` to more than a few counts (under 2 ms total), so
the difference is not observable.

---

### Player Wrapper Pattern

**File:** `upstream/core/player.c`

The upstream player uses static C functions that forward to external symbols
of the same name:

```c
static uint8_t servo_to_service(void) {
    extern uint8_t servo_to_service(void);
    return servo_to_service();
}
```

This was the original 8051 firmware's way of providing a private calling
convention inside a translation unit while still resolving to the linker's
public symbol. The linker handles it correctly on ARM; the `static` wrapper is
never called through the external linkage.

---

## Quirks of the ODE System

These are implementation choices or known limitations specific to the ODE
firmware running on the Pico 2.

### ECC Q-Parity Interleave Is Linearised

**File:** `src/ecc.c`, `ecc_generate()`, lines ~174–184

**What the spec says (ECMA-130 Annex C):**
Q-parity codewords use a diagonal interleave across the 43×52 data+P matrix.
Each of the 52 Q codewords covers 43 bytes sampled along a diagonal stride, not
a contiguous linear slice.

**What the code does:**
```c
const uint8_t *base = sector + 12 + j * 43;
rs_q_encode(base, 1, 41, &q0, &q1);  // stride=1: linear read
```

The Q codewords are computed over sequential bytes, not the diagonal pattern.
The 104 Q-parity bytes written to sector bytes 2248–2351 are therefore wrong
relative to ECMA-130.

**Why it doesn't matter in practice:**
The CXD2545Q DSP handles error correction transparently before Akiko sees any
data. Akiko and the 68EC020 never verify Q-parity. Games that read raw sectors
via READS check EDC (bytes 2064–2067), which is computed correctly. Q-parity
would only matter if the ODE needed to emulate correctable read errors, which
it does not.

**If this ever needs to be fixed:**
Implement the full ECMA-130 Annex C diagonal interleave. The Q data source
for codeword `j` is `sector[12 + (j*43 + k*44) % 2236]` for k = 0..40,
where indices wrap around the 43×52 virtual matrix spanning the header, user
data, EDC, zeroes, and P-parity regions.

---

### COMMO PIO: Both State Machines on PIO1

**File:** `upstream/hal/pio_hw.c`, `upstream/include/pio_hw.h`

```c
#define PIO_COMMO_RX  pio1   // COMMO receive  — PIO1 SM0
#define PIO_COMMO_TX  pio1   // COMMO transmit — PIO1 SM1
```

Both the RX and TX COMMO state machines live on PIO1. The distinct names
`PIO_COMMO_RX` / `PIO_COMMO_TX` exist only for readability. In
`pio_commo_tx_byte()` the first argument to `commo_tx_send()` is
`PIO_COMMO_RX` rather than `PIO_COMMO_TX`; since both expand to `pio1` this
is harmless, but it looks like a copy-paste oversight.

---

### `subcode_build_q_isrc()` Ignores `track_no`

**File:** `src/subcode.c`

The function signature accepts a `track_no` parameter inherited from the
original (broken) implementation, which incorrectly wrote a BCD track number
into byte 9. Per ECMA-130 Table 17, Mode 3 (ISRC) byte 9 carries the tail bits
of the 12-character ISRC code, not the track number. The parameter is kept for
API compatibility but is unused (`(void)track_no`).

---

### EDC and GF Tables Live in SRAM, Not Flash

**File:** `src/ecc.c`, `edc_table[256]`, `gf_exp[512]`, `gf_log[256]`

All three lookup tables are initialised at runtime on first call rather than
stored as `const` arrays in flash. Total SRAM cost is about 1.75 KiB. The
one-time init cost is negligible. If flash space ever becomes constrained, all
three could be pre-computed and stored as `const` arrays.

---

### Sector Cache Always Synthesises Full 2352-Byte Sectors

**File:** `src/sector_cache.c`, `sector_cache_prefetch_tick()`

The cache always fetches full 2352-byte raw sectors, even for ISO images whose
native payload is 2048 bytes. ISO sectors are synthesised via
`disc_synthesise_sector()` + `ecc_sector_complete()` at prefetch time on Core 1,
not at delivery time on Core 0. This keeps the DMA ISR simple at the cost of
doing EDC/ECC work unconditionally, including for sectors the host may never
request in raw form.
