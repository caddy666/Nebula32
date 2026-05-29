#pragma once
// No-op logger stubs for host-native test builds.
// All LOG_* macros expand to nothing; all logger functions are no-ops.

#include <stdint.h>
#include <stdbool.h>
#include "cd_types.h"

typedef enum { LOG_DEBUG=0, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

typedef struct {
    bool     logging_enabled;
    bool     log_commands;
    bool     log_sectors;
    bool     log_seeks;
    bool     log_state;
    bool     log_errors;
    bool     log_irq;
    uint32_t log_max_kb;
    char     sdcard_base[256];
    char     wifi_ssid[64];
    char     wifi_password[64];
    char     wifi_hostname[32];
    char     fw_token[33];
} logger_config_t;

static logger_config_t _stub_logger_cfg = {
    false, true, false, true, true, true, false, 4096,
    "0:/", "", "", "nebula32", ""
};

static inline bool                   logger_init(void)          { return false; }
static inline uint32_t               logger_flush(void)         { return 0; }
static inline uint32_t               logger_flush_if_due(void)  { return 0; }
static inline void                   logger_close(void)         {}
static inline bool                   logger_is_enabled(void)    { return false; }
static inline void                   logger_set_enabled(bool e) { (void)e; }
static inline const logger_config_t *logger_get_config(void)    { return &_stub_logger_cfg; }
static inline void logger_write(log_level_t l, const char *t,
                                const char *f, ...) { (void)l;(void)t;(void)f; }

#define LOG_CMD(o,p,n)              ((void)0)
#define LOG_CMD_RESP(o,r,n)         ((void)0)
#define LOG_SECTOR(l,m,b,f)         ((void)0)
#define LOG_SEEK_START(f,t,e)       ((void)0)
#define LOG_SEEK_DONE(l)            ((void)0)
#define LOG_STATE(o,n)              ((void)0)
#define LOG_ERROR_MSG(fmt, ...)     ((void)0)
#define LOG_WARN_MSG(fmt, ...)      ((void)0)
#define LOG_INFO_MSG(tag,fmt, ...)  ((void)0)
#define LOG_DEBUG_MSG(tag,fmt, ...) ((void)0)
