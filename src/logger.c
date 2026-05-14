// =============================================================================
// logger.c — SD Card Activity Logger
// =============================================================================
//
// Implements buffered, non-blocking text logging to "cd32_cd.log" on the SD
// card, controlled by settings in "cd32_ode.cfg".
//
// THREAD SAFETY:
//   All logger_write() / LOG_* macro calls must come from Core 0 only.
//   Core 1 (the real-time bus handler) never calls any logger function.
//   The logger is explicitly excluded from the hot path.
//
// BUFFERING STRATEGY:
//   Log lines are formatted and appended to a 4 KB SRAM ring buffer.
//   The buffer is drained to the SD card file by logger_flush(), which is
//   called from the Core 0 main loop every 2 seconds (or when the buffer
//   is > 75% full).  A single SD card write of 3-4 KB costs ~2-5 ms —
//   acceptable for the main loop, completely invisible to Core 1.
//
// LOG ROTATION:
//   If log_max_kb > 0 in cd32_ode.cfg, the log file is truncated (a new
//   session marker is written) when it exceeds the size limit.  This
//   prevents filling the SD card over extended play sessions.
// =============================================================================

#include "logger.h"
#include "ff.h"            // FatFS file I/O

#include "pico/stdlib.h"
#include "hardware/timer.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static logger_config_t s_cfg = {
    .logging_enabled = false,
    .log_commands    = true,
    .log_sectors     = false,
    .log_seeks       = true,
    .log_state       = true,
    .log_errors      = true,
    .log_irq         = false,
    .log_max_kb      = 4096,
};

// Ring buffer for deferred SD card writes
static char     s_ring[LOG_RING_SIZE];
static uint32_t s_ring_head  = 0;    // Next byte to write (producer)
static uint32_t s_ring_tail  = 0;    // Next byte to read  (consumer/flush)
static uint32_t s_ring_used  = 0;    // Bytes currently in ring

// FatFS log file handle
static FIL  s_log_file;
static bool s_file_open    = false;

// Session start time (ms since boot) — used for relative timestamps
static uint32_t s_boot_ms = 0;

// Last flush time — used to trigger periodic flushes
static absolute_time_t s_last_flush_time;

// Running count of lines dropped due to ring buffer overflow
static uint32_t s_dropped_lines = 0;

// Log file size tracking
static uint32_t s_log_size_bytes = 0;

// ---------------------------------------------------------------------------
// Settings file parser ("cd32_ode.cfg")
// ---------------------------------------------------------------------------
// Keys are case-insensitive. Values of "1", "yes", "true" are truthy.
// Unknown keys are silently ignored so users can add comments freely.

static bool parse_bool(const char *val) {
    return (strcmp(val, "1")    == 0 ||
            strcasecmp(val, "yes")  == 0 ||
            strcasecmp(val, "true") == 0 ||
            strcasecmp(val, "on")   == 0);
}

static void trim(char *s) {
    // Trim trailing whitespace and newlines in-place
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                        s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
    // Trim leading whitespace by moving pointer — we copy result to start
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static void parse_settings_file(void) {
    FIL cfg_file;
    FRESULT fr = f_open(&cfg_file, "0:/cd32_ode.cfg", FA_READ);
    if (fr != FR_OK) {
        // No settings file — defaults remain in place.
        // Create a template file so the user knows it exists.
        FIL out;
        if (f_open(&out, "0:/cd32_ode.cfg", FA_WRITE | FA_CREATE_NEW) == FR_OK) {
            const char *template_text =
                "# CD32 ODE Settings File\n"
                "# Edit this file on the SD card to configure the drive emulator.\n"
                "# Lines starting with # or ; are comments.\n"
                "# Changes take effect on next boot.\n"
                "#\n"
                "# --- Logging ---\n"
                "logging_enabled = 1\n"
                "log_commands    = 1\n"
                "log_sectors     = 0\n"
                "log_seeks       = 1\n"
                "log_state       = 1\n"
                "log_errors      = 1\n"
                "log_irq         = 0\n"
                "log_max_kb      = 4096\n";
            UINT bw;
            f_write(&out, template_text, strlen(template_text), &bw);
            f_close(&out);
            printf("[LOG] Created default cd32_ode.cfg on SD card\n");
        }
        // Enable logging by default (template was created)
        s_cfg.logging_enabled = true;
        return;
    }

    printf("[LOG] Reading cd32_ode.cfg...\n");
    char line[128];
    while (f_gets(line, sizeof(line), &cfg_file)) {
        trim(line);

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;

        // Split on '='
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        // Parse each known key (case-insensitive key comparison)
        if (strcasecmp(key, "logging_enabled") == 0) {
            s_cfg.logging_enabled = parse_bool(val);
        } else if (strcasecmp(key, "log_commands") == 0) {
            s_cfg.log_commands = parse_bool(val);
        } else if (strcasecmp(key, "log_sectors") == 0) {
            s_cfg.log_sectors = parse_bool(val);
        } else if (strcasecmp(key, "log_seeks") == 0) {
            s_cfg.log_seeks = parse_bool(val);
        } else if (strcasecmp(key, "log_state") == 0) {
            s_cfg.log_state = parse_bool(val);
        } else if (strcasecmp(key, "log_errors") == 0) {
            s_cfg.log_errors = parse_bool(val);
        } else if (strcasecmp(key, "log_irq") == 0) {
            s_cfg.log_irq = parse_bool(val);
        } else if (strcasecmp(key, "log_max_kb") == 0) {
            s_cfg.log_max_kb = (uint32_t)atoi(val);
        }
        // Unknown keys are silently ignored
    }
    f_close(&cfg_file);

    printf("[LOG] Settings: logging=%d cmds=%d sectors=%d seeks=%d "
           "state=%d errors=%d irq=%d max=%luKB\n",
           s_cfg.logging_enabled,
           s_cfg.log_commands,
           s_cfg.log_sectors,
           s_cfg.log_seeks,
           s_cfg.log_state,
           s_cfg.log_errors,
           s_cfg.log_irq,
           s_cfg.log_max_kb);
}

// ---------------------------------------------------------------------------
// Ring buffer helpers
// ---------------------------------------------------------------------------

// Append 'len' bytes from 'data' to the ring buffer.
// Drops the data and increments s_dropped_lines if there is no space.
static void ring_append(const char *data, uint32_t len) {
    if (len == 0) return;

    uint32_t free_space = LOG_RING_SIZE - s_ring_used;
    if (len > free_space) {
        // Not enough space — drop this line
        s_dropped_lines++;
        return;
    }

    // Copy bytes into ring buffer, wrapping at LOG_RING_SIZE
    for (uint32_t i = 0; i < len; i++) {
        s_ring[s_ring_head] = data[i];
        s_ring_head = (s_ring_head + 1) % LOG_RING_SIZE;
    }
    s_ring_used += len;
}

// ---------------------------------------------------------------------------
// logger_init
// ---------------------------------------------------------------------------
bool logger_init(void) {
    s_boot_ms = to_ms_since_boot(get_absolute_time());
    s_last_flush_time = get_absolute_time();

    // Parse settings file from SD card
    parse_settings_file();

    if (!s_cfg.logging_enabled) {
        printf("[LOG] Logging disabled by settings\n");
        return false;
    }

    // Open (or create) the log file in append mode
    FRESULT fr = f_open(&s_log_file, "0:/cd32_cd.log",
                         FA_WRITE | FA_OPEN_APPEND);
    if (fr != FR_OK) {
        printf("[LOG] ERROR: Cannot open cd32_cd.log (FatFS error %d)\n", fr);
        s_cfg.logging_enabled = false;
        return false;
    }
    s_file_open = true;

    // Record current file size for rotation tracking
    s_log_size_bytes = (uint32_t)f_size(&s_log_file);

    // Write a session start marker
    char marker[256];
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    int len = snprintf(marker, sizeof(marker),
        "\n"
        "================================================================================\n"
        "[%8lu] SESSION START — CD32 ODE Firmware Boot\n"
        "================================================================================\n",
        (unsigned long)ms);
    UINT bw;
    f_write(&s_log_file, marker, (UINT)len, &bw);
    s_log_size_bytes += bw;
    f_sync(&s_log_file);   // Flush immediately so the header is safe

    printf("[LOG] Logging active → 0:/cd32_cd.log (%lu bytes existing)\n",
           s_log_size_bytes);
    return true;
}

// ---------------------------------------------------------------------------
// logger_write — format and enqueue a log line
// ---------------------------------------------------------------------------
// Called from Core 0 ONLY.  Must not be called from Core 1 or any IRQ.
void logger_write(log_level_t level, const char *tag,
                   const char *fmt, ...) {
    if (!s_cfg.logging_enabled || !s_file_open) return;

    // Format:  [timestamp_ms] LEVEL TAG  message\n
    static const char *level_str[] = { "DEBG", "INFO", "WARN", "ERR " };
    const char *lstr = (level <= LOG_ERROR) ? level_str[level] : "????";

    char line[LOG_LINE_MAX];
    uint32_t ms = to_ms_since_boot(get_absolute_time());

    // Prefix: timestamp + level + tag
    int prefix_len = snprintf(line, sizeof(line),
                               "[%8lu] %s %s  ",
                               (unsigned long)ms, lstr, tag);
    if (prefix_len < 0) return;

    // Message body
    va_list ap;
    va_start(ap, fmt);
    int body_len = vsnprintf(line + prefix_len,
                              sizeof(line) - (size_t)prefix_len - 2,
                              fmt, ap);
    va_end(ap);

    if (body_len < 0) return;

    // Ensure newline terminator
    int total = prefix_len + body_len;
    if (total >= (int)sizeof(line) - 1) total = (int)sizeof(line) - 2;
    line[total]     = '\n';
    line[total + 1] = '\0';

    // Append to ring buffer
    ring_append(line, (uint32_t)(total + 1));

    // Trigger an immediate flush if the ring is getting full
    if (s_ring_used >= LOG_FLUSH_WATERMARK) {
        logger_flush();
    }
}

// ---------------------------------------------------------------------------
// logger_flush — drain ring buffer to SD card
// ---------------------------------------------------------------------------
// Must be called from Core 0 main loop only.
uint32_t logger_flush(void) {
    if (!s_file_open || s_ring_used == 0) return 0;

    // Check log rotation limit
    if (s_cfg.log_max_kb > 0 &&
        s_log_size_bytes >= s_cfg.log_max_kb * 1024) {
        // Truncate: close, re-open with FA_CREATE_ALWAYS to reset to empty
        f_close(&s_log_file);
        FRESULT fr = f_open(&s_log_file, "0:/cd32_cd.log",
                             FA_WRITE | FA_CREATE_ALWAYS);
        if (fr == FR_OK) {
            s_log_size_bytes = 0;
            // Write rotation marker
            char rot[128];
            int rlen = snprintf(rot, sizeof(rot),
                "[%8lu] INFO LOG   --- Log rotated (exceeded %luKB limit) ---\n",
                (unsigned long)to_ms_since_boot(get_absolute_time()),
                (unsigned long)s_cfg.log_max_kb);
            UINT bw;
            f_write(&s_log_file, rot, (UINT)rlen, &bw);
            s_log_size_bytes += bw;
        } else {
            // Rotation failed — stop logging to avoid filling SD card
            printf("[LOG] Log rotation failed (error %d) — logging stopped\n", fr);
            s_cfg.logging_enabled = false;
            return 0;
        }
    }

    // Write all pending bytes to the SD card in one or two contiguous chunks
    // (ring buffer may wrap around, requiring two writes)
    uint32_t total_written = 0;
    UINT bw;

    if (s_ring_tail < s_ring_head) {
        // Contiguous data: tail → head
        uint32_t chunk = s_ring_head - s_ring_tail;
        f_write(&s_log_file, s_ring + s_ring_tail, (UINT)chunk, &bw);
        total_written += bw;
        s_ring_tail = s_ring_head;
    } else if (s_ring_tail > s_ring_head) {
        // Wrapped: two chunks — tail → end, then start → head
        uint32_t chunk1 = LOG_RING_SIZE - s_ring_tail;
        f_write(&s_log_file, s_ring + s_ring_tail, (UINT)chunk1, &bw);
        total_written += bw;

        if (s_ring_head > 0) {
            f_write(&s_log_file, s_ring, (UINT)s_ring_head, &bw);
            total_written += bw;
        }
        s_ring_tail = s_ring_head;
    }

    s_ring_used = 0;   // Ring is now empty (tail == head)
    s_log_size_bytes += total_written;

    // Flush FatFS buffers to physical SD card
    f_sync(&s_log_file);

    // Log any dropped lines since last flush
    if (s_dropped_lines > 0) {
        char drop_msg[80];
        snprintf(drop_msg, sizeof(drop_msg),
                 "[%8lu] WARN LOG   %lu line(s) dropped (ring buffer full)\n",
                 (unsigned long)to_ms_since_boot(get_absolute_time()),
                 (unsigned long)s_dropped_lines);
        UINT dbw;
        f_write(&s_log_file, drop_msg, strlen(drop_msg), &dbw);
        f_sync(&s_log_file);
        s_dropped_lines = 0;
    }

    s_last_flush_time = get_absolute_time();
    return total_written;
}

// ---------------------------------------------------------------------------
// logger_flush_if_due — call from main loop, flushes on timer
// ---------------------------------------------------------------------------
// Returns bytes written (may be 0 if not yet due).
uint32_t logger_flush_if_due(void) {
    if (!s_file_open || !s_cfg.logging_enabled) return 0;

    int64_t elapsed_ms = absolute_time_diff_us(s_last_flush_time,
                                                get_absolute_time()) / 1000;
    if (elapsed_ms >= LOG_FLUSH_INTERVAL_MS || s_ring_used >= LOG_FLUSH_WATERMARK) {
        return logger_flush();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// logger_close
// ---------------------------------------------------------------------------
void logger_close(void) {
    if (!s_file_open) return;

    // Flush any remaining buffered data
    logger_flush();

    // Write a clean session end marker
    char marker[128];
    int len = snprintf(marker, sizeof(marker),
        "[%8lu] INFO LOG   SESSION END — clean shutdown\n",
        (unsigned long)to_ms_since_boot(get_absolute_time()));
    UINT bw;
    f_write(&s_log_file, marker, (UINT)len, &bw);
    f_sync(&s_log_file);
    f_close(&s_log_file);
    s_file_open = false;
    printf("[LOG] Log file closed cleanly\n");
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
bool logger_is_enabled(void) {
    return s_cfg.logging_enabled && s_file_open;
}

const logger_config_t *logger_get_config(void) {
    return &s_cfg;
}

void logger_set_enabled(bool enabled) {
    s_cfg.logging_enabled = enabled;
    if (enabled && !s_file_open) {
        // Re-open log file if it was closed
        FRESULT fr = f_open(&s_log_file, "0:/cd32_cd.log",
                             FA_WRITE | FA_OPEN_APPEND);
        if (fr == FR_OK) {
            s_file_open = true;
            logger_write(LOG_INFO, "LOG ", "Logging re-enabled at runtime");
        }
    } else if (!enabled) {
        logger_write(LOG_INFO, "LOG ", "Logging disabled at runtime");
        logger_flush();
    }
}

// ---------------------------------------------------------------------------
// Command name lookup
// ---------------------------------------------------------------------------
static const char *cmd_name(uint8_t cmd) {
    switch (cmd) {
        case 0x00: return "SYNC";
        case 0x01: return "GETSTAT";
        case 0x02: return "SETLOC";
        case 0x03: return "PLAY";
        case 0x04: return "FORWARD";
        case 0x05: return "BACKWARD";
        case 0x06: return "READN";
        case 0x07: return "MOTORON";
        case 0x08: return "STOP";
        case 0x09: return "PAUSE";
        case 0x0A: return "RESET";
        case 0x0B: return "MUTE";
        case 0x0C: return "UNMUTE";
        case 0x0D: return "SETFILTER";
        case 0x0E: return "SETMODE";
        case 0x0F: return "GETPARAM";
        case 0x10: return "GETLOCL";
        case 0x11: return "GETLOCP";
        case 0x13: return "GETTN";
        case 0x14: return "GETTD";
        case 0x15: return "SEEKL";
        case 0x16: return "SEEKP";
        case 0x19: return "TEST";
        case 0x1A: return "ID";
        case 0x1B: return "READS";
        case 0x1E: return "READTOC";
        default:   return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Internal log helpers (called by macros in logger.h)
// ---------------------------------------------------------------------------

void _log_cmd(uint8_t cmd, const uint8_t *params, uint8_t n_params) {
    char param_str[48] = "";
    if (params && n_params > 0) {
        int pos = 0;
        for (int i = 0; i < n_params && pos < (int)sizeof(param_str) - 4; i++) {
            pos += snprintf(param_str + pos, sizeof(param_str) - pos,
                            "%02X", params[i]);
            if (i < n_params - 1) {
                param_str[pos++] = ':';
                param_str[pos]   = '\0';
            }
        }
    }

    if (n_params > 0) {
        logger_write(LOG_INFO, "CMD ",
                     "0x%02X %-10s params=[%s]",
                     cmd, cmd_name(cmd), param_str);
    } else {
        logger_write(LOG_INFO, "CMD ",
                     "0x%02X %-10s",
                     cmd, cmd_name(cmd));
    }
}

void _log_cmd_resp(uint8_t cmd, const uint8_t *resp, uint8_t n_resp) {
    if (!resp || n_resp == 0) return;

    char resp_str[48] = "";
    int pos = 0;
    for (int i = 0; i < n_resp && pos < (int)sizeof(resp_str) - 4; i++) {
        pos += snprintf(resp_str + pos, sizeof(resp_str) - pos,
                        "%02X", resp[i]);
        if (i < n_resp - 1) {
            resp_str[pos++] = ':';
            resp_str[pos]   = '\0';
        }
    }

    // Decode the status byte (first response byte) into human-readable flags
    uint8_t stat = resp[0];
    char stat_flags[32] = "";
    if (stat & 0x80) strcat(stat_flags, "BUSY ");
    if (stat & 0x40) strcat(stat_flags, "RSLR ");
    if (stat & 0x20) strcat(stat_flags, "DRQ ");
    if (stat & 0x04) strcat(stat_flags, "DISC ");
    if (stat & 0x01) strcat(stat_flags, "ERR");

    logger_write(LOG_INFO, "CMD ",
                 "0x%02X %-10s → [%s] (%s)",
                 cmd, cmd_name(cmd), resp_str, stat_flags);
}

void _log_sector(uint32_t lba, const char *mode_str,
                  uint32_t bytes, bool filtered) {
    if (filtered) {
        logger_write(LOG_DEBUG, "SECT",
                     "LBA=%-6lu %s filtered (XA mismatch)", (unsigned long)lba, mode_str);
    } else {
        logger_write(LOG_DEBUG, "SECT",
                     "LBA=%-6lu %s %luB → host", (unsigned long)lba, mode_str, (unsigned long)bytes);
    }
}

void _log_seek_start(uint32_t from_lba, uint32_t to_lba, uint32_t est_us) {
    logger_write(LOG_INFO, "SEEK",
                 "start from=%-6lu to=%-6lu est=%lums",
                 (unsigned long)from_lba,
                 (unsigned long)to_lba,
                 (unsigned long)(est_us / 1000));
}

void _log_state(int old_state, int new_state) {
    static const char *state_names[] = {
        "RESET", "IDLE", "SPINUP", "READY",
        "SEEKING", "READING", "PLAYING", "PAUSED", "ERROR"
    };
    const char *old_name = (old_state >= 0 && old_state <= 8)
                            ? state_names[old_state] : "???";
    const char *new_name = (new_state >= 0 && new_state <= 8)
                            ? state_names[new_state] : "???";
    logger_write(LOG_INFO, "DRV ",
                 "state %-8s → %s", old_name, new_name);
}

void _log_irq(uint8_t flags) {
    char flag_str[48] = "";
    if (flags & 0x01) strcat(flag_str, "DATA_END ");
    if (flags & 0x02) strcat(flag_str, "SUBCODE ");
    if (flags & 0x04) strcat(flag_str, "DISC_DET ");
    if (flags & 0x08) strcat(flag_str, "CMD_DONE ");
    if (flags & 0x10) strcat(flag_str, "SEEK_DONE");
    logger_write(LOG_DEBUG, "IRQ ", "flags=0x%02X [%s]", flags, flag_str);
}
