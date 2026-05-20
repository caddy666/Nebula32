// =============================================================================
// fw_update.c — SD-card UF2 firmware self-update
// =============================================================================

#include "fw_update.h"

#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

#include "display.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// UF2 block format (512 bytes per block)
// ---------------------------------------------------------------------------

#define UF2_MAGIC_START0        0x0A324655UL
#define UF2_MAGIC_START1        0x9E5D5157UL
#define UF2_MAGIC_END           0x0AB16F30UL
#define UF2_FLAG_FAMILY_ID      0x00002000UL
#define UF2_FLAG_NOFLASH        0x00000001UL

// RP2350 family IDs — accept all three variants
#define UF2_FAMILY_RP2350_ARM_S   0xe48bff59UL
#define UF2_FAMILY_RP2350_RISCV   0xe48bff60UL
#define UF2_FAMILY_RP2350_ARM_NS  0xe48bff61UL

typedef struct __attribute__((packed)) {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;   /* XIP-mapped address in Bank 0 space */
    uint32_t payload_size;  /* bytes of data[] to write (normally 256) */
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;     /* or family_id when FLAG_FAMILY_ID set */
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;

// ---------------------------------------------------------------------------
// Flash layout
// ---------------------------------------------------------------------------

#define FW_BANK1_OFFSET  0x100000u   /* Bank 1 starts 1 MB into flash */

// Config page offset (last 4 KB of 2 MB flash)
// PICO_FLASH_SIZE_BYTES is defined by the SDK for the target board.
#define FW_CONFIG_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

// Number of 4 KB sectors in the full flash
#define FW_TOTAL_SECTORS  (PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE)

// Sector index for Bank 1 start
#define FW_BANK1_FIRST_SECTOR  (FW_BANK1_OFFSET / FLASH_SECTOR_SIZE)

// ---------------------------------------------------------------------------
// Static sector buffer (4 KB) — avoids stack overflow during sector copies
// ---------------------------------------------------------------------------
static uint8_t __attribute__((aligned(4))) s_sector_buf[FLASH_SECTOR_SIZE];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool is_rp2350_family(uint32_t fid) {
    return fid == UF2_FAMILY_RP2350_ARM_S  ||
           fid == UF2_FAMILY_RP2350_RISCV  ||
           fid == UF2_FAMILY_RP2350_ARM_NS;
}

static void erase_map_set(uint8_t map[64], uint32_t sector_index) {
    if (sector_index < (uint32_t)FW_TOTAL_SECTORS)
        map[sector_index / 8] |= (uint8_t)(1u << (sector_index % 8));
}

static bool erase_map_get(const uint8_t map[64], uint32_t sector_index) {
    if (sector_index >= (uint32_t)FW_TOTAL_SECTORS) return false;
    return (map[sector_index / 8] >> (sector_index % 8)) & 1u;
}

// ---------------------------------------------------------------------------
// fw_result_str
// ---------------------------------------------------------------------------

const char *fw_result_str(fw_result_t r) {
    switch (r) {
        case FW_OK:              return "ok";
        case FW_ERR_NOT_FOUND:   return "file not found";
        case FW_ERR_IO:          return "SD I/O error";
        case FW_ERR_BAD_MAGIC:   return "bad UF2 magic";
        case FW_ERR_FAMILY:      return "wrong family ID (not RP2350)";
        case FW_ERR_TOO_LARGE:   return "firmware exceeds 1 MB bank limit";
        case FW_ERR_BLOCK_COUNT: return "inconsistent num_blocks";
        case FW_ERR_BAD_SEQUENCE:return "block_no not sequential";
        case FW_ERR_NO_BLOCKS:   return "no data blocks in UF2";
        case FW_ERR_NO_ORIGIN:   return "no block at flash offset 0 (wrong binary?)";
        case FW_ERR_VERIFY:      return "Bank 1 verify failed";
        case FW_ERR_BAD_PAYLOAD: return "payload_size is 0, >476, or not page-aligned";
        default:                 return "unknown error";
    }
}

// ---------------------------------------------------------------------------
// fw_validate — Pass 1: SD read only, no flash operations
// ---------------------------------------------------------------------------

fw_result_t fw_validate(const char *path,
                        uint32_t   *out_num_blocks,
                        uint8_t     erase_map[64]) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK)
        return FW_ERR_NOT_FOUND;

    uf2_block_t blk;
    UINT br;
    uint32_t expected_num_blocks = 0;
    uint32_t expected_next_block_no = 0;  /* enforce sequential block_no */
    bool     has_origin_block = false;    /* seen a block at offset 0? */
    uint32_t data_block_count = 0;
    fw_result_t result = FW_OK;

    while (f_read(&fil, &blk, sizeof(blk), &br) == FR_OK && br == sizeof(blk)) {
        // Magic check
        if (blk.magic_start0 != UF2_MAGIC_START0 ||
            blk.magic_start1 != UF2_MAGIC_START1 ||
            blk.magic_end    != UF2_MAGIC_END) {
            result = FW_ERR_BAD_MAGIC;
            break;
        }

        // B4 FIX: sequence check applies to ALL blocks (including NOFLASH) so that
        // a NOFLASH block at position N still consumes its block_no slot.
        if (blk.block_no != expected_next_block_no) {
            result = FW_ERR_BAD_SEQUENCE;
            break;
        }
        expected_next_block_no++;

        // Informational block — skip all further checks for this block
        if (blk.flags & UF2_FLAG_NOFLASH)
            continue;

        // Family ID check
        if ((blk.flags & UF2_FLAG_FAMILY_ID) && !is_rp2350_family(blk.file_size)) {
            result = FW_ERR_FAMILY;
            break;
        }

        // B2/B3/S3 FIX: reject oversized or unaligned payload_size before any
        // arithmetic that uses it — prevents wrap-around in the config-page check
        // and buffer overread in flash_range_program / memcmp.
        if (blk.payload_size == 0 || blk.payload_size > 476 ||
            blk.payload_size % FLASH_PAGE_SIZE != 0) {
            result = FW_ERR_BAD_PAYLOAD;
            break;
        }

        // B1 FIX: reject SRAM/peripheral addresses (below XIP_BASE); without this
        // guard the subtraction wraps and bank0_off looks like a valid offset.
        if (blk.target_addr < XIP_BASE) {
            result = FW_ERR_TOO_LARGE;
            break;
        }

        // Address must target Bank 0 XIP space and fit within one bank
        uint32_t bank0_off = blk.target_addr - XIP_BASE;
        if (bank0_off >= FW_BANK1_OFFSET) {
            result = FW_ERR_TOO_LARGE;
            break;
        }

        // Compute Bank 1 staging offset for this block
        uint32_t bank1_off = bank0_off + FW_BANK1_OFFSET;

        // Skip blocks that would hit the shared config page
        if (bank1_off + blk.payload_size > FW_CONFIG_OFFSET)
            continue;

        data_block_count++;
        if (bank0_off == 0) has_origin_block = true;

        // Mark the Bank 1 sector for erasure
        uint32_t sector = bank1_off / FLASH_SECTOR_SIZE;
        erase_map_set(erase_map, sector);

        // Track num_blocks consistency
        if (expected_num_blocks == 0)
            expected_num_blocks = blk.num_blocks;
        else if (blk.num_blocks != expected_num_blocks) {
            result = FW_ERR_BLOCK_COUNT;
            break;
        }
    }

    f_close(&fil);

    if (result == FW_OK) {
        if (data_block_count == 0)
            return FW_ERR_NO_BLOCKS;
        if (!has_origin_block)
            return FW_ERR_NO_ORIGIN;
        if (out_num_blocks) *out_num_blocks = expected_num_blocks;
    }
    return result;
}

// ---------------------------------------------------------------------------
// fw_flash_and_reboot — dual-stage write with verification
// ---------------------------------------------------------------------------
// Stage 1: Write UF2 blocks to Bank 1 (staging)
// Stage 2: Verify Bank 1 via XIP read
// Stage 3: Copy Bank 1 → Bank 0 (commit)
// Stage 4: watchdog_reboot(0, 0, 0)
//
// Core 1 is locked out for all flash stages (Stages 1–3 combined).
// IRQs are disabled only for individual flash_range_erase/program calls,
// allowing SDIO DMA to complete between operations.
// ---------------------------------------------------------------------------

fw_result_t fw_flash_and_reboot(const char *path) {
    // --- Pre-flight validation ---
    uint32_t num_blocks = 0;
    uint8_t erase_map[64] = {0};

    fw_result_t r = fw_validate(path, &num_blocks, erase_map);
    if (r != FW_OK) return r;

    printf("[FW] Validated %lu blocks; starting dual-stage flash\n",
           (unsigned long)num_blocks);
    LOG_INFO_MSG("FW  ", "validate ok: %lu blocks", (unsigned long)num_blocks);
    logger_flush();   // drain to SD before Core 1 lockout prevents SDIO access

    // O4: Render firmware-update overlay before locking out Core 1
    display_fw_progress(0);

    // --- Lock out Core 1 for the entire flash operation ---
    // Core 1 spins in SRAM; SDIO DMA on Core 0 continues between flash ops.
    multicore_lockout_start_blocking();

    // ── Stage 1a: Erase Bank 1 sectors ──
    for (uint32_t s = FW_BANK1_FIRST_SECTOR; s < (uint32_t)FW_TOTAL_SECTORS; s++) {
        if (!erase_map_get(erase_map, s)) continue;
        uint32_t off = s * FLASH_SECTOR_SIZE;
        if (off + FLASH_SECTOR_SIZE > FW_CONFIG_OFFSET) continue;
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(off, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
    }
    display_fw_progress(5);

    // ── Stage 1b: Write UF2 blocks to Bank 1 ──
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        multicore_lockout_end_blocking();
        return FW_ERR_IO;
    }

    uf2_block_t blk;
    UINT br;
    uint32_t blocks_written = 0;
    while (f_read(&fil, &blk, sizeof(blk), &br) == FR_OK && br == sizeof(blk)) {
        if (blk.flags & UF2_FLAG_NOFLASH) continue;
        // Defense-in-depth guards (already validated, but protect flash_range_program)
        if (blk.payload_size == 0 || blk.payload_size > 476 ||
            blk.target_addr < XIP_BASE) continue;
        uint32_t bank0_off = blk.target_addr - XIP_BASE;
        if (bank0_off >= FW_BANK1_OFFSET) continue;
        uint32_t bank1_off = bank0_off + FW_BANK1_OFFSET;
        if (bank1_off + blk.payload_size > FW_CONFIG_OFFSET) continue;
        // IRQs re-enabled between blocks so SDIO DMA can complete
        uint32_t ints = save_and_disable_interrupts();
        flash_range_program(bank1_off, blk.data, blk.payload_size);
        restore_interrupts(ints);
        blocks_written++;
        if (num_blocks > 0)
            display_fw_progress((uint8_t)(5u + 65u * blocks_written / num_blocks));
    }
    f_close(&fil);

    printf("[FW] Bank 1 write complete; verifying via XIP\n");
    display_fw_progress(70);

    // ── Stage 2: Verify Bank 1 via XIP readback ──
    // If this fails, Bank 0 is untouched — safe to return error.
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        multicore_lockout_end_blocking();
        return FW_ERR_IO;
    }

    r = FW_OK;
    while (f_read(&fil, &blk, sizeof(blk), &br) == FR_OK && br == sizeof(blk)) {
        if (blk.flags & UF2_FLAG_NOFLASH) continue;
        // S3 FIX: same payload_size and address guards as B1/B2 — prevents
        // memcmp reading past blk.data[476] with an oversized payload_size.
        if (blk.payload_size == 0 || blk.payload_size > 476 ||
            blk.target_addr < XIP_BASE) continue;
        uint32_t bank0_off = blk.target_addr - XIP_BASE;
        if (bank0_off >= FW_BANK1_OFFSET) continue;
        uint32_t bank1_off = bank0_off + FW_BANK1_OFFSET;
        if (bank1_off + blk.payload_size > FW_CONFIG_OFFSET) continue;
        const uint8_t *xip = (const uint8_t *)(XIP_BASE + bank1_off);
        if (memcmp(xip, blk.data, blk.payload_size) != 0) {
            r = FW_ERR_VERIFY;
            break;
        }
    }
    f_close(&fil);

    if (r != FW_OK) {
        multicore_lockout_end_blocking();
        printf("[FW] Verify FAILED — Bank 0 untouched\n");
        LOG_INFO_MSG("FW  ", "verify FAIL");
        return r;
    }

    printf("[FW] Verify OK; copying Bank 1 → Bank 0\n");
    display_fw_progress(85);

    // ── Stage 3: Copy Bank 1 → Bank 0 (commit) ──
    // Uses the erase_map to find which Bank 1 sectors contain new firmware.
    // For each such sector: copy 4 KB via XIP to SRAM, erase Bank 0 sector,
    // then program Bank 0 from SRAM.
    //
    // B7 FIX: IRQs are re-enabled between the sector erase and each page-write
    // (and between page-writes).  The original code held IRQs disabled across
    // erase (~50 ms) + 16 page-writes (~7.2 ms each) = ~115 ms per sector,
    // suppressing SDIO, USB, and the watchdog for a 150 KB firmware (~40 s total).
    uint32_t sectors_to_copy = 0;
    for (uint32_t s = FW_BANK1_FIRST_SECTOR; s < (uint32_t)FW_TOTAL_SECTORS; s++) {
        if (erase_map_get(erase_map, s)) sectors_to_copy++;
    }
    uint32_t sectors_copied = 0;

    for (uint32_t s = FW_BANK1_FIRST_SECTOR; s < (uint32_t)FW_TOTAL_SECTORS; s++) {
        if (!erase_map_get(erase_map, s)) continue;

        uint32_t bank1_sect_off = s * FLASH_SECTOR_SIZE;
        uint32_t bank0_sect_off = bank1_sect_off - FW_BANK1_OFFSET;
        if (bank0_sect_off + FLASH_SECTOR_SIZE > FW_CONFIG_OFFSET) continue;

        // Copy Bank 1 sector to SRAM via XIP (IRQs enabled — cache is coherent)
        memcpy(s_sector_buf, (const uint8_t *)(XIP_BASE + bank1_sect_off),
               FLASH_SECTOR_SIZE);

        // Erase Bank 0 sector (own IRQ window)
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(bank0_sect_off, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);

        // Program Bank 0 sector page-by-page (each page gets its own IRQ window)
        for (uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE) {
            ints = save_and_disable_interrupts();
            flash_range_program(bank0_sect_off + p,
                                s_sector_buf + p,
                                FLASH_PAGE_SIZE);
            restore_interrupts(ints);
        }

        sectors_copied++;
        if (sectors_to_copy > 0)
            display_fw_progress((uint8_t)(85u + 14u * sectors_copied / sectors_to_copy));
    }

    multicore_lockout_end_blocking();

    printf("[FW] Flash complete; rebooting\n");
    display_fw_progress(100);
    LOG_INFO_MSG("FW  ", "commit ok; rebooting");
    logger_flush();

    // B5 FIX: add "0:/" volume prefix to destination so FF_MULTI_PARTITION=1
    // builds don't reject the cross-prefix rename.
    // B6 FIX: check return value — if rename fails, NEBULA32.UF2 stays on SD
    // and the sentinel fires again on the next power-on (infinite re-flash loop).
    FRESULT rename_r = f_rename(path, "0:/NEBULA32.OLD");
    if (rename_r != FR_OK)
        printf("[FW] WARNING: rename sentinel failed (%d) — remove NEBULA32.UF2 manually\n",
               (int)rename_r);
    LOG_INFO_MSG("FW  ", "rename sentinel: %d", (int)rename_r);
    logger_flush();

    // O6: Signal to main.c that we just did a successful flash.  The scratch
    // register survives a watchdog reboot but is cleared on power-cycle.
    watchdog_hw->scratch[0] = FW_UPDATE_MAGIC;

    // Stage 4: watchdog reboot — bootrom starts new firmware from offset 0
    watchdog_reboot(0, 0, 0);
    while (1) { tight_loop_contents(); }
}
