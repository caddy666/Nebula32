#include "test_runner.h"
#include "subcode.h"
#include <string.h>
#include <stdint.h>

// Verify the CRC stored in buf[10..11] is consistent with bytes 0-9.
static int crc_ok(const uint8_t *buf) {
    uint16_t computed = subcode_crc16(buf, 10);
    uint16_t stored   = ((uint16_t)buf[10] << 8) | buf[11];
    return computed == stored;
}

void test_subcode(void) {
    uint8_t buf[QCHANNEL_SIZE];

    // -------------------------------------------------------------------------
    SUITE("subcode: CRC-16/CCITT");

    // All-zeros input with init=0 and poly=0x1021: CRC = 0x0000, inverted = 0xFFFF
    uint8_t zeros[10] = {0};
    ASSERT_EQ(subcode_crc16(zeros, 10), 0xFFFFu,
              "crc16(10 x 0x00) = 0xFFFF");

    // Single-byte 0xFF: hand-traceable reference value
    // crc=0, byte=0xFF: crc ^= 0xFF00 = 0xFF00
    // After 8 bit iterations with poly 0x1021 the result is 0x1EF0, inverted 0xE10F
    uint8_t one_ff[1] = { 0xFF };
    ASSERT_EQ(subcode_crc16(one_ff, 1), 0xE10Fu,
              "crc16(0xFF) = 0xE10F");

    // -------------------------------------------------------------------------
    SUITE("subcode: Q-channel control/ADR byte");

    subcode_build_q_position(1, 1, /*is_data=*/true, 0, 0, buf);
    // Q_CTRL_DATA=0x04 << 4 | Q_ADR_POSITION=0x01 = 0x41
    ASSERT_EQ(buf[0], 0x41u, "data track ctrl/adr = 0x41");

    subcode_build_q_position(1, 1, /*is_data=*/false, 0, 0, buf);
    // Q_CTRL_AUDIO=0x00 << 4 | Q_ADR_POSITION=0x01 = 0x01
    ASSERT_EQ(buf[0], 0x01u, "audio track ctrl/adr = 0x01");

    // -------------------------------------------------------------------------
    SUITE("subcode: track and index BCD encoding");

    subcode_build_q_position(1, 1, false, 0, 0, buf);
    ASSERT_EQ(buf[1], 0x01u, "track  1 BCD = 0x01");
    ASSERT_EQ(buf[2], 0x01u, "index  1 BCD = 0x01");

    subcode_build_q_position(10, 0, false, 0, 0, buf);
    ASSERT_EQ(buf[1], 0x10u, "track 10 BCD = 0x10");
    ASSERT_EQ(buf[2], 0x00u, "index  0 BCD = 0x00  (pregap)");

    subcode_build_q_position(99, 1, false, 0, 0, buf);
    ASSERT_EQ(buf[1], 0x99u, "track 99 BCD = 0x99");

    // -------------------------------------------------------------------------
    SUITE("subcode: reserved byte 6 is always zero");

    subcode_build_q_position(1, 1, false, 0, 0, buf);
    ASSERT_EQ(buf[6], 0x00u, "byte 6 (reserved) = 0x00");

    // -------------------------------------------------------------------------
    SUITE("subcode: absolute time (correct — uses lba_to_msf with lead-in)");

    // Absolute disc address at LBA 0 = 00:02:00 (2-second lead-in)
    subcode_build_q_position(1, 1, true, 0, 0, buf);
    ASSERT_EQ(buf[7], 0x00u, "abs MM at LBA   0 = 0x00");
    ASSERT_EQ(buf[8], 0x02u, "abs SS at LBA   0 = 0x02  (lead-in offset)");
    ASSERT_EQ(buf[9], 0x00u, "abs FF at LBA   0 = 0x00");

    // LBA 75 = 1 second of content → absolute 00:03:00
    subcode_build_q_position(1, 1, true, 0, 75, buf);
    ASSERT_EQ(buf[7], 0x00u, "abs MM at LBA  75 = 0x00");
    ASSERT_EQ(buf[8], 0x03u, "abs SS at LBA  75 = 0x03");
    ASSERT_EQ(buf[9], 0x00u, "abs FF at LBA  75 = 0x00");

    // LBA 150 = 2 seconds of content → absolute 00:04:00
    subcode_build_q_position(1, 1, true, 0, 150, buf);
    ASSERT_EQ(buf[8], 0x04u, "abs SS at LBA 150 = 0x04");

    // -------------------------------------------------------------------------
    SUITE("subcode: CRC is self-consistent for each generated packet");

    subcode_build_q_position(1, 1, true, 0, 0, buf);
    ASSERT_TRUE(crc_ok(buf), "CRC valid for data track position packet");

    subcode_build_q_position(5, 1, false, 1000, 1075, buf);
    ASSERT_TRUE(crc_ok(buf), "CRC valid for audio track position packet");

    subcode_build_q_mcn("1234567890123", buf);
    ASSERT_TRUE(crc_ok(buf), "CRC valid for MCN packet");

    subcode_build_q_isrc(1, true, NULL, buf);
    ASSERT_TRUE(crc_ok(buf), "CRC valid for ISRC packet");

    // =========================================================================
    // FAILING TESTS
    // =========================================================================
    // Root cause: subcode_build_q_position() computes relative time with
    // lba_to_msf(rel_lba), but lba_to_msf() adds a 150-sector (2-second)
    // lead-in offset that applies only to absolute disc addresses.  Relative
    // time within a track has no lead-in offset.
    //
    // Fix needed in subcode.c: replace lba_to_msf(rel_lba) with a direct
    // sector-count-to-MSF conversion:
    //   frm = rel_lba % 75
    //   sec = (rel_lba / 75) % 60
    //   min = rel_lba / (75 * 60)
    //   BCD-encode each component
    // =========================================================================

    SUITE("subcode: relative time at track start [EXPECTED FAIL — lba_to_msf offset bug]");

    // disc_lba == track_lba: zero sectors elapsed → relative time 00:00:00
    // Bug: lba_to_msf(0) = BCD 00:02:00, so buf[4] gets 0x02 instead of 0x00
    subcode_build_q_position(1, 1, false, /*track_lba=*/0, /*disc_lba=*/0, buf);
    ASSERT_EQ(buf[3], 0x00u, "rel MM at sector 0 of track = 0x00");
    ASSERT_EQ(buf[4], 0x00u, "rel SS at sector 0 of track = 0x00  [BUG: gets 0x02]");
    ASSERT_EQ(buf[5], 0x00u, "rel FF at sector 0 of track = 0x00");

    SUITE("subcode: relative time 1 second in [EXPECTED FAIL — lba_to_msf offset bug]");

    // 75 sectors past track start = 1 second → relative 00:01:00
    // Bug: lba_to_msf(75) = total 225 → sec=3 → BCD 00:03:00
    subcode_build_q_position(1, 1, false, /*track_lba=*/0, /*disc_lba=*/75, buf);
    ASSERT_EQ(buf[3], 0x00u, "rel MM at 75 sectors = 0x00");
    ASSERT_EQ(buf[4], 0x01u, "rel SS at 75 sectors = 0x01  [BUG: gets 0x03]");
    ASSERT_EQ(buf[5], 0x00u, "rel FF at 75 sectors = 0x00");

    SUITE("subcode: pregap countdown [EXPECTED FAIL — lba_to_msf offset bug]");

    // 2 frames before track start: rel_lba=2 → countdown 00:00:02
    // Bug: lba_to_msf(2) = total 152 → sec=2,frm=2 → BCD 00:02:02
    subcode_build_q_position(1, 0, false, /*track_lba=*/150, /*disc_lba=*/148, buf);
    ASSERT_EQ(buf[3], 0x00u, "pregap rel MM = 0x00");
    ASSERT_EQ(buf[4], 0x00u, "pregap rel SS = 0x00  [BUG: gets 0x02]");
    ASSERT_EQ(buf[5], 0x02u, "pregap rel FF = 0x02  (2 frames remaining)");
}
