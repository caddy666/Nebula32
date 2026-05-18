// =============================================================================
// test_akiko_dma.cpp — Fake Akiko DMA engine tests (REPLICA pattern)
//
// Does NOT compile da_output.c directly (which requires PIO/DMA hardware stubs).
// Instead, DaReplica replicates the ping-pong buffer state machine so the tests
// verify the integration between sector_cache and the DMA delivery layer on the
// host without any hardware abstraction.
//
// FakeAkiko records which LBAs it "received" in delivery order, allowing
// verification of correct sequence, absence of stale data after an interrupt,
// and correct LBA tracking.
// =============================================================================

#include <CppUTest/TestHarness.h>
extern "C" {
#include "sector_cache.h"
}
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// inject_sector — write a test sector directly into a cache slot.
// Data pattern: buf[i] = (lba + i) & 0xFF for easy integrity checking.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// FakeAkiko — records LBAs received from the DMA engine in delivery order.
// ---------------------------------------------------------------------------
struct FakeAkiko {
    static const int MAX_SECTORS = 32;
    uint32_t lbas[MAX_SECTORS];
    int      count;

    void reset() { count = 0; }

    void receive(uint32_t lba) {
        if (count < MAX_SECTORS) lbas[count++] = lba;
    }

    bool contains(uint32_t lba) const {
        for (int i = 0; i < count; i++)
            if (lbas[i] == lba) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// DaReplica — replicates the da_output.c ping-pong DMA state machine.
//
// Layout mirrors da_start_play / _dma_irq_handler from da_output.c:
//   start()  ≈ da_start_play()
//   tick()   ≈ one firing of _dma_irq_handler()  (one sector delivered)
//   stop()   ≈ da_stop()
//
// expand() is a verbatim copy of expand_to_i2s24() in da_output.c.
// ---------------------------------------------------------------------------
static const int DA_WORDS = 588 * 2;   // uint32_t words per 2352-byte sector

struct DaReplica {
    uint32_t       buf[2][DA_WORDS];
    uint32_t       buf_lba[2];
    uint32_t       next_lba;
    bool           playing;
    int            active_buf;
    sector_cache_t *cache;
    FakeAkiko      *akiko;

    void _expand(const uint8_t *raw, uint32_t *out) {
        for (int i = 0; i < 588; i++) {
            uint16_t l = (uint16_t)(raw[i*4+0]) | ((uint16_t)(raw[i*4+1]) << 8);
            uint16_t r = (uint16_t)(raw[i*4+2]) | ((uint16_t)(raw[i*4+3]) << 8);
            out[i*2+0] = (uint32_t)l << 16;
            out[i*2+1] = (uint32_t)r << 16;
        }
    }

    bool start(sector_cache_t *c, uint32_t start_lba, FakeAkiko *a) {
        cache      = c;
        akiko      = a;
        next_lba   = start_lba;
        playing    = false;
        active_buf = 0;

        uint8_t  raw[SECTOR_RAW_SIZE];
        uint32_t bytes;

        // Pre-load buf[0] — failure aborts start (mirrors da_start_play)
        if (!sector_cache_get(cache, next_lba, raw, &bytes)) return false;
        _expand(raw, buf[0]);
        sector_cache_release_before(cache, next_lba);
        buf_lba[0] = next_lba++;

        // Pre-load buf[1] — silence-pad on cache miss (mirrors da_start_play)
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

    // Simulate one ISR firing: the active buffer has just finished streaming.
    // Akiko receives it; we refill the idle buffer with the next sector.
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
            playing = false;   // underrun / end of disc
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

// ---------------------------------------------------------------------------
// Test group
// ---------------------------------------------------------------------------
TEST_GROUP(AkikoDma)
{
    sector_cache_t cache;
    disc_image_t   fake_disc;
    FakeAkiko      akiko;
    DaReplica      da;

    void setup() {
        memset(&fake_disc, 0, sizeof(fake_disc));
        fake_disc.total_sectors = 100;
        sector_cache_init(&cache, &fake_disc);
        akiko.reset();
        memset(&da, 0, sizeof(da));
    }
    void teardown() {}
};

// ---------------------------------------------------------------------------
// PingPongSequence — six sectors delivered in strict LBA order.
// start() pre-loads buf[0]=LBA0 and buf[1]=LBA1; each tick() shifts the
// window forward by one, so ticks 1-6 deliver LBAs 0, 1, 2, 3, 4, 5.
// ---------------------------------------------------------------------------
TEST(AkikoDma, PingPongSequence)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));

    for (int i = 0; i < 6; i++) da.tick();

    LONGS_EQUAL(6, akiko.count);
    for (int i = 0; i < 6; i++)
        LONGS_EQUAL((uint32_t)i, akiko.lbas[i]);
}

// ---------------------------------------------------------------------------
// CacheMissAtStart — start() must fail gracefully when cache is empty.
// ---------------------------------------------------------------------------
TEST(AkikoDma, CacheMissAtStart)
{
    CHECK_FALSE(da.start(&cache, 0, &akiko));
    CHECK_FALSE(da.playing);
    LONGS_EQUAL(0, akiko.count);
}

// ---------------------------------------------------------------------------
// InterruptedMidTransfer — stop at sector 2, restart at sector 50.
// Sectors 2 and 3 must never appear after the restart.
// ---------------------------------------------------------------------------
TEST(AkikoDma, InterruptedMidTransfer)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    da.tick(); da.tick();           // deliver LBAs 0, 1
    LONGS_EQUAL(2, akiko.count);

    // Abort and seek
    da.stop();
    sector_cache_seek(&cache, 50);
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)(50 + i));

    akiko.reset();
    CHECK_TRUE(da.start(&cache, 50, &akiko));
    for (int i = 0; i < 4; i++) da.tick();

    LONGS_EQUAL(4, akiko.count);
    for (int i = 0; i < 4; i++)
        LONGS_EQUAL((uint32_t)(50 + i), akiko.lbas[i]);

    // No stale sectors from the aborted session
    CHECK_FALSE(akiko.contains(2));
    CHECK_FALSE(akiko.contains(3));
}

// ---------------------------------------------------------------------------
// CardYankMidPlay_ExactSectorsDelivered — inject 4 sectors (LBA 0-3).
// start(0) pre-loads 0+1; tick1 delivers 0 and reloads 2; tick2 delivers 1
// and reloads 3; tick3 delivers 2 and misses LBA 4 → playing stops.
// akiko must have received exactly LBAs [0, 1, 2] and never LBA 3.
// ---------------------------------------------------------------------------
TEST(AkikoDma, CardYankMidPlay_ExactSectorsDelivered)
{
    for (int i = 0; i < 4; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    da.tick(); da.tick(); da.tick();

    LONGS_EQUAL(3, akiko.count);
    LONGS_EQUAL(0u, akiko.lbas[0]);
    LONGS_EQUAL(1u, akiko.lbas[1]);
    LONGS_EQUAL(2u, akiko.lbas[2]);
    CHECK_FALSE(akiko.contains(3));
    CHECK_FALSE(da.playing);
}

// ---------------------------------------------------------------------------
// PostReset_BufsZeroedNextLbaReset — after start() + 2 ticks, stop() must
// zero both DMA buffers and reset next_lba to 0, matching the clean state
// expected after an Amiga hardware reset.
// ---------------------------------------------------------------------------
TEST(AkikoDma, PostReset_BufsZeroedNextLbaReset)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    da.tick(); da.tick();

    da.stop();

    CHECK_FALSE(da.playing);
    LONGS_EQUAL(0u, da.next_lba);
    for (int i = 0; i < DA_WORDS; i++) {
        LONGS_EQUAL(0u, da.buf[0][i]);
        LONGS_EQUAL(0u, da.buf[1][i]);
    }
}

// ---------------------------------------------------------------------------
// StaleNextLba — next_lba advances correctly during playback and resets on
// restart so the new session begins at exactly the requested LBA.
//
// After start(0): pre-loads 0+1, so next_lba = 2.
// After 2 ticks: each tick pulls one new sector, so next_lba = 4.
// After stop() + start(50): pre-loads 50+51, so next_lba = 52.
// ---------------------------------------------------------------------------
TEST(AkikoDma, StaleNextLba)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    LONGS_EQUAL(2u, da.next_lba);   // start pre-loaded two sectors

    da.tick(); da.tick();
    LONGS_EQUAL(4u, da.next_lba);   // two more sectors queued by ticks

    da.stop();
    sector_cache_seek(&cache, 50);
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)(50 + i));

    akiko.reset();
    CHECK_TRUE(da.start(&cache, 50, &akiko));
    LONGS_EQUAL(52u, da.next_lba);  // pre-loaded 50+51

    for (int i = 0; i < 4; i++) da.tick();
    // Sectors 2, 3 from the first session must not appear
    CHECK_FALSE(akiko.contains(2));
    CHECK_FALSE(akiko.contains(3));
}

// ---------------------------------------------------------------------------
// SilencePad_SingleSector_UInt32MaxNotDelivered — when start() can only
// pre-load one sector, buf[1] is zeroed and buf_lba[1] set to UINT32_MAX.
// The first tick delivers LBA 0 then misses LBA 1 → playing stops.
// UINT32_MAX must never be passed to akiko.receive().
// ---------------------------------------------------------------------------
TEST(AkikoDma, SilencePad_SingleSector_UInt32MaxNotDelivered)
{
    inject_sector(&cache, 0, 0u);   // only LBA 0 available

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    CHECK_TRUE(da.buf_lba[1] == UINT32_MAX);   // second buffer silence-padded

    da.tick();

    LONGS_EQUAL(1, akiko.count);
    LONGS_EQUAL(0u, akiko.lbas[0]);
    CHECK_FALSE(akiko.contains(UINT32_MAX));
    CHECK_FALSE(da.playing);
}

// ---------------------------------------------------------------------------
// ResetMidSession_SecondSessionByteDataClean — stop() mid-session (mirroring
// an Amiga /RESET assertion) then verify that the second session's DMA buffer
// contains byte content from the new LBA range, not stale data from the
// interrupted session.
//
// Session 1: LBA 0-7; inject_sector data[i] = (lba+i)&0xFF, so buf[0] after
// expand has left-word-0 = ((1<<8)|0)<<16 = 0x01000000.
// Session 2: LBA 50-57; buf[0] left-word-0 = ((51<<8)|50)<<16 = 0x33320000.
// ---------------------------------------------------------------------------
TEST(AkikoDma, ResetMidSession_SecondSessionByteDataClean)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));
    da.tick(); da.tick();

    da.stop();
    sector_cache_seek(&cache, 50);

    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)(50 + i));

    akiko.reset();
    CHECK_TRUE(da.start(&cache, 50, &akiko));

    // LBA 50 data[0..3] = {50,51,52,53} → l=(51<<8|50)=0x3332, r=(53<<8|52)=0x3534
    UNSIGNED_LONGS_EQUAL(0x33320000u, da.buf[0][0]);
    UNSIGNED_LONGS_EQUAL(0x35340000u, da.buf[0][1]);
    // Confirm these differ from session-1 LBA-0 pattern
    CHECK_TRUE(da.buf[0][0] != 0x01000000u);
}

// ---------------------------------------------------------------------------
// DataIntegrity_ExpandedFirstSample — verify the I2S word-packing end-to-end:
// inject_sector fills data[i] = (lba+i) & 0xFF; _expand shifts each 16-bit
// PCM sample left 16 bits into the upper half of a uint32_t.
//
// LBA 0: data[0..3] = {0,1,2,3}
//   left  word 0: l=0x0100 → 0x0100<<16 = 0x01000000
//   right word 0: r=0x0302 → 0x0302<<16 = 0x03020000
//
// LBA 1: data[0..3] = {1,2,3,4}
//   left  word 0: l=0x0201 → 0x02010000
// ---------------------------------------------------------------------------
TEST(AkikoDma, DataIntegrity_ExpandedFirstSample)
{
    for (int i = 0; i < SECTOR_BUFFER_COUNT; i++)
        inject_sector(&cache, i, (uint32_t)i);

    CHECK_TRUE(da.start(&cache, 0, &akiko));

    UNSIGNED_LONGS_EQUAL(0x01000000u, da.buf[0][0]);   // LBA 0, left  ch, sample 0
    UNSIGNED_LONGS_EQUAL(0x03020000u, da.buf[0][1]);   // LBA 0, right ch, sample 0
    UNSIGNED_LONGS_EQUAL(0x02010000u, da.buf[1][0]);   // LBA 1, left  ch, sample 0
}
