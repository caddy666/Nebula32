#include "effects.h"
#include "display.h"
#include "pico/stdlib.h"
#include "hardware/interp.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Colour helpers                                                       */
/* ------------------------------------------------------------------ */

static uint16_t hsv(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return rgb(v, v, v);
    uint8_t reg = h / 43;
    uint8_t rem = (uint8_t)((h - (uint16_t)reg * 43) * 6);
    uint8_t p   = (uint8_t)((v * (255U - s)) >> 8);
    uint8_t q   = (uint8_t)((v * (255U - ((s * rem) >> 8))) >> 8);
    uint8_t t_  = (uint8_t)((v * (255U - ((s * (255U - rem)) >> 8))) >> 8);
    switch (reg) {
        case 0:  return rgb(v, t_, p);
        case 1:  return rgb(q, v,  p);
        case 2:  return rgb(p, v,  t_);
        case 3:  return rgb(p, q,  v);
        case 4:  return rgb(t_, p, v);
        default: return rgb(v, p,  q);
    }
}

/* Amiga "copper" gradient — dark red at base → white-yellow at tip. */
static const uint8_t COP_R[] = { 0x30, 0x60, 0x90, 0xB0, 0xD0, 0xF0, 0xFF, 0xFF };
static const uint8_t COP_G[] = { 0x00, 0x00, 0x10, 0x30, 0x60, 0x90, 0xD0, 0xFF };
static const uint8_t COP_B[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40, 0xFF };
#define COP_STEPS 7

/* INTERP0 blend mode: peek[1] = base0 + ((base1-base0) * accum1[7:0]) >> 8 —
 * the exact arithmetic of the previous C lerp (COP tables are ascending, so
 * the unsigned default is safe).  Thread-context only: the Core 0 ISRs never
 * touch interp, so no interp_save/restore is needed. */
static bool interp_ready = false;

static void ensure_interp(void) {
    if (interp_ready) return;
    interp_claim_lane_mask(interp0, 0x3);
    interp_config cfg = interp_default_config();
    interp_config_set_blend(&cfg, true);
    interp_set_config(interp0, 0, &cfg);
    cfg = interp_default_config();
    interp_set_config(interp0, 1, &cfg);
    interp_ready = true;
}

static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    interp0->base[0]  = a;
    interp0->base[1]  = b;
    interp0->accum[1] = t;
    return (uint8_t)interp0->peek[1];
}

static uint16_t copper_color(uint8_t frac) {
    uint8_t seg  = (uint8_t)((uint16_t)frac * COP_STEPS >> 8);
    uint8_t t    = (uint8_t)((uint16_t)frac * COP_STEPS - (uint16_t)seg * 256);
    if (seg >= COP_STEPS) { seg = COP_STEPS - 1; t = 255; }
    ensure_interp();
    uint8_t r = lerp8(COP_R[seg], COP_R[seg+1], t);
    uint8_t g = lerp8(COP_G[seg], COP_G[seg+1], t);
    uint8_t b = lerp8(COP_B[seg], COP_B[seg+1], t);
    return rgb(r, g, b);
}

/* ------------------------------------------------------------------ */
/* Line buffer                                                          */
/* ------------------------------------------------------------------ */

static uint16_t line[DISP_W];

/* ------------------------------------------------------------------ */
/* Sine table — 256 entries, values 0..255                             */
/* ------------------------------------------------------------------ */

static uint8_t sine256[256];
static bool    sine_ready = false;

static void ensure_sine(void) {
    if (sine_ready) return;
    for (int i = 0; i < 256; i++) {
        float v = sinf((float)i * 6.28318F / 256.0F);
        sine256[i] = (uint8_t)((v * 127.0F) + 128.0F);
    }
    sine_ready = true;
}

/* ------------------------------------------------------------------ */
/* Effect 0: SPECTRUM                                                   */
/* ------------------------------------------------------------------ */

#define BAR_W      7
#define BAR_GAP    0
#define BAR_MARGIN 8
#define MAX_BAR_H  (DISP_H - 2)

static void render_spectrum(const EffectCtx *ctx) {
    for (int y = 0; y < DISP_H; y++) {
        uint16_t bg = rgb(0x00, 0x00, 0x08);
        for (int i = 0; i < DISP_W; i++) line[i] = bg;

        int y_from_bottom = DISP_H - 1 - y;

        for (int b = 0; b < NUM_BARS; b++) {
            int bar_h  = (ctx->spectrum[b] * MAX_BAR_H) >> 8;
            int peak_y = DISP_H - 1 - ((ctx->peaks[b] * MAX_BAR_H) >> 8);
            int x0 = BAR_MARGIN + b * (BAR_W + BAR_GAP);
            int x1 = x0 + BAR_W;

            if (y_from_bottom < bar_h) {
                uint8_t frac = (bar_h > 0) ? (uint8_t)((y_from_bottom * 255) / bar_h) : 0;
                uint16_t col = copper_color(frac);
                for (int x = x0; x < x1; x++) line[x] = col;
            } else if (y == peak_y && ctx->peaks[b] > 4) {
                uint16_t white = rgb(0xFF, 0xFF, 0xFF);
                for (int x = x0; x < x1; x++) line[x] = white;
            }
        }

        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Effect 1: SCOPE                                                      */
/* ------------------------------------------------------------------ */

static inline int sample_to_y(int16_t s) {
    int y = DISP_H / 2 - ((int)s * (DISP_H / 2 - 2)) / 32767;
    if (y < 0) y = 0;
    if (y >= DISP_H) y = DISP_H - 1;
    return y;
}

static void render_scope(const EffectCtx *ctx) {
    static int16_t sy[DISP_W];
    for (int x = 0; x < DISP_W; x++) {
        int sample_idx = (x * (FFT_SIZE - 1)) / (DISP_W - 1);
        sy[x] = (int16_t)sample_to_y(ctx->waveform[sample_idx]);
    }

    uint16_t trace_col = rgb(0x00, 0xFF, 0x80);
    uint16_t glow_col  = rgb(0x00, 0x40, 0x20);
    uint16_t bg        = rgb(0x00, 0x00, 0x00);
    uint16_t cline_col = rgb(0x10, 0x00, 0x30);

    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            int dy = abs(y - (int)sy[x]);
            if (dy == 0)                   line[x] = trace_col;
            else if (dy == 1)              line[x] = glow_col;
            else if (y == DISP_H / 2)     line[x] = cline_col;
            else                           line[x] = bg;
        }
        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Effect 2: RASTER BARS                                               */
/* ------------------------------------------------------------------ */

#define N_RASTER 6
#define RASTER_H 18

static void render_raster(const EffectCtx *ctx) {
    ensure_sine();

    uint32_t avg = 0;
    for (int b = 0; b < NUM_BARS; b++) avg += ctx->spectrum[b];
    avg /= NUM_BARS;

    int bar_y[N_RASTER];
    for (int b = 0; b < N_RASTER; b++) {
        uint8_t phase = (uint8_t)(ctx->frame * (3 + b) + b * 42);
        int spread    = DISP_H / 2 + (int)(avg >> 2);
        bar_y[b] = DISP_H / 2 + (((int)sine256[phase] - 128) * spread) / 128;
    }

    for (int y = 0; y < DISP_H; y++) {
        uint32_t r_acc = 0, g_acc = 0, b_acc = 0;

        for (int b = 0; b < N_RASTER; b++) {
            int dy = abs(y - bar_y[b]);
            if (dy >= RASTER_H) continue;
            int scaled = (RASTER_H - dy) * 255 / RASTER_H;
            uint8_t h  = (uint8_t)(b * 40 + (ctx->frame >> 1));
            uint16_t c = hsv(h, 230, (uint8_t)scaled);
            uint16_t native = (uint16_t)((c >> 8) | (c << 8));
            r_acc += (native >> 8) & 0xF8U;
            g_acc += (native >> 3) & 0xFCU;
            b_acc += (native << 3) & 0xF8U;
        }
        if (r_acc > 255) r_acc = 255;
        if (g_acc > 255) g_acc = 255;
        if (b_acc > 255) b_acc = 255;

        uint16_t row_col = rgb((uint8_t)r_acc, (uint8_t)g_acc, (uint8_t)b_acc);
        for (int x = 0; x < DISP_W; x++) line[x] = row_col;
        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Effect 3: COMBO — raster background + spectrum overlay              */
/* ------------------------------------------------------------------ */

static void render_combo(const EffectCtx *ctx) {
    ensure_sine();

    uint32_t avg = 0;
    for (int b = 0; b < NUM_BARS; b++) avg += ctx->spectrum[b];
    avg /= NUM_BARS;

    int bar_y[4];
    for (int b = 0; b < 4; b++) {
        uint8_t phase = (uint8_t)(ctx->frame * (2 + b) + b * 64);
        bar_y[b] = DISP_H / 2 + (((int)sine256[phase] - 128) * (DISP_H / 2)) / 128;
    }

    for (int y = 0; y < DISP_H; y++) {
        uint32_t r_acc = 0, g_acc = 0, b_acc = 0;
        for (int b = 0; b < 4; b++) {
            int dy = abs(y - bar_y[b]);
            if (dy >= RASTER_H) continue;
            int scaled = (RASTER_H - dy) * 120 / RASTER_H;
            uint8_t h  = (uint8_t)(b * 60 + (ctx->frame >> 2));
            uint16_t c = hsv(h, 200, (uint8_t)scaled);
            uint16_t native = (uint16_t)((c >> 8) | (c << 8));
            r_acc += (native >> 8) & 0xF8U;
            g_acc += (native >> 3) & 0xFCU;
            b_acc += (native << 3) & 0xF8U;
        }
        if (r_acc > 255) r_acc = 255;
        if (g_acc > 255) g_acc = 255;
        if (b_acc > 255) b_acc = 255;

        for (int x = 0; x < DISP_W; x++)
            line[x] = rgb((uint8_t)r_acc, (uint8_t)g_acc, (uint8_t)b_acc);

        int y_from_bottom = DISP_H - 1 - y;
        for (int b = 0; b < NUM_BARS; b++) {
            int bar_h  = (ctx->spectrum[b] * MAX_BAR_H) >> 8;
            int peak_y = DISP_H - 1 - ((ctx->peaks[b] * MAX_BAR_H) >> 8);
            int x0     = BAR_MARGIN + b * (BAR_W + BAR_GAP);
            int x1     = x0 + BAR_W;

            if (y_from_bottom < bar_h) {
                uint8_t frac = (bar_h > 0) ? (uint8_t)((y_from_bottom * 255) / bar_h) : 0;
                uint16_t col = copper_color(frac);
                for (int x = x0; x < x1; x++) line[x] = col;
            } else if (y == peak_y && ctx->peaks[b] > 4) {
                uint16_t white = rgb(0xFF, 0xFF, 0xFF);
                for (int x = x0; x < x1; x++) line[x] = white;
            }
        }

        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Effect 4: SPACEBALLS — chunky-pixel dancer                          */
/* ------------------------------------------------------------------ */

#define CHONK    4
#define CW       (DISP_W / CHONK)
#define CH       (DISP_H / CHONK)
#define CX       (CW / 2)
#define CY       (CH / 2)

#define NUM_POSES  8
#define MAX_SPARKS 12
#define PAL_SIZE   16

static uint8_t sb_canvas[CH][CW];

static const uint8_t pal_r[PAL_SIZE] = {
      0,  20,  60, 100,  60,   0,   0, 100,
    255, 255, 255, 255, 255,   0, 200, 120
};
static const uint8_t pal_g[PAL_SIZE] = {
      0,   0,   0,   0,   0,  60, 200, 255,
    255, 255, 230, 140,  40,   0,   0,   0
};
static const uint8_t pal_b[PAL_SIZE] = {
      0,  60, 120, 180, 255, 255, 255, 255,
    200,   0,   0,   0,   0, 120, 160, 100
};

typedef struct {
    int8_t hx,hy;
    int8_t sx,sy;
    int8_t px,py;
    int8_t lax,lay, rax,ray;
    int8_t lkx,lky, rkx,rky;
    int8_t lfx,lfy, rfx,rfy;
} Pose;

static const Pose poses[NUM_POSES] = {
    {  0,-12,  0,-9,  0,-3, -4,-5,  4,-5, -2, 4,  2, 4, -3,11,  3,11 },
    {  0,-12,  0,-9,  0,-3, -7,-13, 7,-13, -2, 4,  2, 4, -3,11,  3,11 },
    {  0,-14,  0,-11, 0,-5, -6,-10, 6,-10, -7, 2,  7, 2, -9, 8,  9, 8 },
    {  2,-12,  1,-9,  0,-3, -4,-13, 6,-5,  -3, 4,  3, 3, -3,11,  4,10 },
    { -1,-12, -1,-9,  0,-3, -6,-7,  9,-6,  -3, 4,  4, 2, -4,11, 11, 2 },
    {  0,-9,   0,-6,  0,-1, -5,-4,  5,-4,  -4, 5,  4, 5, -5,10,  5,10 },
    { -9,-2,  -5,-4,  1,-4, -9,-8,  3,-9,   4,-1,  6, 2,  8,-5,  9, 5 },
    {  1,-12,  1,-9,  0,-3, -4,-10, 7,-5,  -2, 4,  3, 3, -3,11,  4,10 },
};

typedef struct { int8_t cx,cy,vx,vy; uint8_t life; } Spark;

static struct {
    uint8_t  pose;
    uint8_t  col_offset;
    uint8_t  frame_div;
    uint8_t  beat_cooldown;
    uint8_t  beat_flash;
    uint8_t  no_beat_timer;
    uint32_t bass_avg;
    Spark    sparks[MAX_SPARKS];
} sb;

static void sb_line(int x0, int y0, int x1, int y1, uint8_t col) {
    int dx = abs(x1-x0), sx = x0<x1 ? 1:-1;
    int dy = -abs(y1-y0), sy = y0<y1 ? 1:-1;
    int err = dx+dy;
    for(;;) {
        if ((unsigned)x0 < CW && (unsigned)y0 < CH)
            sb_canvas[y0][x0] = col;
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 >= dy) { err+=dy; x0+=sx; }
        if (e2 <= dx) { err+=dx; y0+=sy; }
    }
}

static void sb_circle(int cx, int cy, int r, uint8_t col) {
    for (int dy=-r; dy<=r; dy++)
        for (int dx=-r; dx<=r; dx++)
            if (dx*dx+dy*dy <= r*r+1)
                if ((unsigned)(cx+dx)<CW && (unsigned)(cy+dy)<CH)
                    sb_canvas[cy+dy][cx+dx] = col;
}

static void sb_draw_pose(uint8_t idx) {
    memset(sb_canvas, 0, sizeof(sb_canvas));
    const Pose *p = &poses[idx];
    int hx=CX+p->hx, hy=CY+p->hy;
    int sx=CX+p->sx, sy=CY+p->sy;
    int px=CX+p->px, py=CY+p->py;

    sb_line(CX+p->lkx,CY+p->lky, CX+p->lfx,CY+p->lfy, 1);
    sb_line(px,py,               CX+p->lkx,CY+p->lky,  1);
    sb_line(CX+p->rkx,CY+p->rky, CX+p->rfx,CY+p->rfy, 2);
    sb_line(px,py,               CX+p->rkx,CY+p->rky,  2);
    sb_line(sx,sy, CX+p->lax,CY+p->lay, 3);
    sb_line(sx,sy, CX+p->rax,CY+p->ray, 4);
    sb_line(sx,sy, px,py,                5);
    sb_line(hx,hy, sx,sy,                6);
    sb_circle(hx,hy, 2,                  7);
}

static void render_spaceballs(const EffectCtx *ctx) {
    uint32_t bass = (uint32_t)ctx->spectrum[0] + ctx->spectrum[1]
                  + ctx->spectrum[2] + ctx->spectrum[3];

    if (sb.bass_avg == 0) sb.bass_avg = bass * 256U;
    sb.bass_avg = (sb.bass_avg * 15U + bass * 256U) >> 4;

    bool beat = (bass * 256U * 13U / 10U > sb.bass_avg)
             && (bass > 80U)
             && (sb.beat_cooldown == 0);

    if (beat) {
        sb.pose           = (uint8_t)((sb.pose + 1U) % NUM_POSES);
        sb.beat_flash     = 10;
        sb.beat_cooldown  = 15;
        sb.no_beat_timer  = 0;
        for (int i = 0; i < MAX_SPARKS; i++) {
            uint32_t seed = ctx->frame * 13U + (uint32_t)i * 7U;
            sb.sparks[i].cx   = (int8_t)(CX + (int)(((seed*11U)%21U)) - 10);
            sb.sparks[i].cy   = (int8_t)(CY + (int)(((seed* 7U)%15U)) -  7);
            sb.sparks[i].vx   = (int8_t)((int)((seed*3U)%7U) - 3);
            sb.sparks[i].vy   = (int8_t)(-2 - (int8_t)((seed)%4U));
            sb.sparks[i].life = (uint8_t)(12U + (seed % 8U));
        }
    }
    if (sb.beat_cooldown) sb.beat_cooldown--;
    if (sb.beat_flash)    sb.beat_flash--;

    if (++sb.no_beat_timer > 80) {
        sb.no_beat_timer = 0;
        sb.pose = (uint8_t)((sb.pose + 1U) % NUM_POSES);
    }

    if (++sb.frame_div >= 3) {
        sb.frame_div  = 0;
        sb.col_offset = (uint8_t)((sb.col_offset + 1U) % 15U);
    }

    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!sb.sparks[i].life) continue;
        sb.sparks[i].life--;
        if ((sb.sparks[i].life & 1) == 0) {
            sb.sparks[i].cx = (int8_t)(sb.sparks[i].cx + sb.sparks[i].vx);
            sb.sparks[i].cy = (int8_t)(sb.sparks[i].cy + sb.sparks[i].vy);
            if (sb.sparks[i].vy < 4) sb.sparks[i].vy++;
        }
    }

    sb_draw_pose(sb.pose);
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!sb.sparks[i].life) continue;
        int8_t scx = sb.sparks[i].cx, scy = sb.sparks[i].cy;
        if ((unsigned)scx < CW && (unsigned)scy < CH)
            sb_canvas[scy][scx] = 8;
    }

    for (int y = 0; y < DISP_H; y++) {
        int cy_c    = y >> 2;
        bool row_edge = ((y & 3) == 3);
        bool odd_row  = (y & 1);

        uint8_t bg_b = (uint8_t)(25U + (uint32_t)y * 15U / DISP_H);
        uint8_t bg_r = (uint8_t)(sb.beat_flash * 2U);
        uint16_t bg_full = rgb(bg_r,      0,      bg_b);
        uint16_t bg_dim  = rgb(bg_r >> 1, 0, bg_b >> 1);

        for (int x = 0; x < DISP_W; x++) {
            int  cx_c    = x >> 2;
            bool col_edge = ((x & 3) == 3);
            uint8_t cell  = (cy_c < CH) ? sb_canvas[cy_c][cx_c] : 0;

            if (cell == 0) {
                line[x] = odd_row ? bg_dim : bg_full;
                continue;
            }

            uint8_t pi;
            if (cell == 8) {
                pi = (uint8_t)((8U + sb.col_offset) % 15U + 1U);
            } else {
                pi = (uint8_t)(((uint8_t)(cell - 1U) * 2U + sb.col_offset) % 15U + 1U);
            }

            uint8_t r = pal_r[pi], g = pal_g[pi], b = pal_b[pi];

            if (sb.beat_flash) {
                uint16_t fl = (uint16_t)sb.beat_flash * 16U;
                r = (uint8_t)((r + fl > 255U) ? 255U : r + fl);
                g = (uint8_t)((g + fl > 255U) ? 255U : g + fl);
                b = (uint8_t)((b + fl > 255U) ? 255U : b + fl);
            }

            if (row_edge || col_edge) {
                r = (uint8_t)(r * 3U >> 2);
                g = (uint8_t)(g * 3U >> 2);
                b = (uint8_t)(b * 3U >> 2);
            }

            if (odd_row) {
                r = (uint8_t)(r * 3U >> 2);
                g = (uint8_t)(g * 3U >> 2);
                b = (uint8_t)(b * 3U >> 2);
            }

            line[x] = rgb(r, g, b);
        }
        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Effect 5: JUGGLER — procedural 3-ball cascade stick figure          */
/* ------------------------------------------------------------------ */
/* Three balls orbit in staggered ellipses; the figure's arm reaches  */
/* toward whichever ball is at the bottom of its arc (being "caught"). */

static void render_juggler(const EffectCtx *ctx) {
    ensure_sine();

    // Average spectrum for audio-reactive brightness
    uint32_t avg = 0;
    for (int b = 0; b < NUM_BARS; b++) avg += ctx->spectrum[b];
    avg /= NUM_BARS;

    // Ball orbital positions in canvas space (60×60, CX/CY at centre)
    float t = (float)ctx->frame * 0.15F;
    int bx[3], by[3];
    for (int i = 0; i < 3; i++) {
        float phase = t + (float)i * 2.0944F;   // 2π/3 offset per ball
        bx[i] = CX + (int)(11.0F * sinf(phase));
        by[i] = CY - 22 + (int)(9.0F * cosf(phase));
    }

    // Find ball at lowest canvas-y (bottom of arc = "hand" position)
    int lowest = 0;
    for (int i = 1; i < 3; i++)
        if (by[i] > by[lowest]) lowest = i;

    // Body landmarks
    int hy = CY - 38;   // head top
    int sy = CY - 26;   // shoulder
    int py = CY - 10;   // pelvis

    // Arm endpoints: one hand reaches to lowest ball, other points upward
    int lhx, lhy, rhx, rhy;
    if (bx[lowest] <= CX) {
        lhx = bx[lowest]; lhy = by[lowest];
        rhx = CX + 9;     rhy = sy - 6;
    } else {
        lhx = CX - 9;     lhy = sy - 6;
        rhx = bx[lowest]; rhy = by[lowest];
    }

    memset(sb_canvas, 0, sizeof(sb_canvas));

    // Body (cell value 6 = white in juggler palette)
    sb_circle(CX, hy, 4, 6);                     // head
    sb_line(CX, hy + 5, CX, sy, 6);              // neck → shoulder
    sb_line(CX, sy, CX, py, 6);                  // torso
    sb_line(CX, sy, lhx, lhy, 3);               // left arm
    sb_line(CX, sy, rhx, rhy, 4);               // right arm
    sb_line(CX, py, CX - 7, py + 13, 1);        // left leg
    sb_line(CX, py, CX + 7, py + 13, 2);        // right leg

    // Balls (cell values 8/9/10 → R/G/B)
    for (int i = 0; i < 3; i++)
        sb_circle(bx[i], by[i], 2, (uint8_t)(8 + i));

    // Rasterise canvas → display
    uint8_t brightness = (uint8_t)(20U + (avg >> 2));
    uint16_t bg = rgb(brightness >> 2, brightness >> 2, brightness);

    for (int y = 0; y < DISP_H; y++) {
        int cy_c = y >> 2;
        for (int x = 0; x < DISP_W; x++) {
            int cx_c = x >> 2;
            uint8_t cell = ((unsigned)cy_c < CH && (unsigned)cx_c < CW)
                           ? sb_canvas[cy_c][cx_c] : 0;
            if (cell == 0) {
                line[x] = bg;
                continue;
            }
            uint8_t rv, gv, bv;
            switch (cell) {
                case 8:  rv = 230; gv =  40; bv =  40; break;  // red ball
                case 9:  rv =  40; gv = 230; bv =  40; break;  // green ball
                case 10: rv =  40; gv = 100; bv = 230; break;  // blue ball
                default: rv = 210; gv = 210; bv = 210; break;  // body
            }
            // Modest audio-reactive highlight
            uint16_t boost = (uint16_t)(avg >> 3);
            rv = (rv + boost > 255U) ? 255U : (uint8_t)(rv + boost);
            line[x] = rgb(rv, gv, bv);
        }
        display_hline((uint16_t)y, line);
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void effects_render(const EffectCtx *ctx) {
    switch (ctx->mode % NUM_EFFECTS) {
        case 0:  render_spectrum(ctx);   break;
        case 1:  render_scope(ctx);      break;
        case 2:  render_raster(ctx);     break;
        case 3:  render_combo(ctx);      break;
        case 4:  render_spaceballs(ctx); break;
        default: render_juggler(ctx);    break;
    }
}
