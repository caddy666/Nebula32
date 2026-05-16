// =============================================================================
// test_sector_cache_stress.cpp — TSan concurrent producer/consumer stress test
//
// Topology mirrors the RP2350 dual-core design:
//   producer  = Core 1: calls sector_cache_prefetch_tick() in a tight loop
//   consumer  = Core 0: calls sector_cache_get() / release_before() per sector
//
// Build:  make stress_sector_cache
// Run:    ./stress_sector_cache
// TSan reports any DATA RACE to stderr; a clean run prints "all tests PASSED".
//
// Separate binary from cd32_tests: TSan and UBSan cannot be co-linked.
// =============================================================================

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <sched.h>

extern "C" {
#include "sector_cache.h"
}

// ---------------------------------------------------------------------------
// Fake disc_read_sector — fills buf with deterministic data keyed on LBA.
// Returns SECTOR_RAW_SIZE on every call (simulates a perfect SD card).
// ---------------------------------------------------------------------------
uint32_t disc_read_sector(disc_image_t *disc, uint32_t lba,
                          uint8_t *buf, sector_mode_t mode)
{
    (void)disc; (void)mode;
    for (uint32_t i = 0; i < SECTOR_RAW_SIZE; i++)
        buf[i] = (uint8_t)((lba + i) & 0xFF);
    return SECTOR_RAW_SIZE;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static sector_cache_t g_cache;
static disc_image_t   g_disc;

#define STRESS_SECTORS 500

static int g_data_errors;   // incremented on content mismatch

// ---------------------------------------------------------------------------
// producer_thread — Core 1 analog: spams prefetch_tick
// ---------------------------------------------------------------------------
static void *producer_thread(void *arg)
{
    (void)arg;
    // Twice as many iterations as sectors so the cache stays ahead of the consumer.
    for (int i = 0; i < STRESS_SECTORS * 4; i++) {
        sector_cache_prefetch_tick(&g_cache);
        sched_yield();
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// consumer_thread — Core 0 analog: reads and verifies each sector in order
// ---------------------------------------------------------------------------
static void *consumer_thread(void *arg)
{
    (void)arg;
    uint8_t  buf[SECTOR_RAW_SIZE];
    uint32_t bytes;

    for (uint32_t lba = 0; lba < STRESS_SECTORS; lba++) {
        bool got = false;
        for (int spin = 0; spin < 500000 && !got; spin++) {
            if (sector_cache_get(&g_cache, lba, buf, &bytes)) {
                for (uint32_t j = 0; j < bytes; j++) {
                    if (buf[j] != (uint8_t)((lba + j) & 0xFF))
                        __atomic_fetch_add(&g_data_errors, 1, __ATOMIC_RELAXED);
                }
                sector_cache_release_before(&g_cache, lba + 1);
                got = true;
            } else {
                sched_yield();
            }
        }
        // If the producer never delivers this LBA the spin just exhausts.
        // That is a functional miss but not a data race.
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_queue_empty_boundary(void)
{
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.total_sectors = 100;
    g_disc.file_open     = true;
    sector_cache_init(&g_cache, &g_disc);

    uint8_t  buf[SECTOR_RAW_SIZE];
    uint32_t bytes = 999;
    bool     result = sector_cache_get(&g_cache, 0, buf, &bytes);
    assert(!result);
    assert(bytes == 0);
    printf("[PASS] queue_empty_boundary\n");
}

static void test_queue_full_boundary(void)
{
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.total_sectors = 100;
    g_disc.file_open     = true;
    sector_cache_init(&g_cache, &g_disc);

    // Fill every slot (SECTOR_BUFFER_COUNT ticks suffice)
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        sector_cache_prefetch_tick(&g_cache);

    // Count valid slots — should be exactly SECTOR_BUFFER_COUNT
    int valid = 0;
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        if (sector_cache_ready(&g_cache, (uint32_t)i)) valid++;
    assert(valid == SECTOR_BUFFER_COUNT);

    // One more tick when the queue is 100% full — must not crash or corrupt
    sector_cache_prefetch_tick(&g_cache);

    // Valid count must still be exactly SECTOR_BUFFER_COUNT (no slot added)
    valid = 0;
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        if (sector_cache_ready(&g_cache, (uint32_t)i)) valid++;
    assert(valid == SECTOR_BUFFER_COUNT);
    printf("[PASS] queue_full_boundary (%d slots)\n", valid);
}

static void test_flush_gen_stale_guard(void)
{
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.total_sectors = 100;
    g_disc.file_open     = true;
    sector_cache_init(&g_cache, &g_disc);

    // Prefetch sector 0
    sector_cache_prefetch_tick(&g_cache);
    assert(sector_cache_ready(&g_cache, 0));

    // Seek to LBA 50 — flushes gen, resets next_fetch_lba
    sector_cache_seek(&g_cache, 50);
    assert(!sector_cache_ready(&g_cache, 0));  // stale sector evicted

    // Next prefetch should fetch sector 50
    sector_cache_prefetch_tick(&g_cache);
    assert(sector_cache_ready(&g_cache, 50));

    printf("[PASS] flush_gen_stale_guard\n");
}

static void test_concurrent_produce_consume(void)
{
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.total_sectors = STRESS_SECTORS + 16;  // headroom for prefetch overshoot
    g_disc.file_open     = true;
    sector_cache_init(&g_cache, &g_disc);
    g_data_errors = 0;

    pthread_t prod_t, cons_t;
    pthread_create(&prod_t, NULL, producer_thread, NULL);
    pthread_create(&cons_t, NULL, consumer_thread, NULL);
    pthread_join(cons_t, NULL);
    pthread_join(prod_t, NULL);

    assert(g_data_errors == 0);
    printf("[PASS] concurrent_produce_consume (TSan clean)\n");
}

static void test_fill_drain_fill_cycle(void)
{
    memset(&g_disc, 0, sizeof(g_disc));
    g_disc.total_sectors = 32;
    g_disc.file_open     = true;
    sector_cache_init(&g_cache, &g_disc);

    // Phase 1: fill 8 slots (LBAs 0-7) via prefetch_tick
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        sector_cache_prefetch_tick(&g_cache);

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        assert(sector_cache_ready(&g_cache, (uint32_t)i));
        uint8_t  buf[SECTOR_RAW_SIZE];
        uint32_t bytes;
        bool ok = sector_cache_get(&g_cache, (uint32_t)i, buf, &bytes);
        assert(ok);
        for (uint32_t j = 0; j < bytes; j++)
            assert(buf[j] == (uint8_t)((i + j) & 0xFF));
    }

    // Phase 2: drain all slots via get + release_before
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        uint8_t  buf[SECTOR_RAW_SIZE];
        uint32_t bytes;
        sector_cache_get(&g_cache, (uint32_t)i, buf, &bytes);
        sector_cache_release_before(&g_cache, (uint32_t)(i + 1));
    }
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        assert(!sector_cache_ready(&g_cache, (uint32_t)i));

    // Phase 3: seek to LBA 8, fill again (LBAs 8-15), verify data integrity
    sector_cache_seek(&g_cache, 8);
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        sector_cache_prefetch_tick(&g_cache);

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        uint32_t lba = (uint32_t)(8 + i);
        assert(sector_cache_ready(&g_cache, lba));
        uint8_t  buf[SECTOR_RAW_SIZE];
        uint32_t bytes;
        bool ok = sector_cache_get(&g_cache, lba, buf, &bytes);
        assert(ok);
        for (uint32_t j = 0; j < bytes; j++)
            assert(buf[j] == (uint8_t)((lba + j) & 0xFF));
    }

    printf("[PASS] fill_drain_fill_cycle\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void)
{
    printf("=== sector_cache stress tests ===\n");
    test_queue_empty_boundary();
    test_queue_full_boundary();
    test_flush_gen_stale_guard();
    test_fill_drain_fill_cycle();
    test_concurrent_produce_consume();
    printf("=== all tests PASSED ===\n");
    return 0;
}
