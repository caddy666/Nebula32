// =============================================================================
// sd_card.c — SD card interface via no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
// =============================================================================
//
// This file configures the SD card library for 4-bit SDIO mode on the
// Raspberry Pi Pico 2 (RP2350) and provides a thin wrapper for mounting
// the FAT filesystem and listing available disc images.
//
// Library: no-OS-FatFS-SD-SDIO-SPI-RPi-Pico by Carl Kugler
//   https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
//
// Hardware wiring (as defined in CMakeLists.txt):
//   GPIO 18 — SDIO_CLK    (SD card CLK)
//   GPIO 19 — SDIO_CMD    (SD card CMD)
//   GPIO 20 — SDIO_D0     (SD card DAT0)
//   GPIO 21 — SDIO_D1     (SD card DAT1)
//   GPIO 22 — SDIO_D2     (SD card DAT2)
//   GPIO 23 — SDIO_D3     (SD card DAT3 / CS)
//
// 4-bit SDIO achieves ~20–25 MB/s — comfortably faster than the 300 KB/s
// required for real-time 2x CD-ROM streaming (2352 bytes × 150/sec ≈ 353 KB/s).
// =============================================================================

#include "sd_card_api.h"
#include "hw_config.h"   // no-OS-FatFS hardware configuration
#include "ff.h"          // FatFS

#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// FatFS filesystem object (one per volume; we use volume "0:")
static FATFS s_fs;
static bool  s_mounted = false;

// =============================================================================
// no-OS-FatFS hardware configuration
// =============================================================================
// The library uses a hw_config.c / hw_config.h pattern.
// We define the SDIO bus configuration here to keep it centralised.
// If your project already has hw_config.c from the library, remove this
// section and add your configuration there instead.
// =============================================================================

// NOTE: sd_get_num() and sd_get_by_num() are defined in hw_config.c,
// which is the canonical hardware configuration file required by the
// no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library.

// =============================================================================
// Initialise and mount the SD card
// =============================================================================

bool sd_card_init_and_mount(void) {
    printf("[SD] Initialising SD card (4-bit SDIO)...\n");

    // The no-OS-FatFS library initialises SD hardware automatically when
    // f_mount is called.  We just need to call f_mount with our FATFS object.
    FRESULT fr = f_mount(&s_fs, "0:", 1);  // "1" = mount immediately

    if (fr != FR_OK) {
        printf("[SD] Mount failed: FatFS error %d\n", fr);
        printf("[SD] Check SD card is inserted and formatted FAT32/exFAT\n");
        return false;
    }

    s_mounted = true;
    printf("[SD] Mounted successfully\n");

    // Print free space as a sanity check
    DWORD fre_clust;
    FATFS *fsp = &s_fs;
    fr = f_getfree("0:", &fre_clust, &fsp);
    if (fr == FR_OK) {
        uint64_t free_mb = ((uint64_t)fre_clust * s_fs.csize * 512) / (1024 * 1024);
        printf("[SD] Free space: %llu MB\n", free_mb);
    }

    return true;
}

// =============================================================================
// Scan for disc images in the root directory
// =============================================================================
// Fills 'paths' with up to 'max_count' image file paths.
// Supported extensions: .iso, .bin, .nrg, .mdf
// Returns the number of images found.

static bool is_image_file(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".iso") == 0 ||
            strcasecmp(ext, ".bin") == 0 ||
            strcasecmp(ext, ".nrg") == 0 ||
            strcasecmp(ext, ".mdf") == 0);
}

uint32_t sd_scan_images(char paths[][MAX_PATH_LEN], uint32_t max_count,
                        const char *base_dir) {
    if (!s_mounted) return 0;
    if (!base_dir || base_dir[0] == '\0') base_dir = "0:/";

    DIR     dir;
    FILINFO fno;
    uint32_t count = 0;

    FRESULT fr = f_opendir(&dir, base_dir);
    if (fr != FR_OK) {
        printf("[SD] Cannot open image directory: %s\n", base_dir);
        return 0;
    }

    while (count < max_count) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') break;

        if (fno.fattrib & AM_DIR) continue;

        if (is_image_file(fno.fname)) {
            snprintf(paths[count], MAX_PATH_LEN, "%s%s", base_dir, fno.fname);
            printf("[SD] Found image: %s (%lu KB)\n",
                   fno.fname, (uint32_t)(fno.fsize / 1024));
            count++;
        }
    }

    f_closedir(&dir);
    printf("[SD] Total images found: %lu\n", count);
    return count;
}

// Check if the SD card is still present and accessible
bool sd_card_is_ready(void) {
    return s_mounted;
}
