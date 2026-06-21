#pragma once
// =============================================================================
// carousel.h — Multi-disc carousel model + .m3u playlist parser
// =============================================================================
//
// Pure-logic core (no FatFS, no pico-sdk) so it is unit-testable on the host.
// The firmware supplies the disc list and performs the actual swap; this module
// only tracks the ordered set of selectable discs and the current position.
//
// Two sources:
//   CAROUSEL_ALL      — every scanned disc, in scan order (entry i == abs index i).
//   CAROUSEL_PLAYLIST — a curated subset: an explicit list of absolute indices.
//
// Navigation (carousel_next/prev) returns the ABSOLUTE disc index to load, or -1
// when the carousel is empty.  It wraps at both ends.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Host builds may not see the firmware -D defines; provide safe fallbacks.
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 256
#endif
#ifndef MAX_IMAGES
#define MAX_IMAGES   32
#endif

typedef enum {
    CAROUSEL_ALL      = 0,
    CAROUSEL_PLAYLIST = 1,
} carousel_source_t;

// Initialise as CAROUSEL_ALL over [0, total_count), positioned at start_abs_index
// (clamped into range).  Call once after the SD scan; safe to call again to reset.
void carousel_init(uint32_t total_count, uint32_t start_abs_index);

// Switch to CAROUSEL_ALL over [0, total_count), keeping position if still valid.
void carousel_use_all(uint32_t total_count);

// Switch to CAROUSEL_PLAYLIST from an explicit list of absolute disc indices
// (count clamped to MAX_IMAGES).  Position resets to 0.  An empty list leaves the
// carousel empty (navigation returns -1).
void carousel_set_playlist(const uint32_t *abs_indices, int count);

// Parse an in-memory .m3u into trimmed path strings.  Pure function — the FatFS
// file read lives in the firmware caller.  Rules: lines beginning '#' or ';'
// (after leading whitespace) are comments; blank lines are skipped; CR and LF
// are both accepted as line endings; leading/trailing spaces are trimmed; entries
// beyond 'cap' are dropped.  Returns the number of entries written to 'out'.
int carousel_parse_m3u(const char *text, size_t len,
                       char out[][MAX_PATH_LEN], int cap);

// Navigation — return the absolute disc index to load next/previous, or -1 if
// the carousel is empty.  Both wrap around.
int carousel_next(void);
int carousel_prev(void);

// Absolute index at the current position, or -1 if empty.
int carousel_current_abs(void);

// Sync the carousel position to a disc that was loaded by another path (UI/web),
// so a following next/prev continues from the right place.  No-op if abs_index
// is not part of the active carousel.
void carousel_sync_pos_to_abs(uint32_t abs_index);

int               carousel_count(void);
int               carousel_pos(void);
carousel_source_t carousel_get_source(void);

// Match a playlist entry against a scanned disc path by BASENAME, case-insensitive.
// The .m3u may list a bare filename or a path; the scan stores full paths.  We
// compare only the final path component so "Game.iso", "covers/Game.iso", and
// "0:/cd/Game.iso" all match a scan entry ending in "game.iso".  Pure — host-tested.
bool carousel_path_matches(const char *playlist_entry, const char *scan_path);
