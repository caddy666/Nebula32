// =============================================================================
// display.cpp — ST7789 240x240 cover-art display
// =============================================================================
// Implements a minimal ST7789 SPI driver using the Pimoroni initialisation
// sequence, then streams JPEG cover art from the SD card tile-by-tile via
// JPEGDEC, writing each MCU block directly to the display window.
//
// No full PicoGraphics framebuffer is needed: the MCU blocks from JPEGDEC
// (~16×16 pixels each = 512 bytes max) go straight to the SPI FIFO.
// This keeps peak RAM usage under 2 KB for the display path.

// Tell JPEGDEC to use <stdlib.h> / <stdint.h> instead of <Arduino.h>
#define __LINUX__

extern "C" {
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "ff.h"           // FatFS for JPEG file access
#include "display.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
}

#include "JPEGDEC.h"

// ---------------------------------------------------------------------------
// Pin assignments (compile-time constants from CMakeLists.txt)
// ---------------------------------------------------------------------------
#ifndef ST7789_DC_PIN
#define ST7789_DC_PIN   24
#endif
#ifndef ST7789_CS_PIN
#define ST7789_CS_PIN   13
#endif
#ifndef ST7789_SCK_PIN
#define ST7789_SCK_PIN  26
#endif
#ifndef ST7789_MOSI_PIN
#define ST7789_MOSI_PIN 27
#endif

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240

// ---------------------------------------------------------------------------
// ST7789 register addresses
// ---------------------------------------------------------------------------
#define ST_SWRESET  0x01
#define ST_TEON     0x35
#define ST_COLMOD   0x3A
#define ST_PORCTRL  0xB2
#define ST_GCTRL    0xB7
#define ST_RAMCTRL  0xB0
#define ST_VCOMS    0xBB
#define ST_LCMCTRL  0xC0
#define ST_VDVVRHEN 0xC2
#define ST_VRHS     0xC3
#define ST_VDVS     0xC4
#define ST_FRCTRL2  0xC6
#define ST_PWCTRL1  0xD0
#define ST_GMCTRP1  0xE0
#define ST_GMCTRN1  0xE1
#define ST_INVON    0x21
#define ST_SLPOUT   0x11
#define ST_DISPON   0x29
#define ST_MADCTL   0x36
#define ST_CASET    0x2A
#define ST_RASET    0x2B
#define ST_RAMWR    0x2C

// ---------------------------------------------------------------------------
// Low-level SPI helpers (CS managed by caller)
// ---------------------------------------------------------------------------

static inline void cs_lo(void) { gpio_put(ST7789_CS_PIN, 0); }
static inline void cs_hi(void) { gpio_put(ST7789_CS_PIN, 1); }
static inline void dc_lo(void) { gpio_put(ST7789_DC_PIN, 0); }
static inline void dc_hi(void) { gpio_put(ST7789_DC_PIN, 1); }

static void st_cmd(uint8_t c) {
    cs_lo(); dc_lo();
    spi_write_blocking(spi1, &c, 1);
    cs_hi();
}

static void st_cmd_d(uint8_t c, const uint8_t *d, size_t n) {
    cs_lo();
    dc_lo();
    spi_write_blocking(spi1, &c, 1);
    if (n && d) {
        dc_hi();
        spi_write_blocking(spi1, d, n);
    }
    cs_hi();
}

// ---------------------------------------------------------------------------
// ST7789 initialisation (Pimoroni sequence for 240×240 non-round panel)
// ---------------------------------------------------------------------------
static void st7789_init_display(void) {
    // Software reset
    st_cmd(ST_SWRESET);
    sleep_ms(150);

    st_cmd_d(ST_TEON,     (const uint8_t *)"\x00", 1);
    st_cmd_d(ST_COLMOD,   (const uint8_t *)"\x05", 1);  // 16 bpp RGB565
    st_cmd_d(ST_PORCTRL,  (const uint8_t *)"\x0c\x0c\x00\x33\x33", 5);
    st_cmd_d(ST_LCMCTRL,  (const uint8_t *)"\x2c", 1);
    st_cmd_d(ST_VDVVRHEN, (const uint8_t *)"\x01", 1);
    st_cmd_d(ST_VRHS,     (const uint8_t *)"\x12", 1);
    st_cmd_d(ST_VDVS,     (const uint8_t *)"\x20", 1);
    st_cmd_d(ST_PWCTRL1,  (const uint8_t *)"\xa4\xa1", 2);
    st_cmd_d(ST_FRCTRL2,  (const uint8_t *)"\x0f", 1);
    st_cmd_d(ST_GCTRL,    (const uint8_t *)"\x14", 1);
    st_cmd_d(ST_VCOMS,    (const uint8_t *)"\x37", 1);
    st_cmd_d(ST_GMCTRP1,  (const uint8_t *)
             "\xD0\x04\x0D\x11\x13\x2B\x3F\x54\x4C\x18\x0D\x0B\x1F\x23", 14);
    st_cmd_d(ST_GMCTRN1,  (const uint8_t *)
             "\xD0\x04\x0C\x11\x13\x2C\x3F\x44\x51\x2F\x1F\x1F\x20\x23", 14);
    // RAMCTRL: byte-swap enable so we can write LE RGB565 directly
    st_cmd_d(ST_RAMCTRL,  (const uint8_t *)"\x00\xc0", 2);
    st_cmd(ST_INVON);
    st_cmd(ST_SLPOUT);
    sleep_ms(50);
    st_cmd(ST_DISPON);
    sleep_ms(50);
    // MADCTL = 0 → ROTATE_0, top-left origin
    st_cmd_d(ST_MADCTL,   (const uint8_t *)"\x00", 1);
    // Set full window
    uint8_t caset[4] = { 0, 0, 0, (uint8_t)(DISPLAY_WIDTH  - 1) };
    uint8_t raset[4] = { 0, 0, 0, (uint8_t)(DISPLAY_HEIGHT - 1) };
    st_cmd_d(ST_CASET, caset, 4);
    st_cmd_d(ST_RASET, raset, 4);
}

// ---------------------------------------------------------------------------
// Write a rectangle of LE-RGB565 pixels to the display
// ---------------------------------------------------------------------------
static void st7789_write_block(int x, int y, int w, int h, const uint16_t *pixels) {
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || w <= 0 || h <= 0) return;
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH  - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;

    uint8_t d[4];

    d[0] = (uint8_t)(x >> 8);         d[1] = (uint8_t)x;
    d[2] = (uint8_t)((x+w-1) >> 8);   d[3] = (uint8_t)(x+w-1);
    st_cmd_d(ST_CASET, d, 4);

    d[0] = (uint8_t)(y >> 8);         d[1] = (uint8_t)y;
    d[2] = (uint8_t)((y+h-1) >> 8);   d[3] = (uint8_t)(y+h-1);
    st_cmd_d(ST_RASET, d, 4);

    cs_lo();
    dc_lo();
    uint8_t cmd = ST_RAMWR;
    spi_write_blocking(spi1, &cmd, 1);
    dc_hi();
    spi_write_blocking(spi1, (const uint8_t *)pixels, (size_t)(w * h * 2));
    cs_hi();
}

// ---------------------------------------------------------------------------
// Fill the entire display with a single RGB565 colour
// ---------------------------------------------------------------------------
static void st7789_fill(uint16_t colour_le) {
    uint8_t d[4];

    d[0] = 0; d[1] = 0;
    d[2] = 0; d[3] = (uint8_t)(DISPLAY_WIDTH - 1);
    st_cmd_d(ST_CASET, d, 4);

    d[0] = 0; d[1] = 0;
    d[2] = 0; d[3] = (uint8_t)(DISPLAY_HEIGHT - 1);
    st_cmd_d(ST_RASET, d, 4);

    // Fill in 16-pixel row bursts to limit stack usage
    static uint16_t row[DISPLAY_WIDTH];
    for (int i = 0; i < DISPLAY_WIDTH; i++) row[i] = colour_le;

    cs_lo();
    dc_lo();
    uint8_t cmd = ST_RAMWR;
    spi_write_blocking(spi1, &cmd, 1);
    dc_hi();
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        spi_write_blocking(spi1, (const uint8_t *)row, DISPLAY_WIDTH * 2);
    }
    cs_hi();
}

// ---------------------------------------------------------------------------
// Public scanline API (used by visualiser)
// ---------------------------------------------------------------------------

void display_hline(uint16_t y, const uint16_t *pixels) {
    st7789_write_block(0, (int)y, DISPLAY_WIDTH, 1, pixels);
}

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour) {
    if (w == 0 || h == 0) return;
    // Reuse the row buffer from st7789_fill's static; allocate on stack (small rects)
    uint16_t row[DISPLAY_WIDTH];
    if (w > DISPLAY_WIDTH) w = DISPLAY_WIDTH;
    for (uint16_t i = 0; i < w; i++) row[i] = colour;
    for (uint16_t r = 0; r < h; r++) {
        st7789_write_block((int)x, (int)(y + r), (int)w, 1, row);
    }
}

// ---------------------------------------------------------------------------
// JPEGDEC: FatFS I/O callbacks
// ---------------------------------------------------------------------------

static void *jpeg_open(const char *path, int32_t *size_out) {
    FIL *fp = (FIL *)malloc(sizeof(FIL));
    if (!fp) return nullptr;
    if (f_open(fp, path, FA_READ) != FR_OK) {
        free(fp);
        return nullptr;
    }
    *size_out = (int32_t)f_size(fp);
    return fp;
}

static void jpeg_close(void *handle) {
    if (handle) {
        f_close((FIL *)handle);
        free(handle);
    }
}

static int32_t jpeg_read(JPEGFILE *pf, uint8_t *buf, int32_t len) {
    UINT br = 0;
    f_read((FIL *)pf->fHandle, buf, (UINT)len, &br);
    return (int32_t)br;
}

static int32_t jpeg_seek(JPEGFILE *pf, int32_t pos) {
    FRESULT r = f_lseek((FIL *)pf->fHandle, (FSIZE_t)pos);
    return (r == FR_OK) ? pos : -1;
}

// ---------------------------------------------------------------------------
// JPEGDEC draw callback — streams each MCU block directly to the display
// ---------------------------------------------------------------------------
static int jpeg_draw(JPEGDRAW *draw) {
    st7789_write_block(draw->x, draw->y,
                       draw->iWidthUsed ? draw->iWidthUsed : draw->iWidth,
                       draw->iHeight,
                       draw->pPixels);
    return 1;  // 1 = continue decoding
}

// ---------------------------------------------------------------------------
// JPEGDEC instance (static, not re-entrant but we're single-threaded here)
// ---------------------------------------------------------------------------
static JPEGDEC s_jpeg;

// ---------------------------------------------------------------------------
// Cover-art path resolution
// Replace anything that isn't alphanumeric / period / underscore with '-'
// so "Chaos Engine.iso" → "Chaos-Engine" matches "Chaos-Engine.jpg".
// ---------------------------------------------------------------------------
static void sanitise_name(char *buf, const char *name, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && name[i]; i++) {
        char c = name[i];
        buf[i] = (c == ' ') ? '-' : c;
    }
    buf[i] = '\0';
}

static bool find_cover_jpeg(const char *disc_path, char *out, size_t out_len) {
    // Strip leading path component
    const char *slash = strrchr(disc_path, '/');
    const char *base = slash ? slash + 1 : disc_path;

    // Strip extension
    char name[128];
    const char *dot = strrchr(base, '.');
    size_t name_len = dot ? (size_t)(dot - base) : strlen(base);
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
    memcpy(name, base, name_len);
    name[name_len] = '\0';

    // Try exact match: 0:/covers/<name>.jpg
    snprintf(out, out_len, "0:/covers/%s.jpg", name);
    FILINFO fi;
    if (f_stat(out, &fi) == FR_OK) return true;

    // Try with spaces → hyphens
    char sanitised[128];
    sanitise_name(sanitised, name, sizeof(sanitised));
    if (strcmp(sanitised, name) != 0) {
        snprintf(out, out_len, "0:/covers/%s.jpg", sanitised);
        if (f_stat(out, &fi) == FR_OK) return true;
    }

    out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" void display_init(void) {
    printf("[DISP] Init ST7789 240x240 (SPI1 SCK=GPIO%d MOSI=GPIO%d DC=GPIO%d CS=GPIO%d)\n",
           ST7789_SCK_PIN, ST7789_MOSI_PIN, ST7789_DC_PIN, ST7789_CS_PIN);

    // Configure GPIO for DC and CS as regular outputs
    gpio_init(ST7789_DC_PIN);
    gpio_set_dir(ST7789_DC_PIN, GPIO_OUT);
    gpio_put(ST7789_DC_PIN, 1);

    gpio_init(ST7789_CS_PIN);
    gpio_set_dir(ST7789_CS_PIN, GPIO_OUT);
    gpio_put(ST7789_CS_PIN, 1);  // deselected

    // Configure SPI1 pins and start at 62.5 MHz (75 MHz on RP2350 — see Pimoroni)
    gpio_set_function(ST7789_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(ST7789_MOSI_PIN, GPIO_FUNC_SPI);
    spi_init(spi1, 62500000);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    st7789_init_display();
    st7789_fill(0x0000);  // black

    printf("[DISP] Ready\n");
    LOG_INFO_MSG("DISP", "ST7789 ready");
}

#define DEFAULT_COVER_PATH  "0:/covers/cd32-default.jpg"

// Decode and render a JPEG at the given FatFS path. Returns true on success.
static bool show_jpeg(const char *jpeg_path) {
    if (!s_jpeg.open(jpeg_path, jpeg_open, jpeg_close, jpeg_read, jpeg_seek, jpeg_draw)) {
        printf("[DISP] JPEGDEC open failed: %s\n", jpeg_path);
        return false;
    }

    s_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

    int w = s_jpeg.getWidth();
    int h = s_jpeg.getHeight();
    printf("[DISP] JPEG %dx%d → 240x240\n", w, h);

    int options = 0;
    if (w >= 480 && h >= 480) options = JPEG_SCALE_HALF;

    st7789_fill(0x0000);

    int rc = s_jpeg.decode(0, 0, options);
    s_jpeg.close();

    if (rc == 0) {
        printf("[DISP] JPEGDEC decode error: %s\n", jpeg_path);
        return false;
    }
    return true;
}

static void show_default(void) {
    if (!show_jpeg(DEFAULT_COVER_PATH)) {
        st7789_fill(0x0000);  // black fallback if default image is missing
    }
}

extern "C" void display_clear(void) {
    show_default();
}

extern "C" void display_show_text(const char *line1, const char *line2) {
    (void)line1; (void)line2;
    show_default();
    printf("[DISP] show_text: \"%s\" / \"%s\"\n",
           line1 ? line1 : "", line2 ? line2 : "");
}

extern "C" void display_show_cover(const char *disc_image_path) {
    if (!disc_image_path || disc_image_path[0] == '\0') {
        show_default();
        return;
    }

    char jpeg_path[MAX_PATH_LEN];
    if (!find_cover_jpeg(disc_image_path, jpeg_path, sizeof(jpeg_path))) {
        printf("[DISP] No cover art for: %s — showing default\n", disc_image_path);
        show_default();
        return;
    }

    printf("[DISP] Loading cover: %s\n", jpeg_path);
    if (!show_jpeg(jpeg_path)) {
        show_default();
    }
}
