// =============================================================================
// test_subchannel_math.cpp — Q-channel subcode frame math tests
//
// Focuses on the relative-time arithmetic inside subcode_build_q_position():
//   - Index 00→01 transition: relative time resets to 00:00:00 at track start.
//   - Pregap countdown: relative time counts DOWN when index=0 and disc_lba is
//     before track_start_lba.
//   - Underflow guard: disc_lba == track_start_lba with index=0 must produce
//     0:0:0, not wrap to 4 294 967 295 frames.
//   - Absolute time: lba_to_msf() adds the 150-sector (2-second) lead-in offset
//     so disc_lba=0 maps to absolute MM:SS:FF = 00:02:00.
//
// Relevant production code: src/subcode.c, include/subcode.h
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "subcode.h"
#include "cd_types.h"
}

TEST_GROUP(SubchannelMath) {};

// ---------------------------------------------------------------------------
// Index01_AtTrackStart_RelTimeIsZero
// Relative MM:SS:FF must be 00:00:00 when disc_lba == track_start_lba and
// index=1 (programme area, not pregap).
// ---------------------------------------------------------------------------
TEST(SubchannelMath, Index01_AtTrackStart_RelTimeIsZero)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150u, 150u, buf);
    BYTES_EQUAL(0x00, buf[3]);   // relative MM
    BYTES_EQUAL(0x00, buf[4]);   // relative SS
    BYTES_EQUAL(0x00, buf[5]);   // relative FF
}

// ---------------------------------------------------------------------------
// Index00_OneBefore_CountdownIs1Frame
// One sector before the track start in pregap: relative time = 00:00:01 BCD.
// The countdown descends: track_start_lba - disc_lba = 1 frame.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, Index00_OneBefore_CountdownIs1Frame)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 0, false, 150u, 149u, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x00, buf[4]);
    BYTES_EQUAL(0x01, buf[5]);   // 1 frame remaining
}

// ---------------------------------------------------------------------------
// Index00_75Before_CountdownIs1Second
// 75 frames before track start = exactly 1 second of pregap.
// Relative SS=1, FF=0 in BCD.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, Index00_75Before_CountdownIs1Second)
{
    uint8_t buf[QCHANNEL_SIZE];
    // track starts at LBA 225; sector is at 150; delta = 75
    subcode_build_q_position(1, 0, false, 225u, 150u, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x01, buf[4]);   // 1 second
    BYTES_EQUAL(0x00, buf[5]);   // 0 frames
}

// ---------------------------------------------------------------------------
// Index01_OneSecAfterStart_RelTimeIs1Second
// 75 frames after track start = 1 second elapsed in programme area.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, Index01_OneSecAfterStart_RelTimeIs1Second)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150u, 225u, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x01, buf[4]);   // 1 second
    BYTES_EQUAL(0x00, buf[5]);
}

// ---------------------------------------------------------------------------
// PregapAtTrackStart_ZeroNoUnderflow
// index=0 but disc_lba == track_start_lba: the guard discards the countdown
// branch and the else path returns rel_lba=0, not a uint32_t wraparound.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, PregapAtTrackStart_ZeroNoUnderflow)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 0, false, 150u, 150u, buf);
    BYTES_EQUAL(0x00, buf[3]);
    BYTES_EQUAL(0x00, buf[4]);
    BYTES_EQUAL(0x00, buf[5]);   // 0, not 0xFF... wraparound
}

// ---------------------------------------------------------------------------
// AbsoluteTime_Lba0Has2SecOffset
// lba_to_msf(0) adds the 150-sector (2-second) Red Book lead-in offset.
// Absolute MM:SS:FF at disc_lba=0 must be 00:02:00 in BCD.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, AbsoluteTime_Lba0Has2SecOffset)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 0u, 0u, buf);
    BYTES_EQUAL(0x00, buf[7]);   // absolute MM = 00
    BYTES_EQUAL(0x02, buf[8]);   // absolute SS = 02
    BYTES_EQUAL(0x00, buf[9]);   // absolute FF = 00
}

// ---------------------------------------------------------------------------
// IndexBcd_FlipsAt00To01_AtTrackStart
// buf[2] carries the BCD-encoded index byte.  At the track boundary:
//   index=0 (pregap) → buf[2]=0x00
//   index=1 (programme area) → buf[2]=0x01
// ---------------------------------------------------------------------------
TEST(SubchannelMath, IndexBcd_FlipsAt00To01_AtTrackStart)
{
    uint8_t buf[QCHANNEL_SIZE];

    subcode_build_q_position(1, 0, false, 150u, 150u, buf);
    BYTES_EQUAL(0x00, buf[2]);   // index 0 → BCD 0x00

    subcode_build_q_position(1, 1, false, 150u, 150u, buf);
    BYTES_EQUAL(0x01, buf[2]);   // index 1 → BCD 0x01
}

// ---------------------------------------------------------------------------
// DataTrack_CtrlNibbleIsData — is_data=true sets CTRL=Q_CTRL_DATA (0x04).
// buf[0] = (CTRL<<4)|ADR = (0x04<<4)|0x01 = 0x41.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, DataTrack_CtrlNibbleIsData)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, true, 150u, 150u, buf);
    BYTES_EQUAL(0x41, buf[0]);
}

// ---------------------------------------------------------------------------
// AudioTrack_CtrlNibbleIsAudio — is_data=false sets CTRL=Q_CTRL_AUDIO (0x00).
// buf[0] = (0x00<<4)|0x01 = 0x01.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, AudioTrack_CtrlNibbleIsAudio)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150u, 150u, buf);
    BYTES_EQUAL(0x01, buf[0]);
}

// ---------------------------------------------------------------------------
// TrackNumberBcd_DoubleDigit — two-digit track numbers encode as packed BCD:
//   23 → 0x23,  99 → 0x99
// ---------------------------------------------------------------------------
TEST(SubchannelMath, TrackNumberBcd_DoubleDigit)
{
    uint8_t buf[QCHANNEL_SIZE];

    subcode_build_q_position(23, 1, false, 150u, 150u, buf);
    BYTES_EQUAL(0x23, buf[1]);

    subcode_build_q_position(99, 1, false, 150u, 150u, buf);
    BYTES_EQUAL(0x99, buf[1]);
}

// ---------------------------------------------------------------------------
// RelativeTime_OneMinute — 4 500 frames = 60 s = exactly 1 minute.
// BCD: MM=0x01, SS=0x00, FF=0x00.
// ---------------------------------------------------------------------------
TEST(SubchannelMath, RelativeTime_OneMinute)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150u, 150u + 4500u, buf);
    BYTES_EQUAL(0x01, buf[3]);   // 1 minute
    BYTES_EQUAL(0x00, buf[4]);   // 0 seconds
    BYTES_EQUAL(0x00, buf[5]);   // 0 frames
}

// ---------------------------------------------------------------------------
// CrcValidAfterBuild — subcode_build_q_position must call subcode_append_crc.
// The value stored in buf[10..11] must equal subcode_crc16(buf, 10).
// ---------------------------------------------------------------------------
TEST(SubchannelMath, CrcValidAfterBuild)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(1, 1, false, 150u, 225u, buf);
    uint16_t expected = subcode_crc16(buf, 10);
    uint16_t stored   = ((uint16_t)buf[10] << 8) | buf[11];
    LONGS_EQUAL(expected, stored);
}

// ---------------------------------------------------------------------------
// Scenario 3: Multiple indices within a track
//
// Red Book allows tracks to be subdivided with index points 00 (pregap),
// 01 (programme start), 02, 03 … up to 99.  These are used on classical CDs
// to mark movements within a single track.  The index is encoded as BCD in
// buf[2]; relative time is always measured from track_start_lba regardless
// of the current index.
// ---------------------------------------------------------------------------

// index=2 encodes as BCD 0x02 in buf[2].
TEST(SubchannelMath, MultiIndex_Index2_BcdIsCorrect)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(2, 2, false, 150u, 150u, buf);
    BYTES_EQUAL(0x02, buf[2]);
}

// index=10 encodes as BCD 0x10 (packed: tens digit 1, units digit 0).
TEST(SubchannelMath, MultiIndex_Index10_DoubleDigitBcd)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(2, 10, false, 150u, 150u, buf);
    BYTES_EQUAL(0x10, buf[2]);
}

// Even with index=2, relative time is measured from track_start_lba.
// Track 2 starts at LBA 150; 75 frames later (LBA 225) = 1 second elapsed.
TEST(SubchannelMath, MultiIndex_RelTimeFromIndex2)
{
    uint8_t buf[QCHANNEL_SIZE];
    subcode_build_q_position(2, 2, false, 150u, 225u, buf);
    BYTES_EQUAL(0x02, buf[2]);   // index 2 in BCD
    BYTES_EQUAL(0x00, buf[3]);   // 0 minutes
    BYTES_EQUAL(0x01, buf[4]);   // 1 second (75 frames elapsed)
    BYTES_EQUAL(0x00, buf[5]);   // 0 frames
}
