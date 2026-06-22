// =============================================================================
// psram.c — QSPI PSRAM driver + bump allocator (BUILD_WITH_PSRAM only)
//
// Only compiled when -DBUILD_WITH_PSRAM=1 is set.
//
// HARDWARE SETUP (RP2350B):
//   - PSRAM chip on QMI CS1 (e.g. APS6404L-3SQR 8 MB).
//   - CS1 GPIO: set PSRAM_CS1_PIN below to whichever GPIO your PCB routes to
//     the PSRAM's /CE pin (must be a QMI-capable pin on the RP2350B package).
//   - Chip timing: adjust PSRAM_TIMING_* for your specific PSRAM chip and
//     the QSPI clock speed derived from your sys_clk.
//     Reference: RP2350 datasheet §4.10 (QMI), chip datasheet, and the
//     Pimoroni pico-plus-2 SDK (github.com/pimoroni/pimoroni-pico).
// =============================================================================
#ifdef BUILD_WITH_PSRAM

#include "psram.h"
#include "cd_types.h"     // CD32_SASSERT
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h" // __dmb()
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include <string.h>
#include <stdio.h>

// The bump allocator hands out 8-byte-aligned blocks measured from s_heap =
// PSRAM_BASE, so the base itself must be 8-byte aligned or every returned
// pointer is misaligned.  Caught at compile time rather than via a runtime fault.
CD32_SASSERT((PSRAM_BASE & 7U) == 0U, "PSRAM_BASE must be 8-byte aligned");

// ---------------------------------------------------------------------------
// Hardware configuration — EDIT FOR YOUR PCB
// ---------------------------------------------------------------------------

// GPIO connected to PSRAM /CE (chip enable).
// On RP2350B 80-pin QFN the QMI CS1n function is available on GPIO 47.
// Verify your PCB schematic and adjust.
#ifndef PSRAM_CS1_PIN
#define PSRAM_CS1_PIN  47
#endif

// QSPI clock divider applied to sys_clk for the PSRAM interface.
// clkdiv=2 → 135 MHz / 2 = 67.5 MHz QSPI — within APS6404L-3SQR spec (max 84 MHz).
// Increase to 3 or 4 if you see read errors; decrease only if your chip allows it.
#ifndef PSRAM_CLKDIV
#define PSRAM_CLKDIV  2
#endif

// RX sample delay in half-cycles.  Usually 2–4 at ≤80 MHz for APS6404L.
#ifndef PSRAM_RXDELAY
#define PSRAM_RXDELAY  2
#endif

// ---------------------------------------------------------------------------
// Allocator state
// ---------------------------------------------------------------------------

#define PSRAM_ALLOC_MAGIC  0xCA5CA5CAu

typedef struct {
    uint32_t magic;
    uint32_t size;   // usable bytes after header (8-byte aligned)
} psram_block_t;

static uint8_t  *s_heap      = NULL;
static uint32_t  s_heap_used = 0;
static uint32_t  s_heap_size = 0;
static bool      s_available = false;

// ---------------------------------------------------------------------------
// Hardware initialisation
// ---------------------------------------------------------------------------

static void _qmi_wait_ready(void) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
}

static void _qmi_cs1_cmd(uint8_t cmd) {
    qmi_hw->direct_csr = (1U << QMI_DIRECT_CSR_EN_LSB)
                       | (1U << QMI_DIRECT_CSR_ASSERT_CS1N_LSB);
    qmi_hw->direct_tx  = (QMI_DIRECT_TX_NOPUSH_BITS)
                       | (QMI_DIRECT_TX_OE_BITS)
                       | cmd;
    _qmi_wait_ready();
    qmi_hw->direct_csr = 0;
    _qmi_wait_ready();
}

void psram_fw_init(void) {
    // 1. Assign GPIO to XIP_CS1 function
    gpio_set_function(PSRAM_CS1_PIN, GPIO_FUNC_XIP_CS1);

    // 2. Enter direct-mode to issue reset commands to the PSRAM chip
    qmi_hw->direct_csr = QMI_DIRECT_CSR_EN_BITS;
    _qmi_wait_ready();

    // Reset enable + reset (standard SPI; PSRAM ignores unknown cmds so safe)
    _qmi_cs1_cmd(0x66);  // Reset Enable
    _qmi_cs1_cmd(0x99);  // Reset Device

    // 3. Leave direct mode
    qmi_hw->direct_csr = 0;

    // 4. Configure M1 (CS1) for Quad I/O access
    //    Timing for APS6404L-3SQR @ 67.5 MHz (adjust for your chip)
    qmi_hw->m[1].timing =
        (QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB) |
        (3U << QMI_M0_TIMING_SELECT_HOLD_LSB)    |
        (1U << QMI_M0_TIMING_COOLDOWN_LSB)        |
        ((uint32_t)PSRAM_RXDELAY << QMI_M0_TIMING_RXDELAY_LSB) |
        (18U << QMI_M0_TIMING_MAX_SELECT_LSB)     |
        (7U  << QMI_M0_TIMING_MIN_DESELECT_LSB)   |
        ((uint32_t)PSRAM_CLKDIV << QMI_M0_TIMING_CLKDIV_LSB);

    // Quad-SPI read: command EB, 24-bit addr, 6 dummy, quad data
    qmi_hw->m[1].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
        (6U  << QMI_M0_RFMT_DUMMY_LEN_LSB)                                  |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB);

    qmi_hw->m[1].rcmd = 0xEB;  // Fast Read Quad I/O

    // Quad-SPI write: command 38, 24-bit addr, quad data
    qmi_hw->m[1].wfmt =
        (QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB)   |
        (QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB);

    qmi_hw->m[1].wcmd = 0x38;  // Quad Write

    // 5. Verify the chip actually responds before trusting it.
    //    The PSRAM is now memory-mapped at PSRAM_BASE.  A wrong PSRAM_RXDELAY or
    //    PSRAM_CLKDIV produces garbage reads that would silently corrupt the
    //    sector cache (its slots now live in PSRAM).  We write four patterns to
    //    two well-separated addresses — catching stuck data lines, stuck address
    //    lines, and gross timing failures — then restore the originals.  On any
    //    mismatch we leave s_available = false so sector_cache_init() cleanly
    //    falls back to its SRAM slots instead of running on dead memory.
    volatile uint32_t *p0 = (volatile uint32_t *)PSRAM_BASE;
    volatile uint32_t *p1 = (volatile uint32_t *)(PSRAM_BASE + (PSRAM_SIZE_BYTES / 2U));
    static const uint32_t pat[2] = { 0xA5C30F69U, 0x5A3CF096U };

    const uint32_t saved0 = p0[0];
    const uint32_t saved1 = p1[0];
    bool ok = true;
    for (unsigned i = 0; i < 2U && ok; i++) {
        p0[0] = pat[i];
        p1[0] = ~pat[i];          // distinct value → also catches p0/p1 aliasing
        __dmb();                   // ensure writes drain before read-back
        if (p0[0] != pat[i] || p1[0] != ~pat[i]) ok = false;
    }
    p0[0] = saved0;               // restore (region may hold data on warm reset)
    p1[0] = saved1;

    if (!ok) {
        s_heap      = NULL;
        s_heap_size = 0;
        s_heap_used = 0;
        s_available = false;
        printf("[PSRAM] probe FAILED — disabling PSRAM (check RXDELAY=%d CLKDIV=%d)\n",
               PSRAM_RXDELAY, PSRAM_CLKDIV);
        return;
    }

    // 6. Initialise the bump allocator over the full PSRAM region
    s_heap       = (uint8_t *)PSRAM_BASE;
    s_heap_size  = PSRAM_SIZE_BYTES;
    s_heap_used  = 0;
    s_available  = true;

    printf("[PSRAM] %u MB at 0x%08lx (CS1 GPIO%d, clkdiv=%d) — probe OK\n",
           PSRAM_SIZE_BYTES / (1024U * 1024U),
           (unsigned long)PSRAM_BASE, PSRAM_CS1_PIN, PSRAM_CLKDIV);
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

bool psram_available(void) { return s_available; }

void *psram_alloc(size_t n) {
    if (!s_available || n == 0) return NULL;
    n = (n + 7U) & ~7U;  // 8-byte align
    uint32_t total = (uint32_t)(sizeof(psram_block_t) + n);
    if (s_heap_used + total > s_heap_size) {
        printf("[PSRAM] alloc exhausted (requested %u, used %lu / %lu)\n",
               (unsigned)n, (unsigned long)s_heap_used, (unsigned long)s_heap_size);
        return NULL;
    }
    psram_block_t *hdr = (psram_block_t *)(s_heap + s_heap_used);
    hdr->magic = PSRAM_ALLOC_MAGIC;
    hdr->size  = (uint32_t)n;
    s_heap_used += total;
    memset(hdr + 1, 0, n);
    return hdr + 1;
}

void psram_free(void *p) {
    if (!p) return;
    psram_block_t *hdr = (psram_block_t *)p - 1;
    if (hdr->magic != PSRAM_ALLOC_MAGIC) return;
    hdr->magic = 0;  // mark freed; bump allocator does not reclaim
}


#endif // BUILD_WITH_PSRAM
