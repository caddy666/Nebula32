#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "sector_cache.h"
#include "disc_image.h"
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Inject a sector slot directly into the cache for testing without SD I/O. */
static void inject_sector(sector_cache_t *cache, int slot_idx,
                           uint32_t lba, uint8_t fill, uint32_t valid_bytes)
{
    sector_slot_t *s = &cache->slots[slot_idx];
    s->lba         = lba;
    s->valid_bytes = valid_bytes;
    s->error       = false;
    memset(s->data, fill, valid_bytes);
    s->valid = true;
}

TEST_GROUP(SectorCache)
{
    sector_cache_t cache;
    disc_image_t   disc;

    void setup() {
        memset(&disc, 0, sizeof(disc));
        disc.file_open = false;
        sector_cache_init(&cache, &disc);
    }
};

/* After init: all slots should be invalid and next_fetch_lba should be 0. */
TEST(SectorCache, InitAllSlotsInvalid)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        CHECK_FALSE(cache.slots[i].valid);
    }
    LONGS_EQUAL(0, (long)cache.next_fetch_lba);
}

/* ready() on an empty cache should return false for any LBA. */
TEST(SectorCache, ReadyOnEmptyIsFalse)
{
    CHECK_FALSE(sector_cache_ready(&cache, 0));
    CHECK_FALSE(sector_cache_ready(&cache, 100));
    CHECK_FALSE(sector_cache_ready(&cache, 0xFFFFFFFFu));
}

/* get() on an empty cache should return false and set bytes_out=0. */
TEST(SectorCache, GetOnEmptyReturnsFalse)
{
    uint8_t buf[SECTOR_RAW_SIZE];
    uint32_t bytes = 999;
    CHECK_FALSE(sector_cache_get(&cache, 0, buf, &bytes));
    LONGS_EQUAL(0, (long)bytes);
}

/* Manual slot injection: set a slot's valid/lba/data, then get() finds it. */
TEST(SectorCache, InjectedSlotFoundByGet)
{
    inject_sector(&cache, 0, 42, 0xAB, 2352);

    uint8_t buf[SECTOR_RAW_SIZE];
    uint32_t bytes = 0;
    CHECK_TRUE(sector_cache_get(&cache, 42, buf, &bytes));
    LONGS_EQUAL(2352, (long)bytes);
    BYTES_EQUAL(0xAB, buf[0]);
    BYTES_EQUAL(0xAB, buf[2351]);
}

/* Injected slot: ready() returns true for the correct LBA. */
TEST(SectorCache, ReadyTrueForInjectedLba)
{
    inject_sector(&cache, 2, 777, 0xCC, 2352);
    CHECK_TRUE(sector_cache_ready(&cache, 777));
    CHECK_FALSE(sector_cache_ready(&cache, 778));
}

/* Get wrong LBA returns false. */
TEST(SectorCache, GetWrongLbaReturnsFalse)
{
    inject_sector(&cache, 0, 100, 0x11, 2352);
    uint8_t buf[SECTOR_RAW_SIZE];
    uint32_t bytes = 0;
    CHECK_FALSE(sector_cache_get(&cache, 101, buf, &bytes));
    LONGS_EQUAL(0, (long)bytes);
}

/* release_before(N): clears slots with lba < N, keeps lba >= N. */
TEST(SectorCache, ReleaseBeforeClearsOldSlots)
{
    inject_sector(&cache, 0, 10, 0x10, 2352);
    inject_sector(&cache, 1, 20, 0x20, 2352);
    inject_sector(&cache, 2, 30, 0x30, 2352);

    sector_cache_release_before(&cache, 25);  /* free LBA < 25 */

    CHECK_FALSE(cache.slots[0].valid);   /* LBA 10 freed */
    CHECK_FALSE(cache.slots[1].valid);   /* LBA 20 freed */
    CHECK_TRUE (cache.slots[2].valid);   /* LBA 30 kept  */
}

/* seek(X): sets next_fetch_lba=X and flushes all slots. */
TEST(SectorCache, SeekFlushesAndSetsLba)
{
    inject_sector(&cache, 0, 50, 0xAA, 2352);
    inject_sector(&cache, 1, 51, 0xBB, 2352);

    sector_cache_seek(&cache, 500);

    LONGS_EQUAL(500, (long)cache.next_fetch_lba);
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        CHECK_FALSE(cache.slots[i].valid);
    }
}

/* flush: all slots become invalid. */
TEST(SectorCache, FlushInvalidatesAllSlots)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        inject_sector(&cache, i, (uint32_t)i * 10, 0xFF, 2352);
    }

    sector_cache_flush(&cache);

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        CHECK_FALSE(cache.slots[i].valid);
    }
}

/* flush_gen increments on each flush so Core 1 can detect stale in-flight reads. */
TEST(SectorCache, FlushGenIncrementsOnFlush)
{
    uint32_t gen0 = cache.flush_gen;
    sector_cache_flush(&cache);
    LONGS_EQUAL((long)(gen0 + 1), (long)cache.flush_gen);
    sector_cache_flush(&cache);
    LONGS_EQUAL((long)(gen0 + 2), (long)cache.flush_gen);
}

/* seek() calls flush() which also bumps flush_gen. */
TEST(SectorCache, SeekBumpsFlushGen)
{
    uint32_t gen0 = cache.flush_gen;
    sector_cache_seek(&cache, 100);
    CHECK(cache.flush_gen > gen0);
}

/* Multiple slots can be populated and individually retrieved by LBA. */
TEST(SectorCache, MultipleSlotsByLba)
{
    inject_sector(&cache, 0, 100, 0xAA, 2352);
    inject_sector(&cache, 1, 200, 0xBB, 2352);
    inject_sector(&cache, 2, 300, 0xCC, 2352);

    uint8_t buf[SECTOR_RAW_SIZE];
    uint32_t bytes = 0;

    CHECK_TRUE(sector_cache_get(&cache, 100, buf, &bytes));
    BYTES_EQUAL(0xAA, buf[0]);

    CHECK_TRUE(sector_cache_get(&cache, 200, buf, &bytes));
    BYTES_EQUAL(0xBB, buf[0]);

    CHECK_TRUE(sector_cache_get(&cache, 300, buf, &bytes));
    BYTES_EQUAL(0xCC, buf[0]);
}
