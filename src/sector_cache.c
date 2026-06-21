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
// Synchronisation uses GCC __atomic_* builtins with ACQUIRE/RELEASE ordering.
// This is recognised by both TSan (host) and generates LDAR/STLR on Cortex-M33
// — stronger and more portable than the previous volatile + __dmb() approach.
// =============================================================================

#include "sector_cache.h"
#include "disc_image.h"
#include "sram_attr.h"
#include "logger.h"      // SD card activity logging
#include "psram.h"
#include "pico/stdlib.h"
#include "pico/assert.h"

#include <string.h>
#include <stdio.h>

// SRAM fallback slot array — used when PSRAM is absent or allocation fails.
static sector_slot_t s_sram_slots[SECTOR_BUFFER_COUNT];

// ---------------------------------------------------------------------------
// sector_cache_init
// ---------------------------------------------------------------------------
void sector_cache_init(sector_cache_t *cache, disc_image_t *disc) {
    hard_assert(cache != NULL, "sector_cache_init: cache is NULL");
    memset(cache, 0, sizeof(*cache));

#ifdef BUILD_WITH_PSRAM
    void *psram_slots = psram_alloc(
        (size_t)SECTOR_BUFFER_COUNT_PSRAM * sizeof(sector_slot_t));
    if (psram_slots) {
        cache->slots      = (sector_slot_t *)psram_slots;
        cache->slot_count = SECTOR_BUFFER_COUNT_PSRAM;
        printf("[CACHE] %u slots in PSRAM (%u KB read-ahead)\n",
               SECTOR_BUFFER_COUNT_PSRAM,
               (unsigned)((size_t)SECTOR_BUFFER_COUNT_PSRAM
                          * sizeof(sector_slot_t) / 1024u));
    } else {
        cache->slots      = s_sram_slots;
        cache->slot_count = SECTOR_BUFFER_COUNT;
        printf("[CACHE] PSRAM alloc failed — %u SRAM slots\n",
               SECTOR_BUFFER_COUNT);
    }
#else
    cache->slots      = s_sram_slots;
    cache->slot_count = SECTOR_BUFFER_COUNT;
#endif

    memset(cache->slots, 0, cache->slot_count * sizeof(sector_slot_t));
    cache->disc             = disc;
    cache->sector_mode      = SECTOR_MODE_DATA;
    cache->next_write_slot  = 0;
}

// ---------------------------------------------------------------------------
// sector_cache_is_full  (Core 1, called before deciding to sleep)
// ---------------------------------------------------------------------------
bool sector_cache_is_full(sector_cache_t *cache) {
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        if (!__atomic_load_n(&cache->slots[i].valid, __ATOMIC_ACQUIRE))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sector_cache_flush
// ---------------------------------------------------------------------------
// Immediately discard all buffered sectors.
// Called on seek (LBA discontinuity) — stale sectors ahead of the new
// position would be delivered to the wrong place in the disc.
void sector_cache_flush(sector_cache_t *cache) {
    // Bump generation so any in-flight Core 1 read discards its result.
    // ACQ_REL ensures the increment is visible to Core 1 before we clear valid flags.
    __atomic_fetch_add(&cache->flush_gen, 1u, __ATOMIC_ACQ_REL);
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        __atomic_store_n(&cache->slots[i].valid, false, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&cache->next_write_slot, 0u, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// sector_cache_seek
// ---------------------------------------------------------------------------
// Flush the cache and start prefetching from 'lba'.
// Called by commo_bridge.c when a SEEK_OPC or JUMP_TRACKS_OPC arrives.
void sector_cache_seek(sector_cache_t *cache, uint32_t lba) {
    sector_cache_flush(cache);
    __atomic_store_n(&cache->next_fetch_lba, lba, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// sector_cache_ready  (Core 0, lightweight presence check)
// ---------------------------------------------------------------------------
// Returns true if the cache holds a valid entry for `lba`, WITHOUT copying
// any data.  Used by the test suite to verify prefetch state.
bool sector_cache_ready(sector_cache_t *cache, uint32_t lba) {
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        sector_slot_t *slot = &cache->slots[i];
        if (__atomic_load_n(&slot->valid, __ATOMIC_ACQUIRE) && slot->lba == lba)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Attempt to find sector 'lba' in the cache.
// On hit: copies up to SECTOR_RAW_SIZE bytes into buf_out, sets *bytes_out.
// On miss: sets *bytes_out = 0, returns false.
//
// Linear scan is O(SECTOR_BUFFER_COUNT) = O(8) — negligible compared to the
// ~6.7 ms between sector deliveries at 2× speed.
bool __not_in_flash_func(sector_cache_get)(sector_cache_t *cache, uint32_t lba,
                                           uint8_t *buf_out, uint32_t *bytes_out) {
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        sector_slot_t *slot = &cache->slots[i];
        // Acquire load: ensures slot->data and slot->valid_bytes are visible
        // if the RELEASE store that set valid=true has already happened.
        if (__atomic_load_n(&slot->valid, __ATOMIC_ACQUIRE) && slot->lba == lba) {
            uint32_t n = slot->valid_bytes;
            if (n > SECTOR_RAW_SIZE) n = SECTOR_RAW_SIZE;
            memcpy(buf_out, slot->data, n);
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
void __not_in_flash_func(sector_cache_release_before)(sector_cache_t *cache, uint32_t current_lba) {
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        sector_slot_t *slot = &cache->slots[i];
        if (__atomic_load_n(&slot->valid, __ATOMIC_ACQUIRE) && slot->lba < current_lba) {
            __atomic_store_n(&slot->valid, false, __ATOMIC_RELEASE);
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
    hard_assert(cache != NULL, "sector_cache_prefetch_tick: cache is NULL");

    // ---- Find a free slot (round-robin to distribute writes evenly) ----
    uint32_t start = __atomic_load_n(&cache->next_write_slot, __ATOMIC_ACQUIRE);
    int free_slot = -1;
    for (uint32_t i = 0; i < cache->slot_count; i++) {
        uint32_t idx = (start + i) % cache->slot_count;
        if (!__atomic_load_n(&cache->slots[idx].valid, __ATOMIC_ACQUIRE)) {
            free_slot = (int)idx;
            break;
        }
    }
    if (free_slot < 0) return;   // All slots full — back-pressure from Core 0

    // ---- Check we haven't reached the end of the disc ----
    if (!cache->disc || !cache->disc->file_open) return;

    // Snapshot next_fetch_lba atomically so all uses in this call are consistent.
    uint32_t fetch_lba = __atomic_load_n(&cache->next_fetch_lba, __ATOMIC_ACQUIRE);
    if (fetch_lba >= cache->disc->total_sectors) return;

    // ---- Claim the slot (mark invalid while writing) ----
    sector_slot_t *slot = &cache->slots[free_slot];
    __atomic_store_n(&slot->valid, false, __ATOMIC_RELEASE);
    slot->error = false;
    slot->lba   = fetch_lba;

    // Snapshot the generation counter before we start the SD read.
    // If Core 0 calls sector_cache_flush() while we're reading, flush_gen
    // will increment and we must NOT mark the slot valid — the LBA is stale.
    uint32_t my_gen = __atomic_load_n(&cache->flush_gen, __ATOMIC_ACQUIRE);

    // ---- Read from SD card via disc_image layer (retry once on transient error) ----
    uint32_t bytes = disc_read_sector(cache->disc, fetch_lba, slot->data,
                                      SECTOR_MODE_RAW);
    if (bytes == 0) {
        // SD cards exhibit ~1% transient error rates (CRC, wake-up latency).
        // A single retry recovers the vast majority without adding perceptible delay.
        bytes = disc_read_sector(cache->disc, fetch_lba, slot->data, SECTOR_MODE_RAW);
    }
    if (bytes > 0) {
        slot->valid_bytes = bytes;
        // Only commit if Core 0 hasn't flushed since we started.
        // The RELEASE store on valid creates the happens-before edge so
        // Core 0's ACQUIRE load sees the fully written slot->data.
        if (__atomic_load_n(&cache->flush_gen, __ATOMIC_ACQUIRE) == my_gen) {
            __atomic_store_n(&cache->next_write_slot,
                             ((uint32_t)free_slot + 1u) % cache->slot_count,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&slot->valid, true, __ATOMIC_RELEASE);
            __atomic_store_n(&cache->next_fetch_lba, fetch_lba + 1, __ATOMIC_RELEASE);
        }
        /* If gen mismatched, Core 0 already set next_fetch_lba via seek —
         * do not advance it or the first sector of the new seek is skipped. */
    } else {
        slot->error       = true;
        slot->valid_bytes = 0;
        LOG_ERROR_MSG("cache prefetch SD read fail at LBA=%lu (after retry)",
                      (unsigned long)fetch_lba);
        printf("[CACHE] Read error at LBA %u\n", (unsigned)fetch_lba);
        // Mirror the success path: only commit if Core 0 hasn't flushed/seeked
        // since we started.  Without this check, a seek during the retry window
        // commits a stale error sentinel at the post-seek slot index, causing a
        // false permanent-miss on the first sector of the new seek position.
        if (__atomic_load_n(&cache->flush_gen, __ATOMIC_ACQUIRE) == my_gen) {
            __atomic_store_n(&cache->next_write_slot,
                             ((uint32_t)free_slot + 1u) % cache->slot_count,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&slot->valid, true, __ATOMIC_RELEASE);
            __atomic_store_n(&cache->next_fetch_lba, fetch_lba + 1, __ATOMIC_RELEASE);
        }
    }
}
