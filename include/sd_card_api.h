#pragma once
// =============================================================================
// sd_card.h — SD card interface declarations
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "disc_image.h"  // for MAX_PATH_LEN

// Initialise SDIO hardware and mount the FAT filesystem.
// Returns true on success.
bool sd_card_init_and_mount(void);

// Scan base_dir for supported disc image files (.iso/.bin/.nrg/.mdf).
// base_dir must be a FatFS path ending with '/' (e.g. "0:/" or "0:/games/").
// Skips the first 'offset' matching files, then fills up to max_count paths.
// Returns the number of paths written (may be less than max_count at end of dir).
uint32_t sd_scan_images(char paths[][MAX_PATH_LEN], uint32_t max_count,
                        const char *base_dir, uint32_t offset);

// Count all matching image files in base_dir without storing paths.
// Use this once at boot to determine total image count for pagination.
uint32_t sd_count_images(const char *base_dir);

// Returns true if the SD card is mounted and accessible
bool sd_card_is_ready(void);

// Scan base_dir for .uf2 firmware files.
// Same offset/max_count paging semantics as sd_scan_images().
uint32_t sd_scan_uf2_files(char paths[][MAX_PATH_LEN], uint32_t max_count,
                           const char *base_dir, uint32_t offset);

// Count all .uf2 files in base_dir without storing paths.
uint32_t sd_count_uf2_files(const char *base_dir);

// Scan base_dir (default "0:/playlists/") for .m3u playlist files.
// Same offset/max_count paging + alphabetical-sort semantics as sd_scan_images().
uint32_t sd_scan_m3u_files(char paths[][MAX_PATH_LEN], uint32_t max_count,
                           const char *base_dir, uint32_t offset);

