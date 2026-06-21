#pragma once
// =============================================================================
// ui.h — Disc Selector User Interface
// =============================================================================
//
// Implements the disc image selection UI driven by the rotary encoder.
// The UI manages:
//
//   • The currently selected disc index (shown via the LED and USB console)
//   • Processing rotary events and translating them into disc changes
//   • Confirming a selection on button press (loads the disc)
//   • Status LED patterns: blink codes for current selection
//
// INTERACTION MODEL:
//   Turn CW/CCW  — scroll through disc list (wraps around)
//   Short press  — load the highlighted disc
//   Long press   — eject current disc (stops drive, enters idle)
//   Fast spin    — jump by ROTARY_ACCEL_FACTOR entries per click
//
// LED STATUS PATTERNS:
//   1 slow blink / 2 sec   — idle, drive ready
//   Fast blink (100 ms)    — reading/playing
//   N rapid blinks, pause  — N = currently selected image number (1-9)
//                            (shown when scrolling with the encoder)
//   Solid on (1 s)         — disc loading in progress
//   Solid off              — error / no disc
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "rotary.h"

// ---------------------------------------------------------------------------
// How long the "selection preview" display stays active before auto-confirming
// (0 = never auto-confirm; user must press button)
// Set in nebula32.cfg as: ui_autoload_ms = 3000
// ---------------------------------------------------------------------------
#define UI_AUTOLOAD_TIMEOUT_MS    0    // Default: no auto-load; wait for press

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise the UI module.
// Must be called after rotary_init() and after the image list is loaded.
// 'image_count' is the number of disc images found on the SD card.
// 'initial_index' is the first selected index (from saved config).
void ui_init(uint32_t image_count, uint32_t initial_index);

// Process all pending rotary events and update the selected disc.
// Call from the Core 0 main loop (same place as handle_console).
// Returns true if the user confirmed a new disc selection (pressed button).
// When true, call ui_get_selected_index() to find out which disc to load.
bool ui_tick(void);

// Returns the currently highlighted (not yet confirmed) disc index.
uint32_t ui_get_cursor_index(void);

// Returns the last confirmed (loaded) disc index.
uint32_t ui_get_selected_index(void);

// Playlist-menu gesture (hold encoder button + turn).  Drains the accumulated
// signed detent delta; returns true and writes *delta_out when a gesture is
// pending, then clears it.  main.c applies the delta to the playlist cycle.
bool ui_take_playlist_delta(int *delta_out);

// Notify the UI that a disc has been loaded (updates internal state).
void ui_on_disc_loaded(uint32_t index);

// Notify the UI that an error occurred loading a disc.
void ui_on_disc_error(void);

// Update the LED to reflect the current drive state.
// Call from the periodic timer callback.
void ui_update_led(void);

// Print the current disc selection to the USB console.
// Called automatically when the selection changes.
void ui_print_selection(void);

