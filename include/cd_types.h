#pragma once
// =============================================================================
// cd_types.h — Fundamental CD timing and addressing types
// =============================================================================
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// MSF (Minute:Second:Frame) address — BCD encoded, as used on Red Book CDs
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t minute;
    uint8_t second;
    uint8_t frame;
} msf_t;

// 75 sectors per second (Red Book)
#define SECTORS_PER_SECOND  75
#define SECTOR_RAW_BYTES    2352
#define SECTOR_DATA_BYTES   2048

// ---------------------------------------------------------------------------
// LBA ↔ MSF conversion (150-sector lead-in offset per Red Book)
// ---------------------------------------------------------------------------
static inline uint32_t msf_to_lba(msf_t m) {
    uint32_t min = ((m.minute >> 4) & 0x0F) * 10 + (m.minute & 0x0F);
    uint32_t sec = ((m.second >> 4) & 0x0F) * 10 + (m.second & 0x0F);
    uint32_t frm = ((m.frame  >> 4) & 0x0F) * 10 + (m.frame  & 0x0F);
    uint32_t lba = (min * 60 + sec) * SECTORS_PER_SECOND + frm;
    return (lba >= 150) ? lba - 150 : 0;
}

static inline msf_t lba_to_msf(uint32_t lba) {
    uint32_t total = lba + 150;
    uint32_t frm   = total % SECTORS_PER_SECOND;
    uint32_t sec   = (total / SECTORS_PER_SECOND) % 60;
    uint32_t min   = total / (SECTORS_PER_SECOND * 60);
    msf_t m;
    m.minute = (uint8_t)(((min / 10) << 4) | (min % 10));
    m.second = (uint8_t)(((sec / 10) << 4) | (sec % 10));
    m.frame  = (uint8_t)(((frm / 10) << 4) | (frm % 10));
    return m;
}

// ---------------------------------------------------------------------------
// Drive state (used by COMMO status responses and logger)
// ---------------------------------------------------------------------------
typedef enum {
    DRIVE_IDLE = 0,
    DRIVE_SPINUP,
    DRIVE_READY,
    DRIVE_SEEKING,
    DRIVE_READING,
    DRIVE_PLAYING,
    DRIVE_PAUSED,
    DRIVE_ERROR,
} drive_state_t;

// ---------------------------------------------------------------------------
// Drive status byte bits (returned in COMMO response packets)
// ---------------------------------------------------------------------------
#define DRIVE_STATUS_BUSY   0x80   // Drive is busy (spinning up / seeking)
#define DRIVE_STATUS_DRQ    0x20   // Data ready (sector waiting)
#define DRIVE_STATUS_DISC   0x04   // Disc present
#define DRIVE_STATUS_ERROR  0x01   // Error condition

// ---------------------------------------------------------------------------
// Sector output mode (controls how disc_read_sector formats the output)
// ---------------------------------------------------------------------------
typedef enum {
    SECTOR_MODE_DATA = 0,   // 2048-byte cooked data (Yellow Book Mode 1)
    SECTOR_MODE_RAW  = 1,   // 2352-byte raw sector (audio or data)
} sector_mode_t;
