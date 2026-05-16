#pragma once
// =============================================================================
// logger.h — SD Card Activity Logger
// =============================================================================
//
// Logs every COMMO command, sector delivery, seek, state transition,
// IRQ event and error to a text file on the SD card: "cd32_cd.log"
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
//   The file "cd32_ode.cfg" in the SD card root controls logger behaviour.
//   It is a plain text file with one KEY=VALUE pair per line.
//   Lines starting with '#' or ';' are comments.  Keys are case-insensitive.
//
//   Supported keys:
//     sdcard_base       = 0:/      # Directory scanned for disc images
//     logging_enabled   = 1        # 1 = on, 0 = off
//     log_commands      = 1        # Log every COMMO command and response
//     log_sectors       = 0        # Log every sector delivery (very verbose)
//     log_seeks         = 1        # Log seek start/complete events
//     log_state         = 1        # Log drive state transitions
//     log_errors        = 1        # Log error conditions
//     log_irq           = 0        # Log every IRQ assertion (very verbose)
//     log_max_kb        = 4096     # Maximum log file size in KB (0 = unlimited)
//
//   Example cd32_ode.cfg:
//     # CD32 ODE settings
//     sdcard_base     = 0:/games/
//     logging_enabled = 1
//     log_commands    = 1
//     log_sectors     = 0
//     log_seeks       = 1
//     log_state       = 1
//     log_errors      = 1
//     log_irq         = 0
//     log_max_kb      = 2048
//
// LOG FILE FORMAT:
//   Each line: [timestamp_ms] LEVEL TAG message
//   Timestamp is milliseconds since boot (wraps at ~49 days).
//
//   Example output:
//     [    142] INFO  CMD  MOTORON
//     [    843] INFO  DRV  state IDLE -> SPINUP
//     [    843] INFO  CMD  MOTORON -> stat=0x02
//     [    1543] INFO  DRV  state SPINUP -> READY
//     [    1544] INFO  CMD  GETSTAT -> stat=0x04
//     [    1545] INFO  CMD  SETMODE mode=0x30 (2x, 2352B)
//     [    1546] INFO  CMD  GETTN -> first=01 last=01
//     [    1547] INFO  CMD  GETTD track=01 -> 00:02:00
//     [    1548] INFO  CMD  SETLOC 00:02:00 (LBA=0)
//     [    1549] INFO  SEEK start LBA=0 from=0 est=80ms
//     [    1549] INFO  CMD  READN -> stat=0x02
//     [    1629] INFO  SEEK complete LBA=0
//     [    1629] INFO  SECT LBA=0 MODE1 2352B -> host
//     [    1642] INFO  SECT LBA=1 MODE1 2352B -> host
//     ...
//     [    2100] WARN  SECT LBA=300 cache miss (retry)
//     [    2101] ERR   DRV  SD read error at LBA=300
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
// Logger configuration (populated from cd32_ode.cfg)
// ---------------------------------------------------------------------------
typedef struct {
    bool     logging_enabled;   // Master on/off switch
    bool     log_commands;      // COMMO command + response bytes
    bool     log_sectors;       // Every sector delivery (verbose!)
    bool     log_seeks;         // Seek start / complete
    bool     log_state;         // Drive state machine transitions
    bool     log_errors;        // All error conditions
    bool     log_irq;           // Every IRQ assertion (very verbose!)
    uint32_t log_max_kb;        // Max log file size KB (0 = unlimited)
    char     sdcard_base[256];  // Base dir for disc image scan (default "0:/")
} logger_config_t;

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
// Reads cd32_ode.cfg from the SD card to determine enabled/disabled state
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

// Toggle logging on/off at runtime (does not modify cd32_ode.cfg).
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

// Log a COMMO command dispatch
// opc       — raw opcode byte
// params    — pointer to parameter bytes (may be NULL)
// n_params  — number of parameter bytes
#define LOG_CMD(opc, params, n_params) \
    do { if (logger_is_enabled() && logger_get_config()->log_commands) { \
        _log_cmd(opc, params, n_params); \
    } } while(0)

// Log a COMMO command response
// opc       — opcode this response is for
// resp      — pointer to response bytes
// n_resp    — number of response bytes
#define LOG_CMD_RESP(opc, resp, n_resp) \
    do { if (logger_is_enabled() && logger_get_config()->log_commands) { \
        _log_cmd_resp(opc, resp, n_resp); \
    } } while(0)

// Log a sector delivery
// lba        — logical block address
// mode_str   — "MODE1", "MODE2", "AUDIO"
// bytes      — bytes delivered
// filtered   — true if not delivered
#define LOG_SECTOR(lba, mode_str, bytes, filtered) \
    do { if (logger_is_enabled() && logger_get_config()->log_sectors) { \
        _log_sector(lba, mode_str, bytes, filtered); \
    } } while(0)

// Log a seek operation start
// from_lba — current head position
// to_lba   — target position
// est_us   — estimated seek time in microseconds
#define LOG_SEEK_START(from_lba, to_lba, est_us) \
    do { if (logger_is_enabled() && logger_get_config()->log_seeks) { \
        _log_seek_start(from_lba, to_lba, est_us); \
    } } while(0)

// Log seek completion
#define LOG_SEEK_DONE(lba) \
    do { if (logger_is_enabled() && logger_get_config()->log_seeks) { \
        logger_write(LOG_INFO, "SEEK", "complete LBA=%lu", (unsigned long)(lba)); \
    } } while(0)

// Log a drive state transition
// old_state / new_state — drive_state_t values
#define LOG_STATE(old_state, new_state) \
    do { if (logger_is_enabled() && logger_get_config()->log_state) { \
        _log_state(old_state, new_state); \
    } } while(0)

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

// Log a debug message (only emitted when log_sectors or log_irq is on,
// since these are the only verbose categories)
#define LOG_DEBUG_MSG(tag, fmt, ...) \
    do { if (logger_is_enabled() && \
             (logger_get_config()->log_sectors || logger_get_config()->log_irq)) { \
        logger_write(LOG_DEBUG, tag, fmt, ##__VA_ARGS__); \
    } } while(0)

// ---------------------------------------------------------------------------
// Internal helpers (called by macros — do not call directly)
// ---------------------------------------------------------------------------
void _log_cmd(uint8_t cmd, const uint8_t *params, uint8_t n_params);
void _log_cmd_resp(uint8_t cmd, const uint8_t *resp, uint8_t n_resp);
void _log_sector(uint32_t lba, const char *mode_str,
                 uint32_t bytes, bool filtered);
void _log_seek_start(uint32_t from_lba, uint32_t to_lba, uint32_t est_us);
void _log_state(int old_state, int new_state);

