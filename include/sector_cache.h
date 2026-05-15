#pragma once
// =============================================================================
// sector_cache.h — Read-ahead sector buffer
// =============================================================================
//
// The CD32 reads sectors continuously during gameplay.  On real hardware the
// laser reads ~1–2 sectors ahead so data is waiting by the time the CPU asks.
// We replicate this with a ring buffer of pre-fetched sectors read from the
// SD card on Core 1 while Core 0 services the COMMO bus and DA output.
//
// Buffer size: SECTOR_BUFFER_COUNT × 2352 bytes
// At 8 sectors that is 18,816 bytes — fits comfortably in RP2350 SRAM.
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "pico/util/queue.h"
#include "disc_image.h"

#define SECTOR_BUFFER_COUNT  8      // Ring buffer depth
#define SECTOR_RAW_SIZE      2352   // Maximum raw sector bytes

// One slot in the ring buffer
typedef struct {
    uint32_t  lba;                        // Which sector is stored here
    uint8_t   data[SECTOR_RAW_SIZE];      // Raw sector data
    uint32_t  valid_bytes;                // How many bytes are valid (≤2352)
    bool      valid;                      // Slot contains a good read
    bool      error;                      // Read error flag
} sector_slot_t;

typedef struct {
    sector_slot_t  slots[SECTOR_BUFFER_COUNT];
    volatile int   head;           // Next slot to fill (writer moves this)
    volatile int   tail;           // Next slot to read (reader moves this)
    volatile uint32_t next_fetch_lba; // LBA the prefetch thread will fetch next
    volatile uint32_t flush_gen;   // Incremented on flush; Core 1 discards stale reads
    disc_image_t  *disc;           // Pointer to the open disc image
    sector_mode_t  sector_mode;    // Current mode (controls sector size)
} sector_cache_t;

void     sector_cache_init   (sector_cache_t *cache, disc_image_t *disc);
void     sector_cache_flush  (sector_cache_t *cache);
void     sector_cache_seek   (sector_cache_t *cache, uint32_t lba);
bool     sector_cache_ready  (sector_cache_t *cache, uint32_t lba);
bool     sector_cache_get    (sector_cache_t *cache, uint32_t lba,
                              uint8_t *buf_out, uint32_t *bytes_out);
void     sector_cache_prefetch_tick(sector_cache_t *cache);  // call from Core 1

// Free all slots with LBA < current_lba so prefetch can reuse them.
// Call this from Core 0 after each sector's DMA transfer completes.
void     sector_cache_release_before(sector_cache_t *cache, uint32_t current_lba);

