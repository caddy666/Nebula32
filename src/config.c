// =============================================================================
// config.c — Persistent configuration in RP2350 flash
// =============================================================================
//
// The RP2350 uses XIP (Execute-In-Place) flash accessed via the QSPI bus.
// Reading flash is transparent (the XIP cache makes it look like SRAM).
// Writing flash requires:
//   1. Disabling the XIP cache and IRQs
//   2. Calling flash_range_erase() to erase the 4 KB sector
//   3. Calling flash_range_program() to write the new data
//   4. Re-enabling IRQs
//
// The Pico SDK's hardware/flash.h provides these low-level operations.
// flash_range_erase() and flash_range_program() MUST run from SRAM
// (not flash) because they disable the XIP cache.  The SDK handles this
// automatically via __no_inline_not_in_flash_func().
// =============================================================================

#include "config.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"  // multicore_lockout_start/end_blocking
#include "hardware/flash.h"
#include "hardware/sync.h"   // save_and_disable_interrupts / restore_interrupts

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3 polynomial 0xEDB88320, bit-reversed)
// ---------------------------------------------------------------------------
// Used to detect flash corruption or an uninitialized config page.

static uint32_t crc32_byte(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int b = 0; b < 8; b++) {
        if (crc & 1) {
            crc = (crc >> 1) ^ 0xEDB88320U;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

uint32_t config_crc32(const ode_config_t *cfg) {
    // CRC covers every byte except the last 4 (the crc32 field itself)
    const uint8_t *data = (const uint8_t *)cfg;
    size_t len = sizeof(ode_config_t) - sizeof(uint32_t);
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_byte(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFU;
}

// ---------------------------------------------------------------------------
// config_defaults — populate cfg with factory settings
// ---------------------------------------------------------------------------
void config_defaults(ode_config_t *cfg) {
    ode_config_t def = CONFIG_DEFAULT;
    memcpy(cfg, &def, sizeof(ode_config_t));
    cfg->crc32 = config_crc32(cfg);
}

// ---------------------------------------------------------------------------
// config_init — load from flash or fall back to defaults
// ---------------------------------------------------------------------------
bool config_init(ode_config_t *cfg) {
    // The config page lives at the end of flash, mapped into the XIP window
    const ode_config_t *flash_cfg = (const ode_config_t *)CONFIG_FLASH_ADDR;

    // Check magic number and version
    if (flash_cfg->magic != CONFIG_MAGIC || flash_cfg->version != CONFIG_VERSION) {
        printf("[CFG] No valid config in flash (magic=0x%08lX) — using defaults\n",
               flash_cfg->magic);
        config_defaults(cfg);
        return false;
    }

    // Verify CRC
    uint32_t computed = config_crc32(flash_cfg);
    if (computed != flash_cfg->crc32) {
        printf("[CFG] Config CRC mismatch (stored=0x%08lX computed=0x%08lX) — defaults\n",
               flash_cfg->crc32, computed);
        config_defaults(cfg);
        return false;
    }

    // Config is valid — copy to RAM struct
    memcpy(cfg, flash_cfg, sizeof(ode_config_t));
    printf("[CFG] Loaded: image=%d speed=%dx audio=%s\n",
           cfg->last_image_index,
           cfg->speed_mode,
           (cfg->flags & CFG_FLAG_AUDIO_ENABLED) ? "on" : "off");
    return true;
}

// ---------------------------------------------------------------------------
// config_save — write config to the last 4 KB flash sector
// ---------------------------------------------------------------------------
// WARNING: This function erases and reprograms flash.
// All interrupts are disabled for ~50 ms during the operation.
// The XIP cache is invalidated — code running from flash will stall.
// Call only when the drive is idle (e.g., user pressed a console key).
bool config_save(const ode_config_t *cfg_in) {
    // Work on a local copy (flash_range_program needs the source in SRAM)
    static ode_config_t write_buf __attribute__((aligned(FLASH_PAGE_SIZE)));
    memcpy(&write_buf, cfg_in, sizeof(ode_config_t));

    // Update CRC before writing
    write_buf.crc32 = config_crc32(&write_buf);

    printf("[CFG] Saving config to flash offset 0x%X...\n", CONFIG_FLASH_OFFSET);

    static uint8_t page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page_buf, 0xFF, FLASH_PAGE_SIZE);
    memcpy(page_buf, &write_buf, sizeof(ode_config_t));

    // Park Core 1 in SRAM before disabling XIP cache; flash ops must not race
    // with Core 1 fetching instructions from flash.
    multicore_lockout_start_blocking();
    uint32_t irq_state = save_and_disable_interrupts();

    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);

    restore_interrupts(irq_state);
    multicore_lockout_end_blocking();

    // Verify the write by reading back through XIP
    const ode_config_t *readback = (const ode_config_t *)CONFIG_FLASH_ADDR;
    uint32_t rb_crc = config_crc32(readback);
    if (readback->magic != CONFIG_MAGIC || rb_crc != readback->crc32) {
        printf("[CFG] ERROR: Flash write verification failed!\n");
        return false;
    }

    printf("[CFG] Saved OK (CRC=0x%08lX)\n", write_buf.crc32);
    return true;
}
