// =============================================================================
// main.c — CD32 Optical Drive Emulator — Entry Point
//          Raspberry Pi Pico 2 (RP2350)
// =============================================================================
//
// CORE ASSIGNMENT:
//   Core 0 — COMMO command polling, periodic state updates, USB CDC console,
//             web server, rotary encoder UI, display, logger flush
//   Core 1 — Sector prefetch loop: SD card → sector_cache ring buffer
//
// BOOT SEQUENCE:
//   1. Clock RP2350 to 135,475,200 Hz (= 16.9344 MHz × 8 — exact DA timing)
//   2. Stdio init (USB CDC)
//   3. Load persistent config from flash
//   4. Mount SD card via 4-bit SDIO; scan disc images
//   5. Initialise logger (reads cd32_ode.cfg, opens cd32_cd.log)
//   6. Open selected disc image; initialise sector_cache
//   7. Initialise DA output PIO (GPIO 0-2) — starts clocking I2S to Akiko
//   8. Initialise subcode encoder PIO (GPIO 5-8)
//   9. Initialise I2C0 (MCP23017 rotary encoder, GPIO 28-29)
//  10. Initialise ST7789 display (SPI1, GPIO 13/24/26/27)
//  11. Initialise COMMO bus bridge (PIO1 SM0/SM1, GPIO 15-17)
//  12. Launch Core 1 (sector prefetch)
//  13. Core 0 enters main polling loop
//
// DA SIGNAL PATH:
//   SD card → disc_read_sector() → sector_cache ring buffer (Core 1 prefetch)
//          → da_start_play() → DMA → PIO TX FIFO → DA_DATA/BCLK/LRCLK
//          → Akiko (U5) + LC78835M DAC (U31)
//
// CLOCK:
//   sys_clk = 135,475,200 Hz gives exact PIO clkdiv = 32 for 1× BCLK
//   (2,117,550 Hz) and clkdiv = 16 for 2× BCLK (4,234,200 Hz).
//   [Original Commodore ref: clkdiv=48 → 1,411,200 Hz; clkdiv=24 → 2,822,400 Hz]
// =============================================================================

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

#include "cd_types.h"
#include "da_output.h"
#include "fw_update.h"
#include "disc_image.h"
#include "sector_cache.h"
#include "sd_card_api.h"
#include "subcode.h"
#include "selftest.h"
#include "config.h"
#include "logger.h"
#include "rotary.h"
#include "ui.h"
#include "webserver.h"
#include "commo_bridge.h"
#include "gpio_map.h"
#include "timer.h"
#include "display.h"
#include "fft.h"
#include "effects.h"
#include "vis_audio.h"

#include "da_output.pio.h"
#include "subcode_encoder.pio.h"

#include <stdio.h>
#include <string.h>

// =============================================================================
// System clock
// =============================================================================
// 135,475,200 Hz = 16,934,400 × 8.
// This gives PIO clkdiv = 32 → SM at 4.234 MHz → BCLK = 2.117 MHz (1× CD).
// The RP2350 can reach this frequency without overclocking — it is below the
// default 150 MHz so no voltage bump is needed.
#define TARGET_SYS_CLK_KHZ  135475

// =============================================================================
// Global state (extern'd by commo_bridge.c and webserver.c)
// =============================================================================
disc_image_t   g_disc;   // Currently open disc image
sector_cache_t g_cache;  // Read-ahead sector ring buffer

// Image list (extern'd by ui.c and webserver.c)
// s_image_paths holds one page of up to PAGE_SIZE paths.
// s_page_offset is the absolute index of the first entry in that page.
// s_total_count is the full count across all pages (from sd_count_images at boot).
#define PAGE_SIZE   64
char     s_image_paths[PAGE_SIZE][MAX_PATH_LEN];
uint32_t s_image_count    = 0;   // entries in current page (≤ PAGE_SIZE)
static uint32_t s_page_offset   = 0;   // absolute index of s_image_paths[0]
static uint32_t s_total_count   = 0;   // total images on SD card
static uint32_t s_selected_image = 0;  // page-local selected index

// Persistent configuration
static ode_config_t g_config;

// Visualiser state
static uint8_t  s_vis_spectrum[NUM_BARS];
static uint8_t  s_vis_peaks[NUM_BARS];
static int16_t  s_vis_waveform[FFT_SIZE];
static int16_t  s_vis_samples[FFT_SIZE];
static EffectCtx s_vis_ctx;
static bool     s_vis_active = false;   // true when cover art is replaced by vis

// Subcode PIO assignment
#if BUILD_WITH_COMMO
static PIO  s_sub_pio    = pio0;   // COMMO on PIO1 — subcode on PIO0 SM1
static uint s_sm_subcode = 1;
#else
static PIO  s_sub_pio    = pio1;   // Subcode on PIO1 SM0 (PIO0 SM0 = DA output)
static uint s_sm_subcode = 0;
#endif

// ---------------------------------------------------------------------------
// load_image_page — fill s_image_paths from the SD card at the given offset
// ---------------------------------------------------------------------------
static void load_image_page(uint32_t offset) {
    s_page_offset = offset;
    s_image_count = sd_scan_images(s_image_paths, PAGE_SIZE,
                                   logger_get_config()->sdcard_base, offset);
    webserver_notify_state_change();  // invalidates cover cache in webserver.c
    webserver_set_page_info(s_page_offset, s_image_count, s_total_count);
    printf("[MAIN] Page offset=%lu count=%lu/%lu\n",
           (unsigned long)s_page_offset,
           (unsigned long)s_image_count,
           (unsigned long)s_total_count);
}

// ---------------------------------------------------------------------------
// navigate_page — advance or retreat one page
// ---------------------------------------------------------------------------
static void navigate_page(int delta) {
    if (delta > 0) {
        if (s_page_offset + PAGE_SIZE >= s_total_count) {
            printf("[MAIN] Already on last page\n");
            return;
        }
        load_image_page(s_page_offset + PAGE_SIZE);
    } else {
        if (s_page_offset == 0) {
            printf("[MAIN] Already on first page\n");
            return;
        }
        load_image_page(s_page_offset >= PAGE_SIZE ? s_page_offset - PAGE_SIZE : 0);
    }
    s_selected_image = 0;
    ui_init(s_image_count, 0);
}

// ---------------------------------------------------------------------------
// load_disc_image — switch to a different disc image
// ---------------------------------------------------------------------------
static bool load_disc_image(uint32_t index) {
    if (index == UINT32_MAX) {
        printf("[MAIN] Ejecting disc\n");
        LOG_INFO_MSG("MAIN", "disc ejected");
        da_stop();
        disc_close(&g_disc);
        sector_cache_flush(&g_cache);
        display_clear();
        ui_on_disc_loaded(UINT32_MAX);
        webserver_notify_state_change();
        return true;
    }

    if (index >= s_image_count) {
        printf("[MAIN] load_disc_image: index %lu out of range\n",
               (unsigned long)index);
        return false;
    }

    printf("[MAIN] Loading: %s\n", s_image_paths[index]);
    LOG_INFO_MSG("MAIN", "loading %lu: %s",
                 (unsigned long)(index + 1), s_image_paths[index]);

    da_stop();
    disc_close(&g_disc);

    if (!disc_open(&g_disc, s_image_paths[index])) {
        printf("[MAIN] ERROR: Failed to open %s\n", s_image_paths[index]);
        LOG_ERROR_MSG("Failed to open image %lu", (unsigned long)index);
        ui_on_disc_error();
        return false;
    }

    sector_cache_init(&g_cache, &g_disc);
    s_selected_image = index;

    uint16_t abs_index = (uint16_t)(s_page_offset + index);
    if (g_config.last_image_index != abs_index) {
        g_config.last_image_index = abs_index;
        config_save(&g_config);
    }

    display_show_cover(s_image_paths[index]);
    ui_on_disc_loaded(index);
    webserver_set_loaded_index(index);
    webserver_notify_state_change();

    printf("[MAIN] Disc loaded: %d-%d tracks, %lu sectors\n",
           g_disc.first_track, g_disc.last_track,
           (unsigned long)g_disc.total_sectors);
    LOG_INFO_MSG("MAIN", "disc loaded: tracks=%d-%d sectors=%lu",
                 g_disc.first_track, g_disc.last_track,
                 (unsigned long)g_disc.total_sectors);
    return true;
}

// =============================================================================
// Core 1 — sector prefetch loop
// =============================================================================
static void core1_main(void) {
    multicore_lockout_victim_init();   // park in SRAM when Core 0 writes flash
    printf("[CORE1] Sector prefetch loop started\n");
    while (true) {
        sector_cache_prefetch_tick(&g_cache);
        // If all slots are full, sector_cache_prefetch_tick() returns immediately.
        // Sleep briefly instead of spinning at 100% CPU — Core 1 will be woken
        // within one sector period (6.7 ms at 2× speed) when Core 0 releases a slot.
        if (sector_cache_is_full(&g_cache))
            sleep_us(100);
        else
            tight_loop_contents();
    }
}

// =============================================================================
// M17SINE phase-lock: measure reference clock and trim DA PIO clkdiv
// =============================================================================
// The RP2350 hardware frequency counter measures GPIN0 (M17SINE on GPIO 9)
// using clk_ref as the reference.  Measurement takes ~1 ms (blocking).
// Called every 2 s from the main loop during audio playback.
static uint32_t s_m17sine_hz = 0;   // Last measured value (0 = not yet measured)

static void m17sine_nudge_tick(void) {
    uint32_t khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLKSRC_GPIN0);
    if (khz == 0) return;   // M17SINE not present (bench test without CD32)
    s_m17sine_hz = khz * 1000u;
    da_nudge_clkdiv_to_m17sine(s_m17sine_hz);
}

// =============================================================================
// Periodic timer callback (1 ms, Core 0)
// =============================================================================
static bool periodic_update_cb(struct repeating_timer *t) {
    (void)t;
    logger_flush_if_due();
    return true;
}

// =============================================================================
// USB console command handler
// =============================================================================
static void handle_console(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT || c == '\r' || c == '\n') return;

    if (c >= '1' && c <= '9') {
        uint32_t idx = (uint32_t)(c - '1');
        if (idx < s_image_count) {
            load_disc_image(idx);
        } else {
            printf("[MAIN] No image %lu on this page (page has %lu)\n",
                   idx + 1, (unsigned long)s_image_count);
        }
    }
    else if (c == '[') {
        navigate_page(-1);
    }
    else if (c == ']') {
        navigate_page(+1);
    }
    else if (c == 'l' || c == 'L') {
        uint32_t page_num   = s_page_offset / PAGE_SIZE + 1;
        uint32_t page_total = (s_total_count + PAGE_SIZE - 1) / PAGE_SIZE;
        printf("\n[MAIN] Page %lu/%lu  (images %lu-%lu of %lu)\n",
               (unsigned long)page_num, (unsigned long)page_total,
               (unsigned long)(s_page_offset + 1),
               (unsigned long)(s_page_offset + s_image_count),
               (unsigned long)s_total_count);
        for (uint32_t i = 0; i < s_image_count; i++) {
            printf("  %lu: %s%s\n",
                   (unsigned long)(s_page_offset + i + 1), s_image_paths[i],
                   (i == s_selected_image) ? "  <- current" : "");
        }
    }
    else if (c == 's' || c == 'S') {
        printf("\n[STATUS]\n");
        printf("  DA speed      : %s\n", da_is_double_speed() ? "2x" : "1x");
        printf("  DA playing    : %s\n", da_is_playing() ? "yes" : "no");
        printf("  Disc tracks   : %d-%d  (%lu sectors)\n",
               g_disc.first_track, g_disc.last_track,
               (unsigned long)g_disc.total_sectors);
        printf("  COMMO active  : %s\n",
               commo_bridge_is_active() ? "yes" : "no");
    }
    else if (c == 't' || c == 'T') {
        printf("\n[TOC] Track listing:\n");
        for (int i = g_disc.first_track; i <= g_disc.last_track; i++) {
            const track_t *trk = &g_disc.tracks[i - 1];
            msf_t msf = lba_to_msf(trk->start_lba);
            printf("  Track %02d: %-8s  %02X:%02X:%02X  len=%lu sectors\n",
                   trk->number,
                   trk->type == TRACK_TYPE_AUDIO ? "AUDIO" :
                   trk->type == TRACK_TYPE_XA    ? "DATA/XA" : "DATA",
                   msf.minute, msf.second, msf.frame,
                   (unsigned long)trk->length_sectors);
        }
    }
    else if (c == 'x' || c == 'X') {
        bool now_double = !da_is_double_speed();
        da_set_double_speed(now_double);
        printf("[MAIN] DA speed: %s\n", now_double ? "2x" : "1x");
    }
    else if (c == 'r' || c == 'R') {
        printf("[MAIN] Reset: flushing cache...\n");
        da_stop();
        sector_cache_flush(&g_cache);
        sector_cache_seek(&g_cache, 0);
        printf("[MAIN] Reset complete\n");
    }
    else if (c == 'g' || c == 'G') {
        bool now_enabled = !logger_is_enabled();
        logger_set_enabled(now_enabled);
        printf("[MAIN] Logging: %s\n", now_enabled ? "ENABLED" : "DISABLED");
    }
    else if (c == 'f' || c == 'F') {
        uint32_t written = logger_flush();
        printf("[MAIN] Log flushed: %lu bytes\n", (unsigned long)written);
    }
    else if (c == 'v' || c == 'V') {
        s_vis_ctx.mode = (uint8_t)((s_vis_ctx.mode + 1) % NUM_EFFECTS);
        const char *names[] = { "Spectrum", "Scope", "Raster", "Combo", "Spaceballs", "Juggler" };
        printf("[VIS] Effect: %s\n", names[s_vis_ctx.mode]);
    }
    else if (c == 'm' || c == 'M') {
        uint32_t khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLKSRC_GPIN0);
        s_m17sine_hz = khz * 1000u;
        uint32_t fixed = da_get_clkdiv_fixed();
        int32_t ppm = 0;
        if (s_m17sine_hz > 0) {
            uint32_t nominal_hz = 16934400u;
            ppm = (int32_t)(((int64_t)s_m17sine_hz - nominal_hz) * 1000000 / nominal_hz);
        }
        printf("[M17] M17SINE=%lu Hz  clkdiv=%u.%u/256  drift=%ld ppm\n",
               (unsigned long)s_m17sine_hz,
               (unsigned)(fixed >> 8), (unsigned)(fixed & 0xFF),
               (long)ppm);
    }
    else if (c == 'h' || c == 'H' || c == '?') {
        printf("\nCD32 ODE Console Commands:\n");
        printf("  1-9  — load disc image 1-9 on current page\n");
        printf("  [/]  — previous / next page of images\n");
        printf("  L    — list current page\n");
        printf("  S    — show status\n");
        printf("  T    — show disc TOC\n");
        printf("  X    — toggle DA speed (1x / 2x)\n");
        printf("  R    — reset (flush cache, seek to 0)\n");
        printf("  G    — toggle SD logging on/off\n");
        printf("  F    — force flush log buffer now\n");
        printf("  M    — measure M17SINE frequency and show clkdiv drift\n");
        printf("  V    — cycle visualiser effect (during CD-DA playback)\n");
        printf("  H/?  — this help\n\n");
    }
}

// =============================================================================
// main()
// =============================================================================
int main(void) {
    // ---- Set system clock to 135,475,200 Hz ----
    // Exact multiple of the Sony 16.9344 MHz master clock × 8.
    // Required for PIO clkdiv = 32 → BCLK = 2,117,112 Hz (1× CD speed).
    if (!set_sys_clock_khz(TARGET_SYS_CLK_KHZ, true)) {
        // If the exact target isn't reachable, try the nearest available.
        // Timing will be slightly off but the PLL will get as close as possible.
    }

    stdio_init_all();
    sleep_ms(2000);   // Wait for USB CDC enumeration

    printf("\n");
    printf("==================================================\n");
    printf("  CD32 Optical Drive Emulator\n");
    printf("  Raspberry Pi Pico 2 (RP2350)\n");
    printf("  sys_clk = %lu Hz (target %d kHz)\n",
           (unsigned long)clock_get_hz(clk_sys), TARGET_SYS_CLK_KHZ);
    printf("==================================================\n\n");

    // ---- Persistent config ----
    bool config_valid = config_init(&g_config);
    if (!config_valid) {
        printf("[MAIN] First boot — using defaults\n");
    }

    // ---- SD card ----
    printf("[MAIN] Mounting SD card (4-bit SDIO)...\n");
    if (!sd_card_init_and_mount()) {
        printf("[MAIN] FATAL: SD card mount failed\n");
        while (true) tight_loop_contents();
    }

    // Logger init reads cd32_ode.cfg (including sdcard_base) before the image
    // scan so that sd_scan_images() uses the configured directory.
    logger_init();

    // Boot-time firmware update sentinel: if NEBULA32.UF2 exists in SD root,
    // flash it to Bank 1, verify, copy to Bank 0, rename to NEBULA32.OLD, reboot.
    // fw_flash_and_reboot never returns on success; any error falls through.
    {
        fw_result_t fw_r = fw_flash_and_reboot("0:/NEBULA32.UF2");
        if (fw_r != FW_ERR_NOT_FOUND)
            printf("[FW] Boot update failed: %s\n", fw_result_str(fw_r));
    }

    s_total_count = sd_count_images(logger_get_config()->sdcard_base);
    if (s_total_count == 0) {
        printf("[MAIN] No disc images found in %s\n",
               logger_get_config()->sdcard_base);
        while (true) tight_loop_contents();
    }

    if (logger_is_enabled()) {
        logger_write(LOG_INFO, "BOOT", "%lu image(s) found in %s",
                     (unsigned long)s_total_count,
                     logger_get_config()->sdcard_base);
    }

    // Load the page that contains the last-used image.
    // Boot with page 0 if the saved index is out of range.
    uint32_t abs_start = g_config.last_image_index;
    if (abs_start >= s_total_count) abs_start = 0;
    uint32_t start_page = (abs_start / PAGE_SIZE) * PAGE_SIZE;
    load_image_page(start_page);

    // ---- Open starting disc image ----
    s_selected_image = abs_start - start_page;  // page-local
    if (s_selected_image >= s_image_count) s_selected_image = 0;

    printf("[MAIN] Opening: %s\n", s_image_paths[s_selected_image]);
    if (!disc_open(&g_disc, s_image_paths[s_selected_image])) {
        printf("[MAIN] FATAL: Cannot open disc image\n");
        while (true) tight_loop_contents();
    }
    sector_cache_init(&g_cache, &g_disc);

    // ---- FFT (visualiser) ----
    fft_init();
    s_vis_ctx.spectrum = s_vis_spectrum;
    s_vis_ctx.peaks    = s_vis_peaks;
    s_vis_ctx.waveform = s_vis_waveform;
    s_vis_ctx.frame    = 0;
    s_vis_ctx.mode     = 0;

    // ---- DA output PIO (PIO0 SM0, GPIO 0/1/2) ----
    // Starts clocking the I2S bit stream immediately at 1× speed.
    // The PIO TX FIFO will stall (pull block) until DMA starts feeding it.
    bool start_double_speed = (g_config.speed_mode == 2);
    da_output_init(start_double_speed);

    // ---- Subcode encoder PIO ----
    uint sub_off = pio_add_program(s_sub_pio, &subcode_encoder_program);
    // Subcode bit rate: 75 sectors/s × 98 subcode frames × 24 bits/frame = 176,400 bps
    subcode_encoder_program_init(s_sub_pio, s_sm_subcode,
                                 sub_off, SUB_DATA_PIN, 176400);
    printf("[PIO] subcode_encoder on PIO%d SM%d offset=%d\n",
           (s_sub_pio == pio0) ? 0 : 1, s_sm_subcode, sub_off);

    // ---- M17SINE — conditionally slave clk_peri to Sony 16.9344 MHz (GPIN0) ----
    // GPIO 9 carries the CD32 mainboard's 16.9344 MHz master reference.
    // Slaving clk_peri to it locks UART/SPI/I2C to the same crystal as Akiko and
    // the LC78835M DAC — mandatory for a CD32-installed drive (both chips derive
    // all timing from this oscillator).
    //
    // On bench (no CD32 attached), GPIO 9 is floating; frequency_count_khz returns
    // 0 or a garbage value.  Calling clock_configure with an absent GPIN0 source
    // freezes clk_peri and hangs all peripherals.  The check below guards this:
    // if M17SINE is outside its expected range we leave clk_peri on its default
    // PLL source so UART/I2C/SPI remain usable for development.
    //
    // PCB NOTE: the sine from the CD32 connector must swing cleanly past the
    // RP2350 GPIO thresholds (~0.8 V / 2.0 V).  A low-amplitude or slow-slewing
    // sine near the threshold causes multiple edge transitions per cycle and
    // corrupts clk_peri.  A series resistor (33 Ω) and Schmitt-trigger buffer
    // between the 26-pin connector and GPIO 9 is strongly recommended.
    gpio_init(M17SINE_PIN);
    gpio_set_dir(M17SINE_PIN, GPIO_IN);
    gpio_set_function(M17SINE_PIN, GPIO_FUNC_GPCK);
    {
        uint32_t m17_khz = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLKSRC_GPIN0);
        if (m17_khz >= 16800 && m17_khz <= 17100) {
            clock_configure(clk_peri, 0,
                            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_GPIN0,
                            16934400, 16934400);
            printf("[MAIN] clk_peri slaved to M17SINE on GPIO%d (%lu kHz)\n",
                   M17SINE_PIN, (unsigned long)m17_khz);
        } else {
            printf("[MAIN] M17SINE not detected on GPIO%d (bench mode — clk_peri unchanged)\n",
                   M17SINE_PIN);
        }
    }

    // ---- Self-test prompt ----
    printf("[MAIN] Press '#' within 3 seconds for self-test mode...\n");
    absolute_time_t st_deadline = make_timeout_time_us(3000000);
    while (absolute_time_diff_us(get_absolute_time(), st_deadline) > 0) {
        int c = getchar_timeout_us(100000);
        if (c == '#') {
            selftest_run();
            printf("[MAIN] Self-test complete.  Continuing boot...\n\n");
            break;
        }
    }

    // ---- I2C0 for MCP23017 (SDA=GPIO28, SCL=GPIO29 @ 400 kHz) ----
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(MCP23017_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MCP23017_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MCP23017_SDA_PIN);
    gpio_pull_up(MCP23017_SCL_PIN);
    printf("[MAIN] I2C0 ready (GPIO%d/GPIO%d)\n",
           MCP23017_SDA_PIN, MCP23017_SCL_PIN);

    // ---- ST7789 240×240 display (SPI1) ----
    display_init();

    // O6: If the device just rebooted after a successful firmware update, play the
    // Boing Ball animation.  The watchdog scratch register survives a watchdog reboot
    // but is cleared on a power-cycle, so this fires exactly once after each flash.
    if (watchdog_hw->scratch[0] == FW_UPDATE_MAGIC) {
        watchdog_hw->scratch[0] = 0;  // consume — one-shot
        display_fw_success_animation();
    }

    // ---- Rotary encoder (MCP23017) ----
    rotary_init();

    // ---- Disc selector UI ----
    ui_init(s_image_count, s_selected_image);

    // ---- DOOR / SCOR GPIOs + 8 ms software timer ----
    gpio_init(PIN_DOOR);
    gpio_set_dir(PIN_DOOR, GPIO_IN);
    gpio_pull_up(PIN_DOOR);
    gpio_init(PIN_SCOR);
    gpio_set_dir(PIN_SCOR, GPIO_IN);
    gpio_pull_up(PIN_SCOR);
    timer_init();

    // ---- COMMO bus bridge ----
    commo_bridge_init();

    // ---- HTTP web server (Pico 2 W) ----
    webserver_init();
    webserver_set_loaded_index(s_selected_image);
    webserver_set_page_info(s_page_offset, s_image_count, s_total_count);
    if (webserver_is_running()) {
        logger_write(LOG_INFO, "WEB ", "http://%s/", webserver_get_ip());
    }

    // ---- Cover art for initial disc ----
    display_show_cover(s_image_paths[s_selected_image]);

    // ---- Launch Core 1 (sector prefetch) ----
    multicore_launch_core1(core1_main);
    printf("[MAIN] Core 1 started (sector prefetch)\n");

    // ---- 500 ms repeating timer for logger flush ----
    // logger_flush_if_due() only does work every 2000 ms, so 1 ms was 2000×
    // wasted wakeups per useful operation.  500 ms keeps the flush timely
    // without meaningless interrupts.
    struct repeating_timer update_timer;
    add_repeating_timer_us(-500000, periodic_update_cb, NULL, &update_timer);

    printf("[MAIN] System ready — CD32 can now access the drive\n");
    printf("[MAIN] DA: %s speed  |  BCLK: %lu Hz\n",
           da_is_double_speed() ? "2x" : "1x",
           /* orig Commodore ref: / (da_is_double_speed() ? 24u : 48u) */
           (unsigned long)(TARGET_SYS_CLK_KHZ * 1000ul
                           / (da_is_double_speed() ? 16u : 32u) / 2u));
    printf("[MAIN] Type H for console help\n\n");

    // ---- Core 0 main loop ----
    absolute_time_t s_nudge_next = make_timeout_time_us(2000000);

    while (true) {
        handle_console();

        // M17SINE phase-lock: trim DA clkdiv every 2 s during audio playback
        if (da_is_playing() && absolute_time_diff_us(get_absolute_time(), s_nudge_next) <= 0) {
            m17sine_nudge_tick();
            s_nudge_next = make_timeout_time_us(2000000);
        }

        if (ui_tick()) {
            uint32_t sel = ui_get_selected_index();
            load_disc_image(sel);
            s_vis_active = false;
        }

        bool commo_active = commo_bridge_poll();
        webserver_poll();

        if (webserver_has_load_request()) {
            uint32_t web_index = webserver_get_load_index();
            load_disc_image(web_index);
            ui_init(s_image_count,
                    web_index < s_image_count ? web_index : 0);
            s_vis_active = false;
        }

        if (webserver_has_page_request()) {
            navigate_page(webserver_get_page_delta());
        }

        // ---- Visualiser tick ----
        // Detect CD-DA playback and switch display from cover art to effects.
        if (da_is_playing()) {
            uint32_t cur_lba = da_get_current_lba();
            const track_t *trk = disc_find_track(&g_disc, cur_lba);
            bool is_audio = trk && (trk->type == TRACK_TYPE_AUDIO);
            da_set_audio_mode(is_audio);

            if (is_audio && vis_audio_get_samples(s_vis_samples)) {
                fft_process(s_vis_samples, s_vis_spectrum, s_vis_peaks, s_vis_waveform);
                effects_render(&s_vis_ctx);
                s_vis_ctx.frame++;
                s_vis_active = true;
            }
        } else if (s_vis_active) {
            // Playback stopped — restore cover art
            s_vis_active = false;
            da_set_audio_mode(false);
            display_show_cover(s_image_paths[s_selected_image]);
        }

        // Only sleep when COMMO was idle this tick — avoids adding 200 µs of
        // latency when Akiko sends back-to-back commands.
        if (!commo_active)
            sleep_us(200);
    }

    return 0;
}
