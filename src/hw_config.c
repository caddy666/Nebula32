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
// GPIO assignments (Core2350B0):
//   GPIO 30 — SDIO_CLK  (D0−2 = 32−2; required by SDIO PIO mod-32 offset)
//   GPIO 31 — SDIO_CMD  (D0−1)
//   GPIO 32 — SDIO_D0
//   GPIO 33 — SDIO_D1   (auto: D0+1)
//   GPIO 34 — SDIO_D2   (auto: D0+2)
//   GPIO 35 — SDIO_D3   (auto: D0+3)
//
// Moved from GPIO 18-23 to free those pins: GPIO 23-25,29 are now WiFi RM2.
// These match the defines in CMakeLists.txt.  If you change the wiring,
// update both CMakeLists.txt (for application code) and this file (for the
// library initialisation).
// =============================================================================

#include "hw_config.h"   // Provided by no-OS-FatFS library
#include "ff.h"          // for PARTITION typedef (required when FF_MULTI_PARTITION=1)

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
    .CLK_gpio  = 30,            // SDIO clock  (D0−2; SDIO PIO mod-32 constraint)
    .CMD_gpio  = 31,            // SDIO command (D0−1)
    .D0_gpio   = 32,            // SDIO data 0
    .D1_gpio   = 33,            // SDIO data 1 (must be D0+1)
    .D2_gpio   = 34,            // SDIO data 2 (must be D0+2)
    .D3_gpio   = 35,            // SDIO data 3 (must be D0+3)
    .SDIO_PIO  = pio1,          // PIO1 SM2/SM3 (SM0/SM1 used by COMMO on GPIO 44-46)
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
// Partition mapping (required when FF_MULTI_PARTITION=1 in ffconf.h)
// ---------------------------------------------------------------------------
// "0:/" → physical drive 0, partition 1  (disc images, config, cover art)
// "1:/" → physical drive 0, partition 2  (virtual CD-ROM content, max 650 MB)
PARTITION VolToPart[] = {
    {0, 1},
    {0, 2},
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
