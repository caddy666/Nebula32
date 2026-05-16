// =============================================================================
// test_ipc_desync.cpp — RP2350 dual-core IPC and FIFO desync boundary tests
//
// Tests the sector_cache ring buffer under conditions that mirror Core 1 stalls
// (SD card latency spikes) and FIFO saturation (all slots filled).  The
// DaReplica state machine replicates da_output.c ping-pong DMA; FakeAkiko
// records deliveries.
//
// inject_sector() writes data directly into cache slots, bypassing
// disc_read_sector (which returns 0 bytes in this binary's disc_image_stub.c).
// =============================================================================

#include <CppUTest/TestHarness.h>
extern "C" {
#include "sector_cache.h"
}
#include <string.h>
#include <stdint.h>

namespace {

static void inject_sector(sector_cache_t *cache, int slot, uint32_t lba)
{
    sector_slot_t *s = &cache->slots[slot];
    for (uint32_t i = 0; i < SECTOR_RAW_SIZE; i++)
        s->data[i] = (uint8_t)((lba + i) & 0xFF);
    s->lba         = lba;
    s->valid_bytes = SECTOR_RAW_SIZE;
    s->error       = false;
    s->valid       = true;
}

static const int DA_WORDS = 588 * 2;

struct FakeAkikoIpc {
    static const int MAX = 32;
    uint32_t lbas[MAX];
    int      count;
    void reset()  { count = 0; }
    void receive(uint32_t lba) { if (count < MAX) lbas[count++] = lba; }
    bool contains(uint32_t lba) const {
        for (int i = 0; i < count; i++) if (lbas[i] == lba) return true;
        return false;
    }
};

struct DaReplicaIpc {
    uint32_t        buf[2][DA_WORDS];
    uint32_t        buf_lba[2];
    uint32_t        next_lba;
    bool            playing;
    int             active_buf;
    sector_cache_t *cache;
    FakeAkikoIpc   *akiko;

    void _expand(const uint8_t *raw, uint32_t *out) {
        for (int i = 0; i < 588; i++) {
            uint16_t l = (uint16_t)(raw[i*4+0]) | ((uint16_t)(raw[i*4+1]) << 8);
            uint16_t r = (uint16_t)(raw[i*4+2]) | ((uint16_t)(raw[i*4+3]) << 8);
            out[i*2+0] = (uint32_t)l << 16;
            out[i*2+1] = (uint32_t)r << 16;
        }
    }

    bool start(sector_cache_t *c, uint32_t start_lba, FakeAkikoIpc *a) {
        cache      = c;
        akiko      = a;
        next_lba   = start_lba;
        playing    = false;
        active_buf = 0;

        uint8_t  raw[SECTOR_RAW_SIZE];
        uint32_t bytes;

        if (!sector_cache_get(cache, next_lba, raw, &bytes)) return false;
        _expand(raw, buf[0]);
        sector_cache_release_before(cache, next_lba);
        buf_lba[0] = next_lba++;

        if (sector_cache_get(cache, next_lba, raw, &bytes)) {
            _expand(raw, buf[1]);
            sector_cache_release_before(cache, next_lba);
            buf_lba[1] = next_lba++;
        } else {
            memset(buf[1], 0, sizeof(buf[1]));
            buf_lba[1] = UINT32_MAX;
        }

        playing = true;
        return true;
    }

    bool tick() {
        if (!playing) return false;

        int done_buf = active_buf;
        akiko->receive(buf_lba[done_buf]);

        uint8_t  raw[SECTOR_RAW_SIZE];
        uint32_t bytes;
        if (sector_cache_get(cache, next_lba, raw, &bytes)) {
            _expand(raw, buf[done_buf]);
            sector_cache_release_before(cache, next_lba);
            buf_lba[done_buf] = next_lba++;
        } else {
            playing = false;
        }

        active_buf ^= 1;
        return playing;
    }

    void stop() {
        playing    = false;
        next_lba   = 0;
        active_buf = 0;
        memset(buf, 0, sizeof(buf));
    }
};

} // anonymous namespace

TEST_GROUP(CoreIpcDesync)
{
    sector_cache_t  cache;
    disc_image_t    fake_disc;
    FakeAkikoIpc    akiko;
    DaReplicaIpc    da;

    void setup() {
        memset(&fake_disc, 0, sizeof(fake_disc));
        fake_disc.total_sectors = 200;
        sector_cache_init(&cache, &fake_disc);
        akiko.reset();
        memset(&da, 0, sizeof(da));
    }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Core1Stall_MidPlay_GracefulStop — inject 3 sectors; start() pre-loads two,
// tick 1 loads the third into the refill slot, tick 2 misses LBA 3 and clears
// playing.  Verify the DaReplica stops cleanly: playing=false, exactly two
// LBAs delivered (the pre-loaded pair released by start()).
// ---------------------------------------------------------------------------
TEST(CoreIpcDesync, Core1Stall_MidPlay_GracefulStop)
{
    for (int i = 0; i < 3; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));

    while (da.tick()) {}

    CHECK_FALSE(da.playing);
    LONGS_EQUAL(2, akiko.count);   // LBAs 0 and 1 delivered before cache miss
}

// ---------------------------------------------------------------------------
// Core1Stall_TickAfterStopIsSafe — a tick() on a stopped DaReplica immediately
// returns false without delivering any additional LBA to akiko.
// ---------------------------------------------------------------------------
TEST(CoreIpcDesync, Core1Stall_TickAfterStopIsSafe)
{
    for (int i = 0; i < 3; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    while (da.tick()) {}

    int count_before = akiko.count;
    CHECK_FALSE(da.tick());
    LONGS_EQUAL(count_before, akiko.count);
}

// ---------------------------------------------------------------------------
// SioFifo_AllSlotsFull_PrefetchBlocksSafely — when every cache slot is valid,
// prefetch_tick hits the free_slot == -1 guard and returns without reading the
// disc or corrupting any slot.  Valid count stays exactly SECTOR_BUFFER_COUNT.
// ---------------------------------------------------------------------------
TEST(CoreIpcDesync, SioFifo_AllSlotsFull_PrefetchBlocksSafely)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    int valid_before = 0;
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        if (sector_cache_ready(&cache, (uint32_t)i)) valid_before++;

    sector_cache_prefetch_tick(&cache);   // all full → back-pressure, no-op

    int valid_after = 0;
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        if (sector_cache_ready(&cache, (uint32_t)i)) valid_after++;

    LONGS_EQUAL(SECTOR_BUFFER_COUNT, valid_before);
    LONGS_EQUAL(SECTOR_BUFFER_COUNT, valid_after);
}

// ---------------------------------------------------------------------------
// FillDrainFill_SecondBatchCorrect — drain a fully populated cache then inject
// a second batch at a higher LBA range; every new sector must be accessible
// with the correct deterministic data pattern.
// ---------------------------------------------------------------------------
TEST(CoreIpcDesync, FillDrainFill_SecondBatchCorrect)
{
    // Phase 1: fill slots 0-7 with LBAs 0-7
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    // Phase 2: drain all via get + release_before
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        uint8_t  buf[SECTOR_RAW_SIZE];
        uint32_t bytes;
        CHECK_TRUE(sector_cache_get(&cache, (uint32_t)i, buf, &bytes));
        sector_cache_release_before(&cache, (uint32_t)(i + 1));
    }

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        CHECK_FALSE(sector_cache_ready(&cache, (uint32_t)i));

    // Phase 3: seek and inject LBAs 100-107
    sector_cache_seek(&cache, 100);
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)(100 + i));

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++) {
        uint32_t lba = (uint32_t)(100 + i);
        uint8_t  buf[SECTOR_RAW_SIZE];
        uint32_t bytes;
        CHECK_TRUE(sector_cache_get(&cache, lba, buf, &bytes));
        LONGS_EQUAL(SECTOR_RAW_SIZE, (int)bytes);
        BYTES_EQUAL((uint8_t)(lba & 0xFF), buf[0]);
    }
}

// ---------------------------------------------------------------------------
// SilencePad_OnlyOneSector_Buf1IsUInt32Max — when start() can only pre-load
// one sector the DaReplica sets buf_lba[1]=UINT32_MAX (silence pad).
// One tick delivers LBA 0 then misses LBA 1 → playing stops, and UINT32_MAX
// is never passed to akiko.receive().
// ---------------------------------------------------------------------------
TEST(CoreIpcDesync, SilencePad_OnlyOneSector_Buf1IsUInt32Max)
{
    inject_sector(&cache, 0, 0u);   // only LBA 0 available

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    CHECK_TRUE(da.buf_lba[1] == UINT32_MAX);

    da.tick();

    LONGS_EQUAL(1, akiko.count);
    LONGS_EQUAL(0u, akiko.lbas[0]);
    CHECK_FALSE(akiko.contains(UINT32_MAX));
    CHECK_FALSE(da.playing);
}
