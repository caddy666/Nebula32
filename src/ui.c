// =============================================================================
// ui.c — Disc Selector User Interface
// =============================================================================

#include "ui.h"
#include "rotary.h"
#include "logger.h"
#include "disc_image.h"   // MAX_PATH_LEN
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <string.h>

#ifndef LED_PIN
#define LED_PIN LED_RED_PIN   /* GPIO 39 — red LED on RM2, direct GPIO */
#endif

// External image list (defined in main.c)
extern char     s_image_paths[][MAX_PATH_LEN];
extern uint32_t s_image_count;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uint32_t  s_image_count_local = 0;
static uint32_t  s_cursor       = 0;    // Highlighted index
static uint32_t  s_loaded       = 0;    // Currently loaded/playing index
static bool      s_needs_load   = false;
static bool      s_error        = false;

// Playlist-menu gesture: "hold the encoder button + turn" cycles playlists
// instead of moving the disc cursor.  Turns-while-held accumulate here; main.c
// drains the delta via ui_take_playlist_delta() and applies it.  s_combo_active
// remembers that a hold+turn happened so the button-release event (PRESS /
// LONG_PRESS) is swallowed instead of loading/ejecting a disc.
static int       s_pl_delta     = 0;
static bool      s_combo_active = false;

// LED blink state
static absolute_time_t s_led_next  = {0};

// Selection preview timer — count blinks showing current number
static bool            s_show_number  = false;  // Show "N blink" feedback
static absolute_time_t s_show_until   = {0};    // When to stop showing number
static int             s_blink_count  = 0;      // Blinks remaining
static int             s_blink_phase  = 0;
#define SHOW_NUMBER_MS   2500   // Show number feedback for 2.5 seconds
#define NUMBER_BLINK_ON  120    // Each blink on-time ms
#define NUMBER_BLINK_OFF  80    // Each blink off-time ms
#define NUMBER_PAUSE     600    // Pause between groups of blinks

// ---------------------------------------------------------------------------
// ui_init
// ---------------------------------------------------------------------------
void ui_init(uint32_t image_count, uint32_t initial_index) {
    s_image_count_local = image_count;
    s_cursor    = (image_count > 0) ? initial_index % image_count : 0;
    s_loaded    = s_cursor;
    s_needs_load = false;
    s_error      = false;

    // LED already initialised in main.c
    printf("[UI] Disc selector ready: %lu images, cursor=%lu\n",
           image_count, s_cursor);
}

// ---------------------------------------------------------------------------
// Internal: start the "number blink" feedback sequence
// Blinks out the 1-based disc number so user knows position without screen
// ---------------------------------------------------------------------------
static void start_number_blink(void) {
    s_show_number = true;
    s_show_until  = make_timeout_time_us((uint64_t)SHOW_NUMBER_MS * 1000);
    // Blink count = cursor+1 (1-based), capped at 9
    s_blink_count = (int)(s_cursor + 1);
    if (s_blink_count > 9) s_blink_count = 9;
    s_blink_phase = 0;
    // Log it
    LOG_INFO_MSG("UI  ", "encoder scroll → image %lu/%lu",
                 (unsigned long)(s_cursor + 1),
                 (unsigned long)s_image_count_local);
}

// ---------------------------------------------------------------------------
// ui_tick — called from main loop
// ---------------------------------------------------------------------------
bool ui_tick(void) {
    if (s_image_count_local == 0) return false;

    s_needs_load = false;
    int steps;
    rotary_event_t ev;

    while ((ev = rotary_poll(&steps)) != ROTARY_NONE) {

        // Playlist-menu gesture: a turn while the button is held cycles
        // playlists rather than discs.  Accumulate the signed step count and
        // skip the normal cursor movement; main.c applies it next loop.
        if ((ev == ROTARY_CW || ev == ROTARY_CCW) && rotary_button_held()) {
            s_pl_delta    += (ev == ROTARY_CW) ? steps : -steps;
            s_combo_active = true;
            printf("[UI] Playlist gesture: delta %+d\n", s_pl_delta);
            continue;
        }

        switch (ev) {

        case ROTARY_CW:
            // Scroll forward through the list (wraps)
            s_cursor = (s_cursor + (uint32_t)steps) % s_image_count_local;
            start_number_blink();
            ui_print_selection();
            break;

        case ROTARY_CCW:
            // Scroll backward with wrap — unified modular form avoids the OOB
            // case where (steps - cursor) % count == 0 would yield count itself.
            s_cursor = (s_cursor + s_image_count_local
                        - (uint32_t)steps % s_image_count_local)
                       % s_image_count_local;
            start_number_blink();
            ui_print_selection();
            break;

        case ROTARY_PRESS:
            // If this release ends a hold+turn playlist gesture, swallow it so
            // it doesn't also load a disc.
            if (s_combo_active) { s_combo_active = false; break; }
            // Short press = load selected disc
            if (s_cursor != s_loaded || s_error) {
                printf("[UI] Button pressed — loading disc %lu\n",
                       (unsigned long)(s_cursor + 1));
                LOG_INFO_MSG("UI  ", "button press: load image %lu",
                             (unsigned long)(s_cursor + 1));
                s_needs_load = true;
                s_show_number = false;
            } else {
                printf("[UI] Button pressed — disc %lu already loaded\n",
                       (unsigned long)(s_cursor + 1));
            }
            break;

        case ROTARY_LONG_PRESS:
            // A held button that was also turned is the playlist gesture, not an
            // eject — swallow the release.
            if (s_combo_active) { s_combo_active = false; break; }
            // Long press = eject disc / return to idle
            printf("[UI] Long press — ejecting disc\n");
            LOG_INFO_MSG("UI  ", "long press: disc ejected");
            // Caller handles eject via ui_get_selected_index returning UINT32_MAX
            s_needs_load = true;
            s_cursor     = UINT32_MAX;  // Sentinel for "eject"
            break;

        case ROTARY_LOG_PRESS: {
            bool now_log = !logger_is_enabled();
            logger_set_enabled(now_log);
            if (!now_log) logger_flush();
            printf("[UI] Logger button: %s\n", now_log ? "ENABLED" : "DISABLED (flushed)");
            break;
        }

        default:
            break;
        }
    }

    return s_needs_load;
}

// ---------------------------------------------------------------------------
// ui_get_cursor_index / ui_get_selected_index
// ---------------------------------------------------------------------------
uint32_t ui_get_cursor_index(void)   { return s_cursor; }
uint32_t ui_get_selected_index(void) { return s_cursor; }

// Drain the accumulated playlist-gesture delta (signed detent count from
// hold+turn).  Returns true and writes *delta_out when non-zero, then clears
// it.  main.c calls this each loop and applies the delta to the playlist cycle.
bool ui_take_playlist_delta(int *delta_out) {
    if (s_pl_delta == 0) return false;
    if (delta_out) *delta_out = s_pl_delta;
    s_pl_delta = 0;
    return true;
}

void ui_on_disc_loaded(uint32_t index) {
    s_loaded = index;
    s_error  = false;
    s_show_number = false;
    printf("[UI] Disc %lu loaded\n", (unsigned long)(index + 1));
}

void ui_on_disc_error(void) {
    s_error = true;
    printf("[UI] Disc load error\n");
}

// ---------------------------------------------------------------------------
// ui_print_selection — show the selection list in the USB console
// ---------------------------------------------------------------------------
void ui_print_selection(void) {
    printf("\n[UI] ─── Disc Selection ──────────────────────\n");
    // Show a window of ±3 entries around the cursor
    int32_t win_start = (int32_t)s_cursor - 3;
    int32_t win_end   = (int32_t)s_cursor + 3;
    if (win_start < 0) win_start = 0;
    if (win_end >= (int32_t)s_image_count_local)
        win_end = (int32_t)s_image_count_local - 1;

    if (win_start > 0)
        printf("[UI]   ... (%ld more above)\n", (long)win_start);

    for (int32_t i = win_start; i <= win_end; i++) {
        bool is_cursor  = ((uint32_t)i == s_cursor);
        bool is_loaded  = ((uint32_t)i == s_loaded);
        const char *prefix = is_cursor ? "▶ " : "  ";
        const char *suffix = is_loaded ? " ← loaded" : "";
        // Extract just the filename from the full path
        const char *path = s_image_paths[i];
        const char *slash = strrchr(path, '/');
        const char *name  = slash ? slash + 1 : path;
        printf("[UI] %s%2ld: %s%s\n", prefix, (long)(i + 1), name, suffix);
    }

    int32_t remaining = (int32_t)s_image_count_local - 1 - win_end;
    if (remaining > 0)
        printf("[UI]   ... (%ld more below)\n", (long)remaining);

    printf("[UI] ─────────────────────────────────────────\n\n");
}

// ---------------------------------------------------------------------------
// ui_update_led — call from 1 ms timer callback
// ---------------------------------------------------------------------------
// LED blink patterns:
//   Number blink active: rapid blink-blink pattern (N blinks then pause)
//   Loading:             solid ON
//   Reading/playing:     fast 100 ms blink
//   Idle/ready:          slow 500 ms blink
//   Error:               double-blink then long pause
void ui_update_led(void) {
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, s_led_next) > 0) return;

    // ---- Number blink pattern ----
    if (s_show_number) {
        // Check if the display window expired
        if (absolute_time_diff_us(now, s_show_until) <= 0) {
            s_show_number = false;
            s_blink_phase = 0;
            gpio_put(LED_PIN, 0);
            s_led_next = make_timeout_time_us(200000);
            return;
        }

        if (s_blink_phase == 0) {
            // Starting a new group of blinks
            if (s_blink_count > 0) {
                gpio_put(LED_PIN, 1);
                s_blink_phase = 1;
                s_led_next = make_timeout_time_us((uint64_t)NUMBER_BLINK_ON * 1000);
            } else {
                // Done with this number — long pause before repeating
                s_blink_count = (int)(s_cursor + 1);
                if (s_blink_count > 9) s_blink_count = 9;
                gpio_put(LED_PIN, 0);
                s_led_next = make_timeout_time_us((uint64_t)NUMBER_PAUSE * 1000);
            }
        } else {
            // Off phase
            gpio_put(LED_PIN, 0);
            s_blink_count--;
            s_blink_phase = 0;
            s_led_next = make_timeout_time_us((uint64_t)NUMBER_BLINK_OFF * 1000);
        }
        return;
    }

    // ---- Normal LED patterns (delegated to main.c's periodic_update_cb) ----
    // (main.c handles the reading/idle/error LED patterns)
    // ui_update_led only overrides when the number-blink sequence is active.
}
