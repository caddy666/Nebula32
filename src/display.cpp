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
#include "hardware/dma.h"
#include "ff.h"           // FatFS for JPEG file access
#include "display.h"
#include "effects.h"
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
#define ST7789_DC_PIN   40
#endif
#ifndef ST7789_CS_PIN
#define ST7789_CS_PIN   41
#endif
#ifndef ST7789_SCK_PIN
#define ST7789_SCK_PIN  42
#endif
#ifndef ST7789_MOSI_PIN
#define ST7789_MOSI_PIN 43
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

// ---------------------------------------------------------------------------
// SPI TX DMA — async scanline pushes
// ---------------------------------------------------------------------------
// One bounce buffer holds the in-flight scanline so the caller (effects.c
// renders into its own line[] buffer) can start composing the next line while
// the previous one is still clocking out.  Every command write drains the DMA
// and the SPI FIFO first, so byte ordering on the wire is preserved.  CS must
// stay low until the drain completes — deasserting it mid-burst would abort
// the RAMWR write — so cs_hi() is deferred to _spi_dma_drain().

static int      s_spi_dma_ch = -1;
static uint16_t s_dma_bounce[DISPLAY_WIDTH];
static bool     s_dma_cs_pending = false;

static void _spi_dma_drain(void) {
    if (!s_dma_cs_pending) return;
    dma_channel_wait_for_finish_blocking(s_spi_dma_ch);
    while (spi_is_busy(spi1)) tight_loop_contents();
    cs_hi();
    s_dma_cs_pending = false;
}

static void st_cmd(uint8_t c) {
    _spi_dma_drain();
    cs_lo(); dc_lo();
    spi_write_blocking(spi1, &c, 1);
    cs_hi();
}

static void st_cmd_d(uint8_t c, const uint8_t *d, size_t n) {
    _spi_dma_drain();
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
    size_t n_bytes = (size_t)(w * h * 2);
    if (s_spi_dma_ch >= 0 && n_bytes <= sizeof(s_dma_bounce)) {
        // Async: bounce-copy then DMA — the caller may reuse 'pixels'
        // immediately.  cs_hi() is deferred to the next _spi_dma_drain(),
        // which the CASET write of the following block performs.
        memcpy(s_dma_bounce, pixels, n_bytes);
        dma_channel_set_read_addr(s_spi_dma_ch, s_dma_bounce, false);
        dma_channel_set_trans_count(s_spi_dma_ch, n_bytes, true);
        s_dma_cs_pending = true;
    } else {
        // Payload exceeds one scanline (JPEGDEC MCU blocks): blocking write —
        // JPEGDEC reuses its pixel buffer as soon as the draw callback returns,
        // so an async DMA read from it would race the decoder.
        spi_write_blocking(spi1, (const uint8_t *)pixels, n_bytes);
        cs_hi();
    }
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

    // SPI TX DMA channel for async scanline pushes (paced by the SPI TX DREQ)
    s_spi_dma_ch = dma_claim_unused_channel(true);
    dma_channel_config dcfg = dma_channel_get_default_config(s_spi_dma_ch);
    channel_config_set_transfer_data_size(&dcfg, DMA_SIZE_8);
    channel_config_set_read_increment(&dcfg, true);
    channel_config_set_write_increment(&dcfg, false);
    channel_config_set_dreq(&dcfg, spi_get_dreq(spi1, true));
    dma_channel_configure(s_spi_dma_ch, &dcfg,
                          &spi_get_hw(spi1)->dr,  // write: SPI1 data register
                          nullptr, 0, false);     // read addr/count set per push

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

// ---------------------------------------------------------------------------
// Firmware update progress overlay
// ---------------------------------------------------------------------------
// Renders a minimal progress UI using only direct SPI writes — safe to call
// while Core 1 is locked out and IRQs are cycling on/off between flash ops.
// ---------------------------------------------------------------------------

extern "C" void display_fw_progress(uint8_t pct) {
    static bool bg_drawn = false;

    if (!bg_drawn || pct == 0) {
        // Black background
        st7789_fill(rgb(0, 0, 0));

        // Amber header band (40 px tall)
        display_fill_rect(0, 20, DISPLAY_WIDTH, 40, rgb(220, 130, 0));

        // Bar track (dark grey, 200 × 20 px, centred)
        display_fill_rect(20, 140, 200, 20, rgb(35, 35, 35));

        bg_drawn = true;
    }

    // Progress bar fill — green proportional region, dark-grey remainder
    uint16_t fill_w = (uint16_t)(200u * (uint32_t)pct / 100u);
    if (fill_w > 0)
        display_fill_rect(20, 140, fill_w, 20, rgb(30, 200, 60));
    if (fill_w < 200)
        display_fill_rect((uint16_t)(20 + fill_w), 140,
                          (uint16_t)(200 - fill_w), 20, rgb(35, 35, 35));
}

// ---------------------------------------------------------------------------
// Firmware update success animation — Amiga Boing Ball (~2 s)
// ---------------------------------------------------------------------------
// Rendered entirely on Core 0 using direct SPI.  Uses atan2f/asinf with the
// RP2350 FPU for per-pixel spherical UV mapping of the checker pattern.
// ---------------------------------------------------------------------------

#include <math.h>

// Draw the grey grid background into the full framebuffer.
static void draw_grid_bg(void) {
    static uint16_t row[DISPLAY_WIDTH];
    uint16_t light = rgb(210, 210, 210);
    uint16_t dark  = rgb(140, 140, 140);
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        bool h_line = ((y % 24) == 0);
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            bool v_line = ((x % 24) == 0);
            row[x] = (h_line || v_line) ? dark : light;
        }
        display_hline((uint16_t)y, row);
    }
}

// Render the boing ball at pixel centre (cx, cy), radius r, rotation rot_t.
// Clears the ball's bounding box to background before drawing.
static void draw_boing_ball(int cx, int cy, int r, float rot_t) {
    static uint16_t row[DISPLAY_WIDTH];
    uint16_t light = rgb(210, 210, 210);
    uint16_t dark  = rgb(140, 140, 140);
    float rf = (float)r;

    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= DISPLAY_HEIGHT) continue;
        float dy = (float)(y - cy);
        float dx_maxf = sqrtf(rf * rf - dy * dy);
        int x0 = cx - (int)dx_maxf;
        int x1 = cx + (int)dx_maxf;
        if (x0 < 0) x0 = 0;
        if (x1 >= DISPLAY_WIDTH) x1 = DISPLAY_WIDTH - 1;

        // Fill the full row with background first (handles partial clipping)
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            bool h_line = ((y % 24) == 0);
            bool v_line = ((x % 24) == 0);
            row[x] = (h_line || v_line) ? dark : light;
        }

        // Overdraw the ball pixels
        for (int x = x0; x <= x1; x++) {
            float nx = (float)(x - cx) / rf;
            float ny = dy / rf;
            float nz_sq = 1.0f - nx * nx - ny * ny;
            if (nz_sq < 0.0f) continue;
            float nz = sqrtf(nz_sq);

            // Spherical UV: theta around equator, phi from south pole
            float theta = atan2f(ny, nx);            // -π..π
            float phi   = asinf(nz);                 //  0..π/2

            // 8 sectors around, 6 rows pole-to-pole; XOR gives checker
            int u = (int)((theta / (float)M_PI + 1.0f + rot_t) * 4.0f);
            int v = (int)(((phi + (float)(M_PI / 2)) / (float)M_PI) * 6.0f);
            bool red = ((u ^ v) & 1) != 0;

            // Lambert shading: nz=1 at highlight, nz=0 at silhouette
            float shade = nz * 0.75f + 0.25f;
            uint8_t rv = red ? (uint8_t)(205.0f * shade) : (uint8_t)(245.0f * shade);
            uint8_t gv = red ? (uint8_t)(25.0f  * shade) : (uint8_t)(245.0f * shade);
            uint8_t bv = red ? (uint8_t)(25.0f  * shade) : (uint8_t)(245.0f * shade);
            row[x] = rgb(rv, gv, bv);
        }

        display_hline((uint16_t)y, row);
    }
}

extern "C" void display_fw_success_animation(void) {
    const int BALL_R   = 52;
    const int FRAMES   = 45;   // ~3 s at ~15 fps
    const float ROT_STEP = 0.12f;

    // Draw background once
    draw_grid_bg();

    // Ball starts slightly off-centre; bounces off walls
    int bx = 90, by = 90;
    int vx =  5, vy =  4;
    float rot = 0.0f;

    for (int f = 0; f < FRAMES; f++) {
        draw_boing_ball(bx, by, BALL_R, rot);

        bx += vx; by += vy;
        if (bx - BALL_R < 0)              { bx = BALL_R;              vx = -vx; }
        if (bx + BALL_R >= DISPLAY_WIDTH)  { bx = DISPLAY_WIDTH  - 1 - BALL_R; vx = -vx; }
        if (by - BALL_R < 0)              { by = BALL_R;              vy = -vy; }
        if (by + BALL_R >= DISPLAY_HEIGHT) { by = DISPLAY_HEIGHT - 1 - BALL_R; vy = -vy; }
        rot += ROT_STEP;

        sleep_ms(65);   // ~15 fps
    }

    // Leave a clean black screen before normal boot continues
    st7789_fill(rgb(0, 0, 0));
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
