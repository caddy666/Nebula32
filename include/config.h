#pragma once
// =============================================================================
// config.h — Persistent configuration stored in RP2350 flash
// =============================================================================
//
// The CD32 ODE saves a small configuration block in the last 4 KB page of
// the RP2350's flash.  This survives power cycles and firmware updates
// (as long as the firmware does not grow into the config page).
//
// STORED SETTINGS:
//   last_image_index  — Index of the disc image that was loaded last.
//                       On next boot the same image is auto-loaded.
//   speed_mode        — Drive speed: 1× or 2×.
//   audio_enabled     — Whether the I2S audio output is active.
//   verify_edc        — Whether to verify EDC on every read sector.
//   swap_bytes        — Whether to byte-swap audio samples (for DACs
//                       that expect big-endian rather than standard I2S).
//
// FLASH LAYOUT (RP2350 has 2 MB flash = 0x10000000–0x101FFFFF):
//   0x101FF000–0x101FFFFF : 4 KB config page (last page in flash)
//   The firmware image must not exceed 0x101FF000 − 0x10000000 = 2044 KB.
//   For reference: our firmware compiles to roughly 100–150 KB.
//
// WRITE WEAR:
//   Flash has ~100,000 erase cycles.  We only write when a setting changes.
//   Assuming one config save per power cycle, this is >270 years.
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "cd_types.h"  // CD32_SASSERT

// ---------------------------------------------------------------------------
// Config page location in flash
// ---------------------------------------------------------------------------
// PICO_FLASH_SIZE_BYTES is defined by the SDK based on the board target.
// We place the config in the last 4 KB (one flash sector).
#define CONFIG_FLASH_OFFSET  (PICO_FLASH_SIZE_BYTES - 4096)
#define CONFIG_FLASH_ADDR    (XIP_BASE + CONFIG_FLASH_OFFSET)

// Magic number to detect a valid (programmed) config page
#define CONFIG_MAGIC         0xCD320DE5u   // "CD32 ODE" signature

// Config format version — increment when the struct layout changes
#define CONFIG_VERSION       2u

// ---------------------------------------------------------------------------
// Config structure
// ---------------------------------------------------------------------------
// Must be <= 4096 bytes.  Pad to a power of two for alignment.
// The CRC32 at the end covers all other fields.
typedef struct __attribute__((packed)) {
    uint32_t magic;             // CONFIG_MAGIC when valid
    uint16_t version;           // CONFIG_VERSION
    uint16_t flags;             // Bitfield — see FLAG_* defines below

    uint16_t last_image_index;  // absolute 0-based index of last loaded image
    uint8_t  speed_mode;        // 1 = 1× speed, 2 = 2× speed
    uint8_t  reserved[57];      // Pad to 64 bytes before CRC

    uint32_t crc32;             // CRC32 of bytes 0..(sizeof-4)
} ode_config_t;

// Ensure the struct fits in one flash page
CD32_SASSERT(sizeof(ode_config_t) <= 4096,
             "Config struct too large for one 4 KB flash page");

// Flags bitfield
#define CFG_FLAG_AUDIO_ENABLED    (1u << 0)  // I2S audio output active
#define CFG_FLAG_VERIFY_EDC       (1u << 1)  // Verify EDC on raw reads
#define CFG_FLAG_SWAP_AUDIO_BYTES (1u << 2)  // Byte-swap I2S samples

// ---------------------------------------------------------------------------
// Default configuration (used when flash contains no valid config)
// ---------------------------------------------------------------------------
#define CONFIG_DEFAULT { \
    .magic            = CONFIG_MAGIC,        \
    .version          = CONFIG_VERSION,      \
    .flags            = CFG_FLAG_AUDIO_ENABLED, \
    .last_image_index = 0,                   \
    .speed_mode       = 2,                   \
    .reserved         = {0},                 \
    .crc32            = 0                    \
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise the config subsystem.
// Reads the config from flash; if invalid, loads defaults and returns false.
// Call once from main() before reading any config fields.
bool config_init(ode_config_t *cfg);

// Write the current config to flash.
// Erases and re-programs the config page.
// NOTE: This function disables interrupts for ~50 ms during the flash write.
// Do not call during active disc I/O (i.e., call only from the console handler
// after the user has changed a setting, when the drive is idle).
bool config_save(const ode_config_t *cfg);

// Reset config to factory defaults without saving to flash.
void config_defaults(ode_config_t *cfg);

// Compute CRC32 of the config fields (excluding the crc32 field itself).
// Used internally by config_save() and config_init().
uint32_t config_crc32(const ode_config_t *cfg);

