// =============================================================================
// carousel.c — Multi-disc carousel model + .m3u playlist parser (pure logic)
// =============================================================================
// No FatFS / no pico-sdk dependencies: this compiles unchanged on the host so
// the model and parser are unit-tested directly (tests/host/test_carousel.cpp).
// =============================================================================

#include "carousel.h"
#include <string.h>
#include <ctype.h>

// Last path component of 's' (after the final '/' or '\\').
static const char *basename_of(const char *s) {
    const char *b = s;
    for (const char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\') b = p + 1;
    }
    return b;
}

bool carousel_path_matches(const char *playlist_entry, const char *scan_path) {
    if (!playlist_entry || !scan_path) return false;
    const char *a = basename_of(playlist_entry);
    const char *b = basename_of(scan_path);
    // Case-insensitive compare of the two basenames (FAT/exFAT are case-folding).
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return false;
        if (ca == '\0') return true;   // both reached end together
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static carousel_source_t s_source   = CAROUSEL_ALL;
static uint32_t          s_all_count = 0;                 // CAROUSEL_ALL: [0, count)
static uint32_t          s_entries[MAX_IMAGES];           // CAROUSEL_PLAYLIST: abs indices
static int               s_entry_count = 0;               // playlist length
static int               s_pos       = 0;                 // current position

// ---------------------------------------------------------------------------
// Internal: number of entries in the active source.
// ---------------------------------------------------------------------------
static int active_count(void) {
    return (s_source == CAROUSEL_ALL) ? (int)s_all_count : s_entry_count;
}

// Absolute disc index at a given position (caller guarantees 0 <= pos < count).
static int abs_at(int pos) {
    return (s_source == CAROUSEL_ALL) ? pos : (int)s_entries[pos];
}

// ---------------------------------------------------------------------------
// Init / source selection
// ---------------------------------------------------------------------------
void carousel_use_all(uint32_t total_count) {
    s_source    = CAROUSEL_ALL;
    s_all_count = total_count;
    if (total_count == 0)            s_pos = 0;
    else if (s_pos >= (int)total_count) s_pos = (int)total_count - 1;
}

void carousel_init(uint32_t total_count, uint32_t start_abs_index) {
    s_source      = CAROUSEL_ALL;
    s_all_count   = total_count;
    s_entry_count = 0;
    if (total_count == 0) {
        s_pos = 0;
    } else {
        s_pos = (start_abs_index >= total_count) ? (int)total_count - 1
                                                 : (int)start_abs_index;
    }
}

void carousel_set_playlist(const uint32_t *abs_indices, int count) {
    if (count < 0) count = 0;
    if (count > MAX_IMAGES) count = MAX_IMAGES;
    for (int i = 0; i < count; i++) s_entries[i] = abs_indices[i];
    s_entry_count = count;
    s_source      = CAROUSEL_PLAYLIST;
    s_pos         = 0;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
int carousel_current_abs(void) {
    int n = active_count();
    if (n <= 0) return -1;
    if (s_pos < 0 || s_pos >= n) s_pos = 0;
    return abs_at(s_pos);
}

int carousel_next(void) {
    int n = active_count();
    if (n <= 0) return -1;
    s_pos = (s_pos + 1) % n;          // wrap forward
    return abs_at(s_pos);
}

int carousel_prev(void) {
    int n = active_count();
    if (n <= 0) return -1;
    s_pos = (s_pos - 1 + n) % n;      // wrap backward (no negative modulo)
    return abs_at(s_pos);
}

void carousel_sync_pos_to_abs(uint32_t abs_index) {
    int n = active_count();
    for (int i = 0; i < n; i++) {
        if ((uint32_t)abs_at(i) == abs_index) { s_pos = i; return; }
    }
    // Not part of the active carousel — leave position unchanged.
}

int               carousel_count(void)      { return active_count(); }
int               carousel_pos(void)        { return s_pos; }
carousel_source_t carousel_get_source(void) { return s_source; }

// ---------------------------------------------------------------------------
// .m3u parser
// ---------------------------------------------------------------------------
// One path per line.  '#' or ';' (after leading whitespace) = comment.  Blank
// lines skipped.  CR and LF both end a line.  Leading/trailing spaces trimmed.
int carousel_parse_m3u(const char *text, size_t len,
                       char out[][MAX_PATH_LEN], int cap) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < cap) {
        // Find end of this line (LF, CR, or end of buffer).
        size_t start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        size_t end = i;                 // [start, end) is the raw line
        // Consume the line terminator(s): handle both \n, \r, and \r\n.
        if (i < len && text[i] == '\r') i++;
        if (i < len && text[i] == '\n') i++;

        // Trim leading whitespace.
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        // Trim trailing whitespace.
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;

        if (start == end) continue;                       // blank
        if (text[start] == '#' || text[start] == ';') continue;  // comment

        size_t l = end - start;
        if (l >= MAX_PATH_LEN) l = MAX_PATH_LEN - 1;      // truncate over-long
        memcpy(out[n], &text[start], l);
        out[n][l] = '\0';
        n++;
    }
    return n;
}
