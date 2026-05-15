// =============================================================================
// hw_config.c — SD card hardware configuration for no-OS-FatFS library
// =============================================================================
//
// The no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library (Carl Kugler) requires exactly
// one hw_config.c file in the application that defines the hardware layout.
// This file supersedes the equivalent in sd_card.c; if you get linker errors
// about duplicate sd_get_num() / sd_get_by_num(), remove those definitions
// from sd_card.c and keep only this file.
//
// We configure a single SD card on a 4-bit SDIO bus at 25 MHz.
// The SDIO pins must be consecutive (D0..D3) — a hardware requirement of the
// RP2350 SDIO PIO program in the library.
//
// GPIO assignments:
//   GPIO 18 — SDIO_CLK
//   GPIO 19 — SDIO_CMD
//   GPIO 20 — SDIO_D0
//   GPIO 21 — SDIO_D1
//   GPIO 22 — SDIO_D2
//   GPIO 23 — SDIO_D3
//
// These match the defines in CMakeLists.txt.  If you change the wiring,
// update both CMakeLists.txt (for application code) and this file (for the
// library initialisation).
// =============================================================================

#include "hw_config.h"   // Provided by no-OS-FatFS library

// ---------------------------------------------------------------------------
// SDIO bus descriptor
// ---------------------------------------------------------------------------
// sd_sdio_if_t fields:
//   CLK_gpio  — Clock pin
//   CMD_gpio  — Command / response pin
//   D0_gpio   — Data pin 0 (D1 = D0+1, D2 = D0+2, D3 = D0+3 automatically)
//   baud_rate — Maximum bus clock in Hz.  25 MHz is the SD "default speed".
//               Some cards and boards support 50 MHz (high-speed mode).
//               Reduce to 10 MHz if you see CRC errors during mount.
// ---------------------------------------------------------------------------
static sd_sdio_if_t sdio_if = {
    .CLK_gpio  = 18,            // SDIO clock
    .CMD_gpio  = 19,            // SDIO command
    .D0_gpio   = 20,            // SDIO data 0
    .D1_gpio   = 21,            // SDIO data 1 (must be D0+1)
    .D2_gpio   = 22,            // SDIO data 2 (must be D0+2)
    .D3_gpio   = 23,            // SDIO data 3 (must be D0+3)
    .SDIO_PIO  = pio1,          // Use PIO1 
    .DMA_IRQ_num = DMA_IRQ_1,   // Use DMA_IRQ_1 (DMA_IRQ_0 may be used by audio)
    .baud_rate = 25 * 1000 * 1000,  // 25 MHz initial speed
    // .baud_rate = 50 * 1000 * 1000,  // Uncomment for 50 MHz (SDHC/SDXC cards)
};

// ---------------------------------------------------------------------------
// SD card descriptor
// ---------------------------------------------------------------------------
// sd_card_t fields:
//   pcName     — FatFS volume label.  "0:" = drive 0.
//   type       — SD_IF_SDIO (4-bit SDIO) or SD_IF_SPI
//   sdio_if    — Pointer to the SDIO config above
//   use_card_detect — Set true and fill card_detect_gpio if your board has a
//                     card-detect switch.  Leave false for most modules.
// ---------------------------------------------------------------------------
static sd_card_t sd_cards[] = {
    {
        .type               = SD_IF_SDIO,
        .sdio_if_p          = &sdio_if,
        .use_card_detect    = false,
        // If your SD module has a card-detect pin, uncomment and configure:
        // .use_card_detect = true,
        // .card_detect_gpio = 17,
        // .card_detected_true = 0,
    },
};

// ---------------------------------------------------------------------------
// Library callbacks (must be defined exactly once)
// ---------------------------------------------------------------------------

// Returns the number of SD card objects
size_t sd_get_num(void) {
    return count_of(sd_cards);
}

// Returns a pointer to SD card descriptor by index
sd_card_t *sd_get_by_num(size_t num) {
    if (num < count_of(sd_cards)) {
        return &sd_cards[num];
    }
    return NULL;
}
