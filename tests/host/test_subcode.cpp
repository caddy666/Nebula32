#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "subcode.h"
#include "cd_types.h"
}

TEST_GROUP(Subcode) {};

/* -------------------------------------------------------------------------
 * CRC-16/CCITT known values
 * The Q-channel CRC is computed over 10 bytes with init=0x0000 then
 * bitwise-inverted before storage.
 * ---------------------------------------------------------------------- */

/* All-zero 10-byte input.  CRC of all-zeros with init 0 = 0x0000, inverted = 0xFFFF. */
TEST(Subcode, Crc16AllZerosIsFFFF)
{
    uint8_t data[10] = {0};
    LONGS_EQUAL(0xFFFF, (long)subcode_crc16(data, 10));
}

/* Single 0xFF byte: pre-computed expected result. */
TEST(Subcode, Crc16SingleFF)
{
    uint8_t data[1] = { 0xFF };
    /* CRC-16/CCITT (init=0, no final invert) of 0xFF = 0x1EF0; inverted = 0xE10F */
    LONGS_EQUAL(0xE10F, (long)subcode_crc16(data, 1));
}

/* CRC must differ for different inputs. */
TEST(Subcode, Crc16DiffersForDifferentInputs)
{
    uint8_t a[10] = {0}, b[10] = {0};
    b[5] = 0x01;
    CHECK_TRUE(subcode_crc16(a, 10) != subcode_crc16(b, 10));
}

/* -------------------------------------------------------------------------
 * CONAD byte (byte 0) from subcode_build_q_position
 * ---------------------------------------------------------------------- */

/* Audio track: CTRL nibble 0x0, ADR nibble 0x1 → byte 0 = 0x01 */
TEST(Subcode, ConadByteAudioTrack)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x01, buf[0]);
}

/* Data track: CTRL nibble 0x4, ADR nibble 0x1 → byte 0 = 0x41 */
TEST(Subcode, ConadByteDataTrack)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, true, 0, 0, buf);
    BYTES_EQUAL(0x41, buf[0]);
}

/* -------------------------------------------------------------------------
 * BCD track encoding in byte 1
 * ---------------------------------------------------------------------- */

TEST(Subcode, BcdTrack1Is0x01)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x01, buf[1]);
}

TEST(Subcode, BcdTrack10Is0x10)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(10, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x10, buf[1]);
}

TEST(Subcode, BcdTrack99Is0x99)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(99, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x99, buf[1]);
}

/* -------------------------------------------------------------------------
 * Absolute time fields (bytes 7-9) at well-known LBAs
 * ---------------------------------------------------------------------- */

/* LBA 0 → absolute MSF 00:02:00 */
TEST(Subcode, AbsTimeAtLba0)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 0, buf);
    BYTES_EQUAL(0x00, buf[7]);  /* minute */
    BYTES_EQUAL(0x02, buf[8]);  /* second */
    BYTES_EQUAL(0x00, buf[9]);  /* frame  */
}

/* LBA 75 → absolute MSF 00:03:00 */
TEST(Subcode, AbsTimeAtLba75)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 75, buf);
    BYTES_EQUAL(0x00, buf[7]);
    BYTES_EQUAL(0x03, buf[8]);
    BYTES_EQUAL(0x00, buf[9]);
}

/* LBA 150 → absolute MSF 00:04:00 */
TEST(Subcode, AbsTimeAtLba150)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0, 150, buf);
    BYTES_EQUAL(0x00, buf[7]);
    BYTES_EQUAL(0x04, buf[8]);
    BYTES_EQUAL(0x00, buf[9]);
}

/* -------------------------------------------------------------------------
 * Reserved byte 6 must always be zero in Mode 1 (Position)
 * ---------------------------------------------------------------------- */

TEST(Subcode, ReservedByte6IsZero)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(5, 1, true, 100, 200, buf);
    BYTES_EQUAL(0x00, buf[6]);
}

/* -------------------------------------------------------------------------
 * CRC self-consistency for generated packets
 * ---------------------------------------------------------------------- */

/* Build a packet and verify that the CRC in bytes 10-11 matches
   a fresh recomputation over bytes 0-9 with final inversion. */
TEST(Subcode, CrcSelfConsistentPosition)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(3, 1, false, 0, 300, buf);

    /* The stored CRC was computed then inverted.  Re-invert before comparing. */
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    /* To verify: ~stored should equal the un-inverted CRC of bytes 0-9 */
    /* But subcode_crc16 already inverts, so compare directly: */
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

TEST(Subcode, CrcSelfConsistentMcn)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn("1234567890123", buf);
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

TEST(Subcode, CrcSelfConsistentIsrc)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_isrc(1, false, "GBAYE9200001", buf);
    uint16_t stored = ((uint16_t)buf[10] << 8) | buf[11];
    uint16_t recalc = subcode_crc16(buf, 10);
    LONGS_EQUAL((long)recalc, (long)stored);
}

/* -------------------------------------------------------------------------
 * subcode_append_crc writes correct bytes
 * ---------------------------------------------------------------------- */

TEST(Subcode, AppendCrcWritesCorrectBytes)
{
    uint8_t buf[QCHANNEL_SIZE];
    memset(buf, 0, sizeof(buf));
    /* Set some data in bytes 0-9 */
    buf[0] = 0x41; buf[1] = 0x01; buf[2] = 0x01;
    subcode_append_crc(buf);
    /* Verify stored bytes match recomputed CRC */
    uint16_t crc = subcode_crc16(buf, 10);
    BYTES_EQUAL((crc >> 8) & 0xFF, buf[10]);
    BYTES_EQUAL(crc & 0xFF, buf[11]);
}

/* -------------------------------------------------------------------------
 * MCN null pointer produces all-zero MCN bytes with valid CRC
 * ---------------------------------------------------------------------- */

TEST(Subcode, McnNullProducesZeroMcn)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_mcn(NULL, buf);
    /* ADR nibble should be Q_ADR_MCN = 0x02 */
    BYTES_EQUAL(Q_ADR_MCN, buf[0] & 0x0F);
    /* MCN bytes 1-7 should all be zero */
    for (int i = 1; i <= 7; i++) {
        BYTES_EQUAL(0x00, buf[i]);
    }
}
