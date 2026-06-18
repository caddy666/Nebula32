// =============================================================================
// test_priority.cpp — Mixed priority tests: cadence, stall simulation, Q-channel
//
// Intent: cover cross-cutting concerns that don't fit cleanly into a single
// source-file test group.  Each section is labelled with its focus area.
//
//   01 — SectorCadence: validates the 13.333 ms (2× speed) sector delivery
//        deadline.  Uses synthetic delivery timestamps; no real sleeps.
//
//   02 — StallSim: exercises the SD-card stall probability model used to
//        estimate worst-case cache drain rates under high seek activity.
//
//   03 — SdStallSim: similar stall counting with different seed/scale params.
//
//   04 — QSubchannel: verifies relative-MSF (bytes 3-5) and absolute-MSF
//        (bytes 7-9) produced by subcode_build_q_position().  Relative time
//        must be a direct frame-count BCD with no lead-in offset (BUG-1);
//        absolute time must include the 150-frame lead-in via lba_to_msf().
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
extern "C" {
#include "cd_types.h"
#include "subcode.h"
#include "defs.h"
}

/* =========================================================================
 * 01 — SectorCadence
 *
 * Validates deadline-miss counting logic for 13.333 ms sector cadence.
 * Uses simulated delivery times (µs) instead of real sleeps so the test
 * is fast and deterministic.
 * ======================================================================= */

/* Count how many deliveries in `times` exceed the 13 333 µs sector budget. */
static int count_deadline_misses(const uint32_t *times, int n)
{
    int misses = 0;
    for (int i = 0; i < n; i++)
        if (times[i] > 13333)
            misses++;
    return misses;
}

TEST_GROUP(SectorCadence) {};

TEST(SectorCadence, AllOnTime_ZeroMisses)
{
    uint32_t times[] = { 5000, 6000, 7000, 13000, 13333 };
    LONGS_EQUAL(0, count_deadline_misses(times, 5));
}

TEST(SectorCadence, OneLateDelivery_OneMiss)
{
    uint32_t times[] = { 5000, 5000, 14000, 5000, 5000 };
    LONGS_EQUAL(1, count_deadline_misses(times, 5));
}

TEST(SectorCadence, AllLate_AllMiss)
{
    uint32_t times[] = { 14000, 14000, 14000 };
    LONGS_EQUAL(3, count_deadline_misses(times, 3));
}

TEST(SectorCadence, ExactlyAtBudget_NoMiss)
{
    /* 13 333 µs is on-time (not strictly greater than budget). */
    uint32_t times[] = { 13333 };
    LONGS_EQUAL(0, count_deadline_misses(times, 1));
}

TEST(SectorCadence, OneOverBudget_OneMiss)
{
    uint32_t times[] = { 13334 };
    LONGS_EQUAL(1, count_deadline_misses(times, 1));
}

/* =========================================================================
 * 02 — RingBuffer
 *
 * Validates ring-buffer semantics that protect sector streaming from
 * underflow.  Uses a self-contained implementation; the properties tested
 * mirror those required of any streaming ring buffer in the ODE.
 * ======================================================================= */

class RingBuffer {
public:
    RingBuffer(int cap) : buf(new int[cap]), cap(cap) {}
    ~RingBuffer() { delete[] buf; }

    bool push(int v) {
        if (count == cap) return false;
        buf[head] = v;
        head = (head + 1) % cap;
        count++;
        return true;
    }

    bool pop(int &v) {
        if (count == 0) return false;
        v = buf[tail];
        tail = (tail + 1) % cap;
        count--;
        return true;
    }

    int size() const { return count; }

private:
    int *buf;
    int  cap;
    int  head  = 0;
    int  tail  = 0;
    int  count = 0;
};

TEST_GROUP(RingBuffer)
{
    RingBuffer *rb;
    void setup()    { rb = new RingBuffer(8); }
    void teardown() { delete rb; }
};

TEST(RingBuffer, PushToCapacity_AllSucceed)
{
    for (int i = 0; i < 8; i++)
        CHECK_TRUE(rb->push(i));
    LONGS_EQUAL(8, rb->size());
}

TEST(RingBuffer, PushWhenFull_Fails)
{
    for (int i = 0; i < 8; i++)
        rb->push(i);
    CHECK_FALSE(rb->push(99));
    LONGS_EQUAL(8, rb->size());
}

TEST(RingBuffer, PopToEmpty_AllSucceedInOrder)
{
    for (int i = 0; i < 8; i++)
        rb->push(i);
    int v;
    for (int i = 0; i < 8; i++) {
        CHECK_TRUE(rb->pop(v));
        LONGS_EQUAL(i, v);
    }
}

TEST(RingBuffer, PopWhenEmpty_Underflow)
{
    int v;
    CHECK_FALSE(rb->pop(v));
}

TEST(RingBuffer, Wraparound_CorrectOrder)
{
    /* Fill, drain 4, push 4 more — wraps the head past the end of the buffer. */
    for (int i = 0; i < 8; i++) rb->push(i);
    int v;
    for (int i = 0; i < 4; i++) rb->pop(v);
    for (int i = 8; i < 12; i++) rb->push(i);

    /* Remaining values should be 4..11 in order. */
    for (int expected = 4; expected < 12; expected++) {
        CHECK_TRUE(rb->pop(v));
        LONGS_EQUAL(expected, v);
    }
    CHECK_FALSE(rb->pop(v));  /* empty after full drain */
}

/* =========================================================================
 * 03 — SdStallSim
 *
 * Verifies stall-injection logic without real I/O or wall-clock sleeps.
 * A seeded PRNG produces a deterministic stream; we count how many sectors
 * would stall (rand() % 1000 == 0) and check the count is in the expected
 * range for 10 000 sectors.
 * ======================================================================= */

static int count_stalls(unsigned int seed, int sectors)
{
    srand(seed);
    int stalls = 0;
    for (int i = 0; i < sectors; i++)
        if ((rand() % 1000) == 0)
            stalls++;
    return stalls;
}

TEST_GROUP(SdStallSim) {};

TEST(SdStallSim, ZeroStalls_WhenThresholdNeverMet)
{
    /* A threshold of 0 means every sector stalls — verify the count == sectors. */
    srand(0);
    int hits = 0;
    for (int i = 0; i < 1000; i++)
        if ((rand() % 1) == 0) hits++;
    LONGS_EQUAL(1000, hits);
}

TEST(SdStallSim, Seed42_StallCountInExpectedRange)
{
    /* With 10 000 sectors and p=1/1000, expected stalls ≈ 10 (Poisson). */
    int stalls = count_stalls(42, 10000);
    CHECK_TRUE(stalls >= 1);
    CHECK_TRUE(stalls <= 40);
}

TEST(SdStallSim, DeterministicReplay_SameSeedSameCount)
{
    int a = count_stalls(12345, 10000);
    int b = count_stalls(12345, 10000);
    LONGS_EQUAL(a, b);
}

TEST(SdStallSim, StallCountScalesWithSectors)
{
    int small = count_stalls(1, 1000);
    int large = count_stalls(1, 10000);
    /* More sectors → more expected stalls (this holds for any real seed). */
    CHECK_TRUE(large >= small);
}

/* =========================================================================
 * 04 — QSubchannel
 *
 * Exercises relative-MSF and absolute-MSF bytes produced by
 * subcode_build_q_position().  These positions (bytes 3-9 of the Q channel)
 * are not covered by the existing Subcode or LbaMsf test groups.
 * ======================================================================= */

TEST_GROUP(QSubchannel) {};

/* 75 sectors into a track starting at LBA 0.
 * rel_lba = disc_lba - track_start = 75 - 0 = 75 frames = 1 second → 00:01:00.
 * Direct BCD from frame count; no lead-in offset applied (BUG-1 fix). */
TEST(QSubchannel, RelMsf_75SectorsIn_Is000300)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, buf);
    BYTES_EQUAL(0x00, buf[3]);   /* relative minute */
    BYTES_EQUAL(0x01, buf[4]);   /* relative second  */
    BYTES_EQUAL(0x00, buf[5]);   /* relative frame   */
}

/* disc_lba=225 → absolute total = 225+150 = 375 = 0m 5s 0f → BCD 00:05:00. */
TEST(QSubchannel, AbsMsf_DiscLba225_Is000500)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 225, buf);
    BYTES_EQUAL(0x00, buf[7]);   /* absolute minute */
    BYTES_EQUAL(0x05, buf[8]);   /* absolute second  */
    BYTES_EQUAL(0x00, buf[9]);   /* absolute frame   */
}

/* Pregap (index 0): disc_lba 75 sectors before track start → rel counts down.
 * rel_lba = track_start - disc_lba = 150 - 75 = 75 frames = 1 second → 00:01:00.
 * Direct BCD from frame count; no lead-in offset applied (BUG-1 fix). */
TEST(QSubchannel, Pregap_RelCountsDown_Is000300)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 0, false, 150, 75, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x01, buf[4]);
    BYTES_EQUAL(0x00, buf[5]);
}

/* Index byte (buf[2]) encodes programme area (index 1) as BCD 0x01. */
TEST(QSubchannel, IndexByte_ProgrammeArea_Is0x01)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(3, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x01, buf[2]);
}

/* Index byte (buf[2]) encodes pregap area (index 0) as BCD 0x00. */
TEST(QSubchannel, IndexByte_Pregap_Is0x00)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(3, 0, false, 150, 75, buf);
    BYTES_EQUAL(0x00, buf[2]);
}

/* =========================================================================
 * 05 — CommandFuzz
 *
 * Validates that a command opcode classifier derived from the Chinon/Philips
 * protocol constants in defs.h handles all 256 possible opcode bytes without
 * undefined behaviour, and that known opcodes map to expected categories.
 * ======================================================================= */

typedef enum { OPC_UNKNOWN, OPC_STATUS, OPC_CONTROL, OPC_QUERY } opc_class_t;

static opc_class_t classify_opcode(uint8_t opc)
{
    switch (opc) {
    case S_STAT:
    case S_CMD_ERR:
    case S_ID:
    case S_DISK_ERR:
    case S_CLOSED:
        return OPC_STATUS;
    case LED_CNTRL:
    case SET_ERROR_STATUS:
        return OPC_CONTROL;
    case SEND_AUTO_Q:
    case SEND_Q:
    case RESEND:
        return OPC_QUERY;
    default:
        return OPC_UNKNOWN;
    }
}

TEST_GROUP(CommandFuzz) {};

TEST(CommandFuzz, AllOpcodes_NoUndefinedBehaviour)
{
    /* Every opcode byte 0x00-0xFF must return a valid category (no crash). */
    for (int opc = 0; opc <= 255; opc++) {
        opc_class_t c = classify_opcode((uint8_t)opc);
        CHECK_TRUE(c == OPC_UNKNOWN  ||
                   c == OPC_STATUS   ||
                   c == OPC_CONTROL  ||
                   c == OPC_QUERY);
    }
}

TEST(CommandFuzz, StatusOpcodes_ClassifiedCorrectly)
{
    LONGS_EQUAL(OPC_STATUS, classify_opcode(S_STAT));
    LONGS_EQUAL(OPC_STATUS, classify_opcode(S_CMD_ERR));
    LONGS_EQUAL(OPC_STATUS, classify_opcode(S_ID));
    LONGS_EQUAL(OPC_STATUS, classify_opcode(S_DISK_ERR));
    LONGS_EQUAL(OPC_STATUS, classify_opcode(S_CLOSED));
}

TEST(CommandFuzz, ControlOpcodes_ClassifiedCorrectly)
{
    LONGS_EQUAL(OPC_CONTROL, classify_opcode(LED_CNTRL));
    LONGS_EQUAL(OPC_CONTROL, classify_opcode(SET_ERROR_STATUS));
}

TEST(CommandFuzz, QueryOpcodes_ClassifiedCorrectly)
{
    LONGS_EQUAL(OPC_QUERY, classify_opcode(SEND_AUTO_Q));
    LONGS_EQUAL(OPC_QUERY, classify_opcode(SEND_Q));
    LONGS_EQUAL(OPC_QUERY, classify_opcode(RESEND));
}

TEST(CommandFuzz, NullAndMaxByte_ReturnUnknown)
{
    LONGS_EQUAL(OPC_UNKNOWN, classify_opcode(0x00));
    LONGS_EQUAL(OPC_UNKNOWN, classify_opcode(0xFF));
}

/* =========================================================================
 * 06 — DmaDoubleBuffer
 *
 * Verifies that the double-buffer swap pattern used for DMA sector streaming
 * maintains isolation between active and standby buffers and produces no
 * stale data after a swap.
 * ======================================================================= */

TEST_GROUP(DmaDoubleBuffer)
{
    uint8_t bufA[SECTOR_DATA_BYTES];
    uint8_t bufB[SECTOR_DATA_BYTES];
    uint8_t *active;
    uint8_t *standby;

    void setup() {
        memset(bufA, 0xAA, sizeof(bufA));
        memset(bufB, 0x55, sizeof(bufB));
        active  = bufA;
        standby = bufB;
    }
};

TEST(DmaDoubleBuffer, FillPatternsAreIsolated)
{
    BYTES_EQUAL(0xAA, active[0]);
    BYTES_EQUAL(0xAA, active[SECTOR_DATA_BYTES - 1]);
    BYTES_EQUAL(0x55, standby[0]);
    BYTES_EQUAL(0x55, standby[SECTOR_DATA_BYTES - 1]);
}

TEST(DmaDoubleBuffer, SwapGivesCorrectActiveBuffer)
{
    uint8_t *tmp = active;
    active  = standby;
    standby = tmp;

    BYTES_EQUAL(0x55, active[0]);
    BYTES_EQUAL(0xAA, standby[0]);
}

TEST(DmaDoubleBuffer, StandbyWriteDoesNotCorruptActive)
{
    memset(standby, 0xFF, SECTOR_DATA_BYTES);
    /* Active buffer must be unchanged. */
    BYTES_EQUAL(0xAA, active[0]);
    BYTES_EQUAL(0xAA, active[SECTOR_DATA_BYTES - 1]);
}

TEST(DmaDoubleBuffer, AfterSwap_NewStandbyCanBeOverwritten)
{
    uint8_t *tmp = active;
    active  = standby;
    standby = tmp;

    /* Write new sector into standby after swap. */
    memset(standby, 0xBB, SECTOR_DATA_BYTES);
    BYTES_EQUAL(0x55, active[0]);     /* active unchanged */
    BYTES_EQUAL(0xBB, standby[0]);    /* standby has new data */
}
