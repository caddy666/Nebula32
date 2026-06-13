#pragma once
// =============================================================================
// display.h — ST7789 240x240 cover-art display
// =============================================================================
// Drives a 240x240 ST7789 over hardware SPI1.
// Cover art JPEGs are decoded with JPEGDEC and streamed tile-by-tile.
//
// WIRING (Core2350B0 — see gpio_map.h):
//   GPIO 40 → ST7789 DC   (data/command select)
//   GPIO 41 → ST7789 CS   (chip select, active low)
//   GPIO 42 → ST7789 SCK  (SPI1 clock, up to 75 MHz on RP2350)
//   GPIO 43 → ST7789 DIN  (SPI1 MOSI)
//   ST7789 RST → 3.3 V    (no software reset needed)
//   ST7789 BL  → 3.3 V    (backlight always on; add PWM later if needed)
//
// JPEG LOOKUP:
//   Given disc path  "/Superfrog.iso"
//   Looks for cover  "0:/covers/Superfrog.jpg"
//   Also tries spaces→hyphens variant.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must be called once after I2C and SD card are ready.
void display_init(void);

// Decode and show cover art for the given disc image path.
// Searches the SD card's /covers/ directory for a matching JPEG.
// Falls back to a plain title card if no JPEG is found.
void display_show_cover(const char *disc_image_path);

// Fill the screen with a solid colour and optionally show a short text label.
void display_show_text(const char *line1, const char *line2);

// Clear the display to black.
void display_clear(void);

// ---------------------------------------------------------------------------
// Visualiser scanline API
// ---------------------------------------------------------------------------

// Write DISPLAY_WIDTH LE-RGB565 pixels to a single horizontal row y.
void display_hline(uint16_t y, const uint16_t *pixels);

// Fill a rectangle with a single LE-RGB565 colour.
void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour);

// ---------------------------------------------------------------------------
// Firmware update display
// ---------------------------------------------------------------------------

// Render a firmware-update progress overlay (pct 0–100).
// On the first call (pct == 0) draws the full black background and chrome.
// Subsequent calls only update the progress bar — safe to call between
// individual flash_range_program calls while Core 1 is locked out.
void display_fw_progress(uint8_t pct);

// ~2-second Boing Ball celebration animation played on the first boot after a
// successful firmware update (detected via watchdog scratch register in main.c).
void display_fw_success_animation(void);

#ifdef __cplusplus
}
#endif
