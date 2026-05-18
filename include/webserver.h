#pragma once
// =============================================================================
// webserver.h — HTTP Web Interface for Disc Selection
// =============================================================================
//
// Provides a browser-accessible web interface hosted on the Pico 2 W's
// built-in WiFi (CYW43439 chip, accessed via pico_cyw43_arch).
//
// FEATURES:
//   • Lists all disc images found on the SD card
//   • Shows cover art JPEGs (looked up by filename convention, see below)
//   • Lets the user click a disc to load it — equivalent to pressing the
//     rotary encoder button
//   • Shows the currently loaded disc with a "NOW PLAYING" badge
//   • Responsive HTML/CSS — works on phones, tablets, and desktops
//   • REST API for headless control (curl-friendly)
//   • Auto-refresh every 5 seconds to show real-time drive status
//
// COVER ART CONVENTION:
//   For a disc image "0:/Zool2.iso", the cover art JPEG is looked up as:
//     Primary:   "0:/covers/Zool2.jpg"      (same base name, covers/ dir)
//     Fallback:  "0:/covers/Zool2.jpeg"
//     Fallback:  "0:/Zool2.jpg"             (same directory as image)
//     Fallback:  built-in generic CD cover SVG
//
//   Cover art images should be:
//     • JPEG format (.jpg or .jpeg)
//     • 300×300 to 600×600 pixels (shown as 200×200 on the web page)
//     • Named to match the disc image filename (without extension)
//     • Stored in a "covers/" folder in the SD card root
//
//   To create cover art:
//     1. Find the CD cover scan on sites like Hall of Light, TOSEC, or
//        manually photograph the case
//     2. Crop to square, resize to 300×300
//     3. Save as JPEG (quality 85%)
//     4. Copy to the covers/ directory on the SD card
//
// WIFI SETUP:
//   WiFi SSID and password are read from cd32_ode.cfg:
//     wifi_ssid     = MyNetwork
//     wifi_password = MyPassword
//     wifi_hostname = cd32ode
//   After connecting, the IP address is printed to USB serial.
//   mDNS hostname: http://cd32ode.local/ (if your router supports it).
//
// REST API:
//   GET  /api/status        — JSON: current drive state, loaded disc, list
//   GET  /api/images        — JSON: array of {index, name, has_cover} (current page)
//   POST /api/load/{index}  — Load disc at page-local index; returns JSON status
//   POST /api/eject         — Eject current disc
//   POST /api/page/next     — Advance to next page of images
//   POST /api/page/prev     — Go back to previous page of images
//   GET  /covers/{name}.jpg — Serve cover art from SD card
//   GET  /                  — Main HTML page
// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Config keys in cd32_ode.cfg
// ---------------------------------------------------------------------------
#define WS_CFG_SSID_KEY      "wifi_ssid"
#define WS_CFG_PASS_KEY      "wifi_password"
#define WS_CFG_HOSTNAME_KEY  "wifi_hostname"
#define WS_DEFAULT_HOSTNAME  "cd32ode"
#define WS_HTTP_PORT         80

// Max SSID / password lengths
#define WS_SSID_MAX     64
#define WS_PASS_MAX     64
#define WS_HOST_MAX     32

// Cover art search paths
#define WS_COVERS_DIR   "0:/covers/"
#define WS_COVER_EXT1   ".jpg"
#define WS_COVER_EXT2   ".jpeg"

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise WiFi and start the HTTP server.
// Reads wifi_ssid and wifi_password from cd32_ode.cfg (already parsed by
// logger.c's parse_settings_file — we re-read here for the WiFi keys).
// Returns true if WiFi connects and server starts successfully.
// On failure, returns false and the ODE continues operating without WiFi.
bool webserver_init(void);

// Process incoming HTTP connections.
// MUST be called from the Core 0 main loop (not Core 1).
// Uses pico_cyw43_arch_poll() internally (no-OS / poll mode).
// Typically takes < 1 ms when no connections are pending.
void webserver_poll(void);

// Notify the web server that the disc list or drive state has changed.
// This causes the next API response to reflect the new state.
void webserver_notify_state_change(void);

// Return true if WiFi is connected and the server is running.
bool webserver_is_running(void);

// Return the current IP address as a string, or "" if not connected.
const char *webserver_get_ip(void);

// Return true if there is a pending load request from the web interface.
// Clears the flag on return.
bool webserver_has_load_request(void);

// Return the index of the disc requested by the web interface.
// Only valid immediately after webserver_has_load_request() returns true.
uint32_t webserver_get_load_index(void);

// Notify the web server which disc index is currently loaded (for the badge).
void webserver_set_loaded_index(uint32_t index);

// Update the web server's view of the current page position.
// page_offset — absolute index of the first image on this page
// page_count  — number of images on this page (1-PAGE_SIZE)
// total_count — total images on the SD card across all pages
void webserver_set_page_info(uint32_t page_offset, uint32_t page_count,
                             uint32_t total_count);

// Return true if the web interface requested a page change.
// Clears the flag on return.  Call webserver_get_page_delta() to get direction.
bool webserver_has_page_request(void);

// Return the page direction: +1 = next page, -1 = previous page.
// Only valid immediately after webserver_has_page_request() returns true.
int webserver_get_page_delta(void);

