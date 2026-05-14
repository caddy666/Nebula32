#pragma once
// No-op logger stubs for host-native test builds.
// All LOG_* macros expand to nothing; all logger functions are no-ops.

#include <stdint.h>
#include <stdbool.h>
#include "cd_types.h"

typedef enum { LOG_DEBUG=0, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

static inline bool     logger_init(void)          { return false; }
static inline uint32_t logger_flush(void)         { return 0; }
static inline uint32_t logger_flush_if_due(void)  { return 0; }
static inline void     logger_close(void)         {}
static inline bool     logger_is_enabled(void)    { return false; }
static inline void     logger_set_enabled(bool e) { (void)e; }

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
