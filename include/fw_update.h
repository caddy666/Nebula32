#pragma once
// =============================================================================
// fw_update.h — SD-card UF2 firmware self-update
// =============================================================================
//
// Two-stage dual-bank write with fault tolerance:
//   Stage 1: Write UF2 payload to Bank 1 (flash offset 0x100000)
//   Stage 2: Verify Bank 1 via XIP read — only if this passes do we touch Bank 0
//   Stage 3: Copy Bank 1 → Bank 0 sector-by-sector
//   Stage 4: watchdog_reboot(0,0,0) — boots new firmware at offset 0
//
// If power fails in Stage 1 or 2, Bank 0 (running firmware) is untouched.
// If power fails in Stage 3, USB drag-and-drop recovery is required.
//
// Sentinel: place NEBULA32.UF2 in the SD card root.  On next boot, the device
// flashes automatically and renames the file to NEBULA32.OLD before rebooting.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FW_OK = 0,
    FW_ERR_NOT_FOUND,    /* UF2 file not present on SD */
    FW_ERR_IO,           /* FatFS read/open error */
    FW_ERR_BAD_MAGIC,    /* block magic number wrong */
    FW_ERR_FAMILY,       /* not an RP2350 family UF2 */
    FW_ERR_TOO_LARGE,    /* target_addr offset >= 1 MB (won't fit in one bank) */
    FW_ERR_BLOCK_COUNT,  /* num_blocks inconsistent across blocks */
    FW_ERR_BAD_SEQUENCE, /* block_no not sequential (0, 1, 2, ...) */
    FW_ERR_NO_BLOCKS,    /* UF2 file has no data blocks */
    FW_ERR_NO_ORIGIN,    /* no block targets flash offset 0 (missing reset vector) */
    FW_ERR_VERIFY,       /* Bank 1 XIP readback mismatch after program */
    FW_ERR_BAD_PAYLOAD,  /* payload_size is 0, >476, or not page-aligned */
} fw_result_t;

/* Magic written to watchdog scratch[0] before reboot so main.c can play the
   firmware-update success animation.  Cleared by main.c after reading. */
#define FW_UPDATE_MAGIC  0xF1A5B007UL

// Human-readable string for a fw_result_t code.
const char *fw_result_str(fw_result_t r);

// Validate a UF2 file without touching flash.
// Reads all blocks, checks magic/family/address constraints.
// Fills erase_map[64] with a bitmask of 4 KB Bank-1 sectors to erase
// (caller must pass a zeroed 64-byte buffer).
// Returns FW_OK if safe to flash.
fw_result_t fw_validate(const char *path,
                        uint32_t   *out_num_blocks,
                        uint8_t     erase_map[64]);

// Flash the UF2 to Bank 1, verify, copy to Bank 0, then reboot.
// Internally calls fw_validate.  Never returns on success.
// Returns an error code on failure; Core 1 continues running.
fw_result_t fw_flash_and_reboot(const char *path);
