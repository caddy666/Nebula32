#pragma once
// =============================================================================
// logger.h — SD Card Activity Logger
// =============================================================================
//
// Logs informational events, warnings and errors to a text file on the
// SD card: "cd32_cd.log"
//
// DESIGN GOALS:
//   1. Zero overhead when disabled — all log calls resolve to nothing
//   2. Non-blocking — log writes never stall the bus handler or sector DMA
//   3. Configurable via a plain-text settings file on the SD card
//   4. Human-readable output with timestamps and structured fields
//
// ARCHITECTURE:
//   A 4 KB ring buffer in SRAM accumulates log lines.  A flush is triggered
//   from the Core 0 main loop (never from Core 1 or IRQ context) whenever
//   the buffer exceeds a watermark or a flush timer expires (every 2 seconds).
//   This means log writes are batched — typically 50-200 lines per flush —
//   which keeps SD card write latency well away from real-time paths.
//
// SETTINGS FILE:
//   The file "nebula32.cfg" in the SD card root controls logger behaviour.
//   It is a plain text file with one KEY=VALUE pair per line.
//   Lines starting with '#' or ';' are comments.  Keys are case-insensitive.
//
//   Supported keys:
//     sdcard_base       = 0:/      # Directory scanned for disc images
//     logging_enabled   = 1        # 1 = on, 0 = off
//     log_errors        = 1        # Log error conditions
//     log_max_kb        = 4096     # Maximum log file size in KB (0 = unlimited)
//     wifi_ssid         =          # WiFi network name (leave blank to disable WiFi)
//     wifi_password     =          # WiFi password
//     wifi_hostname     = nebula32 # mDNS hostname (access as nebula32.local)
//     fw_token          =          # 32-char hex token required to flash firmware via web UI
//
//   Example nebula32.cfg:
//     # CD32 ODE settings
//     sdcard_base     = 0:/games/
//     logging_enabled = 1
//     log_errors      = 1
//     log_max_kb      = 2048
//
// LOG FILE FORMAT:
//   Each line: [timestamp_ms] LEVEL TAG message
//   Timestamp is milliseconds since boot (wraps at ~49 days).
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "cd_types.h"   // drive_state_t, msf_t

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------
typedef enum {
    LOG_DEBUG = 0,   // Very detailed (sector-by-sector if enabled)
    LOG_INFO  = 1,   // Normal operation events
    LOG_WARN  = 2,   // Recoverable anomalies (cache miss, retry)
    LOG_ERROR = 3,   // Errors that may affect operation
} log_level_t;

// ---------------------------------------------------------------------------
// Logger configuration (populated from nebula32.cfg)
// ---------------------------------------------------------------------------
typedef struct {
    bool     logging_enabled;   // Master on/off switch
    bool     log_errors;        // All error conditions
    uint32_t log_max_kb;        // Max log file size KB (0 = unlimited)
    char     sdcard_base[256];  // Base dir for disc image scan (default "0:/")
    char     wifi_ssid[64];     // WiFi SSID (parsed from nebula32.cfg)
    char     wifi_password[64]; // WiFi password
    char     wifi_hostname[32]; // mDNS hostname (default "nebula32")
    char     fw_token[33];      // 32-char hex token required by /api/fw/flash/
    uint32_t playlist_save_ms;  // Debounce before a playlist-menu choice is
                                // persisted to flash (ms). 0 = save immediately.
                                // Default 1500; clamped to <= 60000.
} logger_config_t;

// Upper bound for playlist_save_ms (sanity clamp on the parsed value).
#define PLAYLIST_SAVE_MS_MAX   60000u
// Default debounce when the key is absent from nebula32.cfg.
#define PLAYLIST_SAVE_MS_DEFAULT  1500u

// ---------------------------------------------------------------------------
// Log ring buffer constants
// ---------------------------------------------------------------------------
#define LOG_RING_SIZE      (4096)   // 4 KB SRAM ring buffer
#define LOG_LINE_MAX       (128)    // Maximum chars per log line
#define LOG_FLUSH_INTERVAL_MS (2000) // Flush to SD at least every 2 s
#define LOG_FLUSH_WATERMARK  (3072)  // Flush when ring buffer > 75% full

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise the logger.
// Reads nebula32.cfg from the SD card to determine enabled/disabled state
// and which categories to log.  Opens (or creates) cd32_cd.log for appending.
// Must be called after the SD card is mounted.
// Returns true if logging is active, false if disabled or file open failed.
bool logger_init(void);

// Flush pending log lines from the ring buffer to the SD card.
// Call from the Core 0 main loop — never from Core 1 or IRQ context.
// Returns the number of bytes written.
uint32_t logger_flush(void);

// Flush only if the flush interval has elapsed or the buffer is near full.
// This is the preferred call from the periodic timer callback.
// Returns bytes written (0 if not yet due).
uint32_t logger_flush_if_due(void);

// Close the log file cleanly (call before power-off if possible).
void logger_close(void);

// Returns true if logging is currently active.
bool logger_is_enabled(void);

// Returns the current logger configuration (read-only copy).
const logger_config_t *logger_get_config(void);

// Toggle logging on/off at runtime (does not modify nebula32.cfg).
void logger_set_enabled(bool enabled);

// ---------------------------------------------------------------------------
// Low-level write function (used by all LOG_* macros below)
// Formats a log line and appends it to the ring buffer.
// level  — LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR
// tag    — Short 4-char tag, e.g. "CMD ", "SECT", "SEEK", "DRV ", "ERR "
// fmt    — printf-style format string
// This function is safe to call from Core 0 only.  Core 1 must not call it
// (no mutex — designed for single-writer use on Core 0).
void logger_write(log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// ---------------------------------------------------------------------------
// Convenience macros — compile to nothing when logging is fully disabled
// ---------------------------------------------------------------------------
// Each macro checks both the compile-time guard and the runtime flag.
// The tag argument must be a 4-char string literal for alignment.

// Log an error
#define LOG_ERROR_MSG(fmt, ...) \
    do { if (logger_is_enabled() && logger_get_config()->log_errors) { \
        logger_write(LOG_ERROR, "ERR ", fmt, ##__VA_ARGS__); \
    } } while(0)

// Log a warning
#define LOG_WARN_MSG(fmt, ...) \
    do { if (logger_is_enabled()) { \
        logger_write(LOG_WARN, "WARN", fmt, ##__VA_ARGS__); \
    } } while(0)

// Log a general informational message
#define LOG_INFO_MSG(tag, fmt, ...) \
    do { if (logger_is_enabled()) { \
        logger_write(LOG_INFO, tag, fmt, ##__VA_ARGS__); \
    } } while(0)


