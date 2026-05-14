// =============================================================================
// sector_cache.c — Read-ahead sector ring buffer
// =============================================================================
//
// Provides a lock-free ring buffer of pre-fetched CD sectors.
//
// THREADING MODEL:
//   Core 1 (writer): calls sector_cache_prefetch_tick() in a tight loop.
//     — Finds a free slot, reads the next sector from SD card, marks it valid.
//     — Only writes to slots[i].data, .lba, .valid, .error, .valid_bytes
//     — Only advances next_fetch_lba
//
//   Core 0 (reader): calls sector_cache_get() when a sector is due.
//     — Scans all slots for a matching LBA
//     — memcpy's data out; does NOT free the slot yet
//     — Calls sector_cache_release_before() after DMA completes to free slots
//
// The 'valid' flag is the only shared write/read field.  Core 1 sets it true
// after filling a slot; Core 0 sets it false after consuming the sector.
// On the RP2350 (Cortex-M33 dual-core), single-byte writes are atomic, so no
// explicit lock is needed for this one flag — the data is written before the
// flag, and the flag is cleared before we overwrite the data.
//
// The 'volatile' qualifier on 'valid' and 'next_fetch_lba' ensures the
// compiler generates real load/store instructions and does not cache them in
// registers across the critical sections.
// =============================================================================

#include "sector_cache.h"
#include "disc_image.h"
#include "logger.h"      // SD card activity logging
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// sector_cache_init
// ---------------------------------------------------------------------------
void sector_cache_init(sector_cache_t *cache, disc_image_t *disc) {
    memset(cache, 0, sizeof(*cache));
    cache->disc           = disc;
    cache->next_fetch_lba = 0;
    cache->sector_mode    = SECTOR_MODE_DATA;
    // All slots start invalid (memset zeroed them, and valid==false==0)
}

// ---------------------------------------------------------------------------
// sector_cache_flush
// ---------------------------------------------------------------------------
// Immediately discard all buffered sectors.
// Called on seek (LBA discontinuity) — stale sectors ahead of the new
// position would be delivered to the wrong place in the disc.
void sector_cache_flush(sector_cache_t *cache) {
    // Bump generation so any in-flight Core 1 read discards its result.
    // The dmb() ensures Core 1 sees the incremented gen before we clear valid flags.
    cache->flush_gen++;
    __dmb();
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        cache->slots[i].valid = false;
    }
}

// ---------------------------------------------------------------------------
// sector_cache_seek
// ---------------------------------------------------------------------------
// Flush the cache and start prefetching from 'lba'.
// Called by commo_bridge.c when a SEEK_OPC or JUMP_TRACKS_OPC arrives.
void sector_cache_seek(sector_cache_t *cache, uint32_t lba) {
    sector_cache_flush(cache);
    cache->next_fetch_lba = lba;
}

// sector_cache_ready  (Core 0, lightweight presence check)
// ---------------------------------------------------------------------------
// Returns true if the cache holds a valid entry for `lba`, WITHOUT copying
// any data.  Used to check whether a sector is ready before starting DMA.
bool sector_cache_ready(sector_cache_t *cache, uint32_t lba) {
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        volatile sector_slot_t *slot = &cache->slots[i];
        if (slot->valid && slot->lba == lba) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Attempt to find sector 'lba' in the cache.
// On hit: copies up to SECTOR_RAW_SIZE bytes into buf_out, sets *bytes_out.
// On miss: sets *bytes_out = 0, returns false.
//
// Linear scan is O(SECTOR_BUFFER_COUNT) = O(8) — negligible compared to the
// ~6.7 ms between sector deliveries at 2× speed.
bool sector_cache_get(sector_cache_t *cache, uint32_t lba,
                      uint8_t *buf_out, uint32_t *bytes_out) {
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        // Read valid flag with volatile semantics first, then check LBA
        volatile sector_slot_t *slot = &cache->slots[i];
        if (slot->valid && slot->lba == lba) {
            uint32_t n = slot->valid_bytes;
            if (n > SECTOR_RAW_SIZE) n = SECTOR_RAW_SIZE;
            memcpy(buf_out, (const void *)slot->data, n);
            *bytes_out = n;
            return true;
        }
    }
    *bytes_out = 0;
    return false;
}

// ---------------------------------------------------------------------------
// sector_cache_release_before  (Core 0, called after DMA completes)
// ---------------------------------------------------------------------------
// Frees all cached slots with LBA strictly less than current_lba.
// This makes room for the Core 1 prefetch to continue filling ahead.
// Must be called regularly to prevent the cache from filling completely
// and stalling prefetch.
//
// Example: after delivering sector 200, call release_before(201).
// Slots for sectors 197-200 (if present) are freed for reuse.
void sector_cache_release_before(sector_cache_t *cache, uint32_t current_lba) {
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        // A consumed sector's LBA is always < current read position
        if (cache->slots[i].valid && cache->slots[i].lba < current_lba) {
            cache->slots[i].valid = false;   // Free the slot
        }
    }
}

// ---------------------------------------------------------------------------
// sector_cache_prefetch_tick  (Core 1 writer)
// ---------------------------------------------------------------------------
// Called from the Core 1 tight loop.  Reads one sector from SD card into
// the next free cache slot (if any).
//
// SD card read latency is ~0.1–0.5 ms per sector at 25 MHz SDIO.
// At 2× speed the sector period is 6.667 ms, so even a 0.5 ms read
// comfortably keeps up.  With 8 slots we have ~53 ms of buffer time.
void sector_cache_prefetch_tick(sector_cache_t *cache) {
    // ---- Find a free slot ----
    int free_slot = -1;
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        if (!cache->slots[i].valid) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) return;   // All slots full — back-pressure from Core 0

    // ---- Check we haven't reached the end of the disc ----
    if (!cache->disc || !cache->disc->file_open) return;
    if (cache->next_fetch_lba >= cache->disc->total_sectors) return;

    // ---- Claim the slot (mark invalid while writing) ----
    sector_slot_t *slot = &cache->slots[free_slot];
    slot->valid = false;   // Prevent Core 0 from reading a half-filled slot
    slot->error = false;
    slot->lba   = cache->next_fetch_lba;

    // Snapshot the generation counter before we start the SD read.
    // If Core 0 calls sector_cache_flush() while we're reading, the counter
    // will increment and we must NOT mark the slot valid — the LBA is stale.
    uint32_t my_gen = cache->flush_gen;

    // ---- Read from SD card via disc_image layer ----
    // disc_read_sector() handles ISO synthesis, raw BIN pass-through, etc.
    // Always fetch full 2352-byte raw sectors into the cache; the DA output
    // streams the complete raw sector to Akiko on each playback cycle.
    uint32_t bytes = disc_read_sector(cache->disc,
                                       cache->next_fetch_lba,
                                       slot->data,
                                       SECTOR_MODE_RAW);
    if (bytes > 0) {
        slot->valid_bytes = bytes;
        // Memory barrier: ensure data is visible before setting valid flag
        __dmb();               // RP2350 data memory barrier
        // Only commit if Core 0 hasn't flushed since we started — otherwise
        // this slot holds a stale LBA that would be delivered at the wrong position.
        if (cache->flush_gen == my_gen) {
            slot->valid = true;    // NOW Core 0 can see this sector
        }
    } else {
        slot->error = true;
        LOG_ERROR_MSG("cache prefetch SD read fail at LBA=%lu",
                      (unsigned long)cache->next_fetch_lba);
        printf("[CACHE] Read error at LBA %u\n", (unsigned)cache->next_fetch_lba);
    }

    cache->next_fetch_lba++;
}
