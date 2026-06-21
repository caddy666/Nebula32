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
// BOOT SEQUENCE (Core2350B0):
//   1. Clock RP2350B to 135,475,200 Hz (= 16.9344 MHz × 8 — exact DA timing)
//   2. Stdio init (USB CDC + UART0 GPIO 16/17)
//   3. Load persistent config from flash
//   4. Mount SD card via 4-bit SDIO (GPIO 30-35); scan disc images
//   5. Initialise logger (reads nebula32.cfg, opens cd32_cd.log)
//   6. Open selected disc image; initialise sector_cache
//   7. Initialise DA output PIO (GPIO 0-2) — starts clocking I2S to Akiko
//   8. Initialise subcode encoder PIO (GPIO 5-8)
//   9. Initialise UART1 auxiliary serial (GPIO 20-21)
//  10. Initialise ST7789 display (SPI1, GPIO 40-43)
//  11. Initialise direct GPIO rotary encoder (GPIO 12/15/18/19)
//  12. Initialise COMMO bus bridge (PIO1 SM0/SM1, GPIO 44-46)
//  13. Launch Core 1 (sector prefetch)
//  14. Core 0 enters main polling loop
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
#include "carousel.h"
#include "ff.h"           // FatFS — read 0:/playlists/*.m3u
#include "webserver.h"
#include "commo_bridge.h"
#include "psram.h"
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
/* [anchor:sysclk] */
#define TARGET_SYS_CLK_KHZ  135475

// =============================================================================
// Global state (extern'd by commo_bridge.c and webserver.c)
// =============================================================================
disc_image_t   g_disc;   // Currently open disc image
sector_cache_t g_cache;  // Read-ahead sector ring buffer

// True once Core 1 (the prefetch loop) is launched and has armed its lockout
// victim handler.  disc_swap_locked() only parks Core 1 when this is set —
// multicore_lockout_start_blocking() would hang forever if the victim core was
// never launched, so swaps that happen before launch (boot) run unlocked, which
// is safe because Core 1 isn't touching g_disc/g_cache yet.
static volatile bool s_core1_running = false;

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
// disc_swap_locked — mutate the shared disc/cache with Core 1 parked
// ---------------------------------------------------------------------------
// Core 1 runs sector_cache_prefetch_tick(&g_cache) continuously and may be inside
// disc_read_sector() / f_read() at any instant.  Closing the disc out from under
// it (f_close on the live FatFS handle) corrupts the read in flight — that is the
// race this function exists to remove.  We reuse the same multicore_lockout the
// flash writer uses; Core 1 armed the victim handler via
// multicore_lockout_victim_init() in core1_main().  Before launch the lockout is
// skipped (it would hang waiting for a victim, and Core 1 isn't touching the disc
// yet — see s_core1_running).
//
//   path == NULL  → eject: close the disc, flush the cache
//   path != NULL  → load:  close, open(path), re-init the cache
// Returns false only if disc_open(path) fails (disc left closed; cache flushed so
// Core 1 never serves sectors of a disc that is no longer open).
static bool disc_swap_locked(const char *path) {
    bool ok = true;
    if (s_core1_running) multicore_lockout_start_blocking();
    disc_close(&g_disc);
    if (path) {
        ok = disc_open(&g_disc, path);
        if (ok) sector_cache_init(&g_cache, &g_disc);
        else    sector_cache_flush(&g_cache);
    } else {
        sector_cache_flush(&g_cache);
    }
    if (s_core1_running) multicore_lockout_end_blocking();
    return ok;
}

// ---------------------------------------------------------------------------
// load_disc_image — switch to a different disc image
// ---------------------------------------------------------------------------
static bool load_disc_image(uint32_t index) {
    if (index == UINT32_MAX) {
        printf("[MAIN] Ejecting disc\n");
        LOG_INFO_MSG("MAIN", "disc ejected");
        da_stop();
        commo_bridge_signal_eject();   // tell Akiko: DISC bit clear (no-op if no host)
        disc_swap_locked(NULL);        // Core 1 parked during close + flush
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
    commo_bridge_signal_eject();   // eject the outgoing disc (DISC bit clear)
    if (!disc_swap_locked(s_image_paths[index])) {   // Core 1 parked during close/open/init
        printf("[MAIN] ERROR: Failed to open %s\n", s_image_paths[index]);
        LOG_ERROR_MSG("Failed to open image %lu", (unsigned long)index);
        ui_on_disc_error();
        return false;
    }
    commo_bridge_signal_insert();  // insert the new disc → Akiko re-reads the TOC

    s_selected_image = index;

    uint16_t abs_index = (uint16_t)(s_page_offset + index);
    carousel_sync_pos_to_abs(abs_index);   // keep carousel aligned with UI/web loads
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

// ---------------------------------------------------------------------------
// carousel_swap_to — load an ABSOLUTE disc index, paging it in if needed
// ---------------------------------------------------------------------------
// The carousel works in absolute indices over the whole SD scan; load_disc_image
// works in page-local indices.  This bridges them: it pages the target window in
// if necessary, then routes through load_disc_image so the disc-change event and
// the Core-1 lockout swap apply uniformly.  Used by console n/p navigation (and,
// in 3b, the rotary/web carousel controls).
static bool carousel_swap_to(uint32_t abs_index) {
    if (abs_index >= s_total_count) return false;
    if (abs_index < s_page_offset || abs_index >= s_page_offset + s_image_count) {
        load_image_page((abs_index / PAGE_SIZE) * PAGE_SIZE);
        ui_init(s_image_count, 0);
    }
    return load_disc_image(abs_index - s_page_offset);
}

// ---------------------------------------------------------------------------
// disc_is_all_audio — true if every track on the open disc is CD-DA audio
// ---------------------------------------------------------------------------
// Jukebox auto-advance only fires on pure audio discs: advancing a data disc
// out from under a running game would crash it.
static bool disc_is_all_audio(void) {
    if (g_disc.first_track == 0 || g_disc.last_track == 0) return false;
    for (int t = g_disc.first_track; t <= g_disc.last_track; t++) {
        if (g_disc.tracks[t - 1].type != TRACK_TYPE_AUDIO) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Playlist activation (.m3u on 0:/playlists/)  — Phase 3b
// ---------------------------------------------------------------------------
#define PLAYLIST_DIR  "0:/playlists/"

// Static (not stack): a parsed playlist is up to MAX_IMAGES × MAX_PATH_LEN, and
// the .m3u file buffer is a few KB — too large for the Core 0 stack.
static char s_pl_paths[MAX_IMAGES][MAX_PATH_LEN];
static char s_pl_filebuf[4096];

// Resolve parsed playlist entries to absolute scan indices, preserving playlist
// order and skipping entries not on the card.  Pages the scan through the global
// s_image_paths buffer (CLOBBERED — callers reload the correct page afterward,
// which carousel_swap_to / load_image_page do).  Returns resolved count.
static int playlist_resolve(int pl_count, uint32_t *abs_out) {
    for (int p = 0; p < pl_count; p++) abs_out[p] = UINT32_MAX;
    const char *base = logger_get_config()->sdcard_base;
    for (uint32_t off = 0; off < s_total_count; off += PAGE_SIZE) {
        uint32_t n = sd_scan_images(s_image_paths, PAGE_SIZE, base, off);
        for (uint32_t i = 0; i < n; i++) {
            for (int p = 0; p < pl_count; p++) {
                if (abs_out[p] == UINT32_MAX &&
                    carousel_path_matches(s_pl_paths[p], s_image_paths[i])) {
                    abs_out[p] = off + i;
                }
            }
        }
    }
    int found = 0;
    for (int p = 0; p < pl_count; p++)
        if (abs_out[p] != UINT32_MAX) abs_out[found++] = abs_out[p];
    return found;
}

// Activate a playlist by name (no dir/ext).  Reads PLAYLIST_DIR<name>.m3u, parses,
// resolves, switches the carousel to PLAYLIST.  Returns resolved entry count;
// 0 (missing/empty/none-resolved) leaves the carousel unchanged.
static int playlist_activate(const char *name) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%s.m3u", PLAYLIST_DIR, name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("[PL] playlist not found: %s\n", path);
        return 0;
    }
    UINT br = 0;
    f_read(&f, s_pl_filebuf, sizeof(s_pl_filebuf) - 1, &br);
    f_close(&f);
    s_pl_filebuf[br] = '\0';

    int pn = carousel_parse_m3u(s_pl_filebuf, br, s_pl_paths, MAX_IMAGES);
    uint32_t abs[MAX_IMAGES];
    int found = playlist_resolve(pn, abs);
    if (found > 0) {
        carousel_set_playlist(abs, found);
        printf("[PL] playlist '%s': %d/%d entries resolved\n", name, found, pn);
    } else {
        printf("[PL] playlist '%s': nothing resolved (kept current source)\n", name);
    }
    return found;
}

// Switch the carousel back to all discs, aligned to the currently loaded disc.
static void playlist_use_all(void) {
    carousel_use_all(s_total_count);
    carousel_sync_pos_to_abs(s_page_offset + s_selected_image);
    printf("[PL] carousel source: all discs (%lu)\n", (unsigned long)s_total_count);
}

// ---------------------------------------------------------------------------
// Playlist menu — the rotary "hold + turn" gesture cycles through this list.
// ---------------------------------------------------------------------------
// The menu is a cycle of slots:  slot 0 == "all discs", slots 1..N == the
// .m3u playlists found in 0:/playlists/ (base names, no extension).  The rotary
// gesture (ui_take_playlist_delta) moves s_pl_menu_pos through this cycle.
#define PL_MENU_MAX  8
static char s_pl_menu[PL_MENU_MAX][32];  // playlist base names (slot 0 unused)
static int  s_pl_menu_count = 0;         // number of .m3u names cached (0..PL_MENU_MAX-1)
static int  s_pl_menu_pos   = 0;         // 0 = all discs, 1.. = s_pl_menu[pos-1]

// Debounced persistence: a fast spin through the menu changes g_config many
// times, but flash is wear-limited.  cycle_playlist() updates g_config in RAM
// immediately and only ARMS a save; playlist_save_tick() commits it once the
// knob has been still for the debounce window.  Each new detent pushes the
// deadline out, so one spin = exactly one flash write.  The window is the
// user-tunable nebula32.cfg key `playlist_save_ms` (0 = save immediately).
static bool            s_pl_save_pending = false;
static absolute_time_t s_pl_save_due;

// Re-scan 0:/playlists/ and cache the playlist base names.  Slot count excludes
// the implicit "all discs" slot 0.  Call at boot and whenever the SD set changes.
static void playlist_menu_refresh(void) {
    static char paths[PL_MENU_MAX][MAX_PATH_LEN];
    uint32_t n = sd_scan_m3u_files(paths, PL_MENU_MAX, PLAYLIST_DIR, 0);
    s_pl_menu_count = 0;
    for (uint32_t i = 0; i < n && s_pl_menu_count < PL_MENU_MAX - 1; i++) {
        // Strip directory and ".m3u" → bare name that playlist_activate() wants.
        const char *sl = strrchr(paths[i], '/');
        const char *nm = sl ? sl + 1 : paths[i];
        snprintf(s_pl_menu[s_pl_menu_count], sizeof(s_pl_menu[0]), "%s", nm);
        char *dot = strrchr(s_pl_menu[s_pl_menu_count], '.');
        if (dot) *dot = '\0';
        s_pl_menu_count++;
    }
    printf("[PL] menu: %d playlist(s) + all discs\n", s_pl_menu_count);
}

// cycle_playlist — move through the playlist menu by 'delta' detents and apply
// the newly selected source.  Called from the main loop when the rotary
// hold+turn gesture fires (delta is the signed detent count, may be >1).
//
// Slots: 0 == "all discs", 1..s_pl_menu_count == s_pl_menu[slot-1].  Movement
// wraps at both ends.  The carousel source switches immediately; the choice is
// only ARMED for persistence here (debounced — see playlist_save_tick()), so a
// fast spin coalesces into a single flash write.
//
// Constraints: runs in main-loop context (carousel_swap_to uses multicore_lockout
// — legal here, NOT in an ISR).  If s_pl_menu_count == 0 there is only slot 0;
// the gesture should be a harmless no-op (stay on "all discs").
static void cycle_playlist(int delta) {
    if (s_pl_menu_count <= 0) return;  // only "all discs" exists — nothing to cycle

    // 1. Wrap-safe slot move.  Slots: 0 == all discs, 1..count == playlists.
    //    Positive modulo — a bare % goes negative for delta<0 and would index OOB.
    int slots = s_pl_menu_count + 1;
    s_pl_menu_pos = ((s_pl_menu_pos + delta) % slots + slots) % slots;

    // 2. Apply the selected source.  g_config updates in RAM now; the flash
    //    write is debounced (armed below) so a fast spin = one write.
    if (s_pl_menu_pos == 0) {
        playlist_use_all();
        g_config.active_playlist[0] = '\0';
    } else {
        const char *name = s_pl_menu[s_pl_menu_pos - 1];
        if (playlist_activate(name) > 0) {
            snprintf(g_config.active_playlist, sizeof(g_config.active_playlist),
                     "%s", name);
        } else {
            // Activation failed (missing/empty/none-resolved); fall back to all
            // discs so the menu position never points at a dead source.
            s_pl_menu_pos = 0;
            playlist_use_all();
            g_config.active_playlist[0] = '\0';
        }
    }
    // Arm (or re-arm) the debounced save — each detent pushes the deadline out.
    // Window is the user-set playlist_save_ms (0 → fires next tick = immediate).
    uint32_t debounce_ms = logger_get_config()->playlist_save_ms;
    s_pl_save_pending = true;
    s_pl_save_due     = make_timeout_time_us((uint64_t)debounce_ms * 1000);
    printf("[PL] gesture → slot %d/%d (%s)\n", s_pl_menu_pos, slots - 1,
           s_pl_menu_pos == 0 ? "all discs" : s_pl_menu[s_pl_menu_pos - 1]);

    // 3. Re-align the carousel to its current disc and load it.
    int abs = carousel_current_abs();
    if (abs >= 0) { carousel_swap_to((uint32_t)abs); s_vis_active = false; }
}

// playlist_save_tick — commit a debounced playlist choice once the knob has been
// still for PL_SAVE_DEBOUNCE_US.  Called every main-loop iteration; cheap no-op
// when nothing is pending.  Keeps flash writes off the per-detent hot path.
static void playlist_save_tick(void) {
    if (!s_pl_save_pending) return;
    if (absolute_time_diff_us(get_absolute_time(), s_pl_save_due) > 0) return;
    s_pl_save_pending = false;
    config_save(&g_config);
    printf("[PL] playlist choice persisted\n");
}

// =============================================================================
// Core 1 — sector prefetch loop
// =============================================================================
/* [anchor:core1_prefetch] */
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
    else if (c == 'n' || c == 'N') {
        int abs = carousel_next();   // next disc in the carousel (wraps)
        if (abs >= 0) carousel_swap_to((uint32_t)abs);
    }
    else if (c == 'p' || c == 'P') {
        int abs = carousel_prev();   // previous disc in the carousel (wraps)
        if (abs >= 0) carousel_swap_to((uint32_t)abs);
    }
    else if (c == 'j' || c == 'J') {
        g_config.auto_advance = g_config.auto_advance ? 0 : 1;   // toggle jukebox
        config_save(&g_config);                                  // persist across power cycles
        printf("[MAIN] Jukebox auto-advance: %s\n",
               g_config.auto_advance ? "ON" : "OFF");
    }
    else if (c == 'a' || c == 'A') {
        playlist_use_all();                       // carousel = all discs
        g_config.active_playlist[0] = '\0';
        config_save(&g_config);
        int ab = carousel_current_abs();
        if (ab >= 0) carousel_swap_to((uint32_t)ab);
    }
    else if (c == 'm' || c == 'M') {
        // Demo hook: activate 0:/playlists/default.m3u.  The web endpoint
        // /api/playlist/set picks an arbitrary name; console uses a fixed one.
        if (playlist_activate("default") > 0) {
            snprintf(g_config.active_playlist, sizeof(g_config.active_playlist),
                     "%s", "default");
            config_save(&g_config);
            int ab = carousel_current_abs();
            if (ab >= 0) carousel_swap_to((uint32_t)ab);
        }
    }
    else if (c == 'P') {
        // Step the playlist menu forward one slot — the console equivalent of the
        // rotary hold+turn gesture, so the cycle path is testable without the
        // encoder.  Lower-case 'p' stays the carousel-prev key above.
        cycle_playlist(+1);
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
        printf("  n/p  — next / previous disc in the carousel (wraps, fires disc-change)\n");
        printf("  j    — toggle jukebox auto-advance (audio discs; persisted)\n");
        printf("  m/a  — carousel source: m=playlist 'default.m3u'  a=all discs (persisted)\n");
        printf("  P    — step playlist menu forward (same as rotary hold+turn; persisted)\n");
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
/* [anchor:boot_sequence] */
int main(void) {
    // ---- Set system clock to 135,475,200 Hz ----
    // Exact multiple of the Sony 16.9344 MHz master clock × 8.
    // Required for PIO clkdiv = 32 → BCLK = 2,117,112 Hz (1× CD speed).
    if (!set_sys_clock_khz(TARGET_SYS_CLK_KHZ, true)) {
        // If the exact target isn't reachable, try the nearest available.
        // Timing will be slightly off but the PLL will get as close as possible.
    }

    // Initialise QSPI PSRAM on CS1 before any psram_alloc() call.
    // No-op when BUILD_WITH_PSRAM is not set.
    psram_fw_init();

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
        display_init();
        display_show_text("SD CARD", "MOUNT FAILED");
        while (true) tight_loop_contents();
    }

    // Logger init reads nebula32.cfg (including sdcard_base) before the image
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
        display_init();
        display_show_text("NO IMAGES", "CHECK SD CARD");
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

    // ---- Carousel: default to all discs, positioned at the starting image ----
    carousel_init(s_total_count, abs_start);
    playlist_menu_refresh();   // cache 0:/playlists/*.m3u for the rotary gesture
    // Restore a saved playlist, if any.  playlist_activate() pages the scan and
    // clobbers s_image_paths, so reload the start page afterward for the UI.
    if (g_config.active_playlist[0] != '\0') {
        if (playlist_activate(g_config.active_playlist) > 0)
            carousel_sync_pos_to_abs(abs_start);
        load_image_page(start_page);
    }

    printf("[MAIN] Opening: %s\n", s_image_paths[s_selected_image]);
    if (!disc_open(&g_disc, s_image_paths[s_selected_image])) {
        printf("[MAIN] FATAL: Cannot open disc image\n");
        display_init();
        display_show_text("DISC OPEN", "FAILED");
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

    /* [anchor:da_pio_init] */
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

    /* [anchor:m17sine_clk] */
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
            // clk_peri slaving DISABLED (easy to re-enable — uncomment below).
            // It does NOT sync the DA audio: the I2S bit stream is a PIO program
            // clocked from clk_sys, and da_nudge_clkdiv_to_m17sine() (main loop) is
            // the real phase lock.  Slaving clk_peri to 16.9344 MHz only throttles
            // UART/SPI and silently corrupts the UART0 debug baud, because
            // clock_configure() does not re-derive any peripheral's divisor.
            // If re-enabled, KEEP the uart_set_baudrate() re-derive after this block.
            // clock_configure(clk_peri, 0,
            //                 CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_GPIN0,
            //                 16934400, 16934400);
            printf("[MAIN] M17SINE present on GPIO%d (%lu kHz) — clk_peri left on PLL; "
                   "audio locked via DA clkdiv trim\n",
                   M17SINE_PIN, (unsigned long)m17_khz);
        } else {
            printf("[MAIN] M17SINE not detected on GPIO%d (bench mode)\n",
                   M17SINE_PIN);
        }
    }

    // Re-derive UART0 (stdio debug) baud from the current clk_peri.  A no-op while
    // clk_peri stays on the PLL; essential if the clk_peri slave above is ever
    // re-enabled, since clock_configure() leaves peripheral divisors stale.
    uart_set_baudrate(uart0, 115200);

    // ---- UART1 auxiliary serial (GPIO 20/21) ----
    uart_init(uart1, 115200);
    gpio_set_function(PIN_UART1_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART1_RX, GPIO_FUNC_UART);
    printf("[MAIN] UART1 ready (GPIO%d/GPIO%d @ 115200)\n",
           PIN_UART1_TX, PIN_UART1_RX);

    // ---- ST7789 240×240 display (SPI1) ----
    display_init();

    // O6: If the device just rebooted after a successful firmware update, play the
    // Boing Ball animation.  The watchdog scratch register survives a watchdog reboot
    // but is cleared on a power-cycle, so this fires exactly once after each flash.
    if (watchdog_hw->scratch[0] == FW_UPDATE_MAGIC) {
        watchdog_hw->scratch[0] = 0;  // consume — one-shot
        display_fw_success_animation();
    }

    // ---- Rotary encoder (direct GPIO 12/15/18/19) ----
    rotary_init();

    // ---- Disc selector UI ----
    ui_init(s_image_count, s_selected_image);

    // ---- DOOR / SCOR GPIOs + 8 ms software timer ----
    gpio_init(PIN_DOOR);
    gpio_set_dir(PIN_DOOR, GPIO_IN);
    gpio_pull_up(PIN_DOOR);
    timer_init();

    /* [anchor:commo_bridge_init] */
    // ---- COMMO bus bridge ----
    commo_bridge_init();

    // ---- Self-test prompt (all peripherals now initialised) ----
    // Runs after UART1, COMMO, PSRAM, SD, DA PIO, subcode PIO are all up.
    // selftest_run() reconfigures GPIO 0-8 at the end (pin pull integrity test),
    // which overrides PIO0 — power-cycle required after selftest for normal operation.
    printf("[MAIN] Press '#' within 3 seconds for self-test mode...\n");
    {
        absolute_time_t st_deadline = make_timeout_time_us(3000000);
        while (absolute_time_diff_us(get_absolute_time(), st_deadline) > 0) {
            int c = getchar_timeout_us(100000);
            if (c == '#') {
                selftest_run();
                printf("[MAIN] Self-test complete.  Power-cycle to restore DA output.\n\n");
                display_show_text("SELFTEST DONE", "POWER-CYCLE");
                while (true) tight_loop_contents();   // halt — PIO0 now broken
            }
        }
    }

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
    s_core1_running = true;   // from here on, disc swaps must park Core 1 (lockout)
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

    // P3: arm hardware watchdog — 10 s window kicks at every main-loop iteration.
    // Enabled here (after all slow boot operations including WiFi connect) so
    // the 30 s WiFi timeout and SD scan don't inadvertently trip it.
    watchdog_enable(10000, 1);

    /* [anchor:main_loop] */
    // ---- Core 0 main loop ----
    absolute_time_t s_nudge_next = make_timeout_time_us(2000000);
    absolute_time_t s_vis_next_frame = make_timeout_time_us(0);

    while (true) {
        watchdog_update();  // prevent reboot; stalled main loop → hard reset
        handle_console();

        // M17SINE phase-lock: trim DA clkdiv every 2 s during audio playback
        if (da_is_playing() && absolute_time_diff_us(get_absolute_time(), s_nudge_next) <= 0) {
            m17sine_nudge_tick();
            s_nudge_next = make_timeout_time_us(2000000);
        }

        // Jukebox: when enabled, auto-advance to the next carousel disc at the end
        // of an all-audio disc and keep playing.  da_take_eod_event() is only
        // consumed when both gates pass, so a data disc (game) never triggers it.
        // The swap (and its da_start_play) runs here in the main loop, never in the
        // DMA ISR — multicore_lockout inside the swap is illegal in interrupt context.
        if (g_config.auto_advance && disc_is_all_audio() && da_take_eod_event()) {
            int nx = carousel_next();
            if (nx >= 0 && carousel_swap_to((uint32_t)nx) && disc_is_all_audio()) {
                uint32_t lba = g_disc.tracks[g_disc.first_track - 1].start_lba;
                // The swap just re-initialised the cache; Core 1 hasn't prefetched
                // yet and da_start_play() bails if the first sector misses.  Retry
                // briefly (≤ ~32 ms, main-loop context so blocking is fine) to let
                // Core 1 fill the lead-in before we commit to playing.
                for (int a = 0; a < 16 && !da_is_playing(); a++) {
                    da_start_play(&g_cache, lba);
                    if (!da_is_playing()) sleep_ms(2);
                }
                printf("[MAIN] Jukebox: advanced to disc %d (%s)\n",
                       nx + 1, da_is_playing() ? "playing" : "stalled");
            }
        }

        if (ui_tick()) {
            uint32_t sel = ui_get_selected_index();
            load_disc_image(sel);
            s_vis_active = false;
        }

        // Rotary playlist-menu gesture (hold encoder + turn).  Runs here in the
        // main loop so cycle_playlist()'s carousel_swap_to() is outside any ISR.
        int pl_delta;
        if (ui_take_playlist_delta(&pl_delta)) {
            cycle_playlist(pl_delta);
        }
        playlist_save_tick();   // commit a debounced playlist choice when settled

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

        // ---- Web carousel / playlist requests (Phase 3b) ----
        // Consumed here (not in the lwIP callback) so the swap/activation runs on
        // the main loop where multicore_lockout is legal.
        if (webserver_has_carousel_request()) {
            int abs = (webserver_get_carousel_delta() >= 0) ? carousel_next()
                                                            : carousel_prev();
            if (abs >= 0) { carousel_swap_to((uint32_t)abs); s_vis_active = false; }
        }
        if (webserver_has_playlist_request()) {
            const char *name = webserver_get_playlist_name();
            if (name[0] == '\0') {
                playlist_use_all();
                g_config.active_playlist[0] = '\0';
            } else if (playlist_activate(name) > 0) {
                snprintf(g_config.active_playlist,
                         sizeof(g_config.active_playlist), "%s", name);
            }
            config_save(&g_config);
            int abs = carousel_current_abs();
            if (abs >= 0) { carousel_swap_to((uint32_t)abs); s_vis_active = false; }
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
                // Cap rendering at 30 fps.  A 256-sample batch lands every
                // ~5.8 ms of audio but a full frame push takes ~15 ms, so
                // unpaced rendering ran back-to-back and starved the
                // webserver/COMMO polls above.  The FFT still runs per batch
                // (cheap at -O3; keeps spectrum/peaks fresh and the ring
                // drained).  frame advances by 2 per rendered frame to keep
                // animation speed at its historical ~60 ticks/s.
                if (absolute_time_diff_us(get_absolute_time(), s_vis_next_frame) <= 0) {
                    effects_render(&s_vis_ctx);
                    s_vis_ctx.frame += 2;
                    s_vis_active = true;
                    s_vis_next_frame = make_timeout_time_us(33333);
                }
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
