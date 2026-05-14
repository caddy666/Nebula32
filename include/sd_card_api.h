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

// Scan the SD card root directory for supported disc image files.
// Fills 'paths[0..max_count-1]' with full FatFS paths (e.g. "0:/game.iso").
// Returns the number of images found.
uint32_t sd_scan_images(char paths[][MAX_PATH_LEN], uint32_t max_count);

// Returns true if the SD card is mounted and accessible
bool sd_card_is_ready(void);

