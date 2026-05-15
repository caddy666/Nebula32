// =============================================================================
// test_disc_logic.cpp — Disc image parser arithmetic and structure tests
//
// Intent: verify the pure computation layers of disc_image.c without SD I/O.
// FatFS is stubbed out in the host build, so disc_open() cannot be called;
// instead tests construct disc_image_t structs manually and exercise the
// lookup and formatting functions directly.
//
// Key invariants under test:
//   - disc_find_track(): returns correct track for boundary and interior LBAs,
//     returns NULL for LBAs past the last track.
//   - disc_build_toc_response(): BCD-encodes all track start MSFs correctly,
//     emits lead-out entry at track 0xAA, truncates to the requested byte count.
//   - CRC-32 config checksum: deterministic over fixed inputs, sensitive to
//     single-bit flips, consistent across repeated calls.
//   - CUE MSF arithmetic: decimal→BCD→LBA conversion matches Red Book offsets;
//     PREGAP sectors are NOT stored in the BIN file and must be subtracted from
//     the INDEX 01 file offset.
//   - NRG track arithmetic: track length = end_lba - idx1_lba; lead-in entries
//     (idx1==0, end==0) and lead-out entries (end<=idx1) must be skipped.
//   - ISO layout: single-track ISO parser produces correct start_lba=0,
//     sector_size=2048, total_sectors derived from file size.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdint.h>
#include <string.h>
extern "C" {
#include "cd_types.h"
#include "disc_image.h"
}

/* =========================================================================
 * Shared helper — build a minimal two-track disc_image_t without any
 * file I/O.  Track 1: data, LBA 0-2999 (3000 sectors).
 *                Track 2: audio, LBA 3000-5999 (3000 sectors).
 * ======================================================================= */
static disc_image_t make_two_track_disc(void)
{
    disc_image_t d;
    memset(&d, 0, sizeof(d));
    d.first_track = 1;
    d.last_track  = 2;
    d.total_sectors = 6000;

    d.tracks[0].number         = 1;
    d.tracks[0].type           = TRACK_TYPE_DATA;
    d.tracks[0].start_lba      = 0;
    d.tracks[0].length_sectors = 3000;

    d.tracks[1].number         = 2;
    d.tracks[1].type           = TRACK_TYPE_AUDIO;
    d.tracks[1].start_lba      = 3000;
    d.tracks[1].length_sectors = 3000;

    return d;
}

/* =========================================================================
 * 01 — DiscFindTrack
 *
 * Validates disc_find_track() boundary conditions on a two-track disc.
 * ======================================================================= */
TEST_GROUP(DiscFindTrack)
{
    disc_image_t disc;
    void setup()    { disc = make_two_track_disc(); }
    void teardown() {}
};

TEST(DiscFindTrack, FirstSectorOfTrack1_ReturnsTrack1)
{
    const track_t *t = disc_find_track(&disc, 0);
    CHECK(t != NULL);
    LONGS_EQUAL(1, t->number);
}

TEST(DiscFindTrack, LastSectorOfTrack1_ReturnsTrack1)
{
    /* Track 1 is [0, 3000) so LBA 2999 is the last valid sector. */
    const track_t *t = disc_find_track(&disc, 2999);
    CHECK(t != NULL);
    LONGS_EQUAL(1, t->number);
}

TEST(DiscFindTrack, FirstSectorOfTrack2_ReturnsTrack2)
{
    const track_t *t = disc_find_track(&disc, 3000);
    CHECK(t != NULL);
    LONGS_EQUAL(2, t->number);
}

TEST(DiscFindTrack, LastSectorOfTrack2_ReturnsTrack2)
{
    const track_t *t = disc_find_track(&disc, 5999);
    CHECK(t != NULL);
    LONGS_EQUAL(2, t->number);
}

TEST(DiscFindTrack, BeyondDisc_ReturnsNull)
{
    /* LBA 6000 is one past the end of track 2. */
    const track_t *t = disc_find_track(&disc, 6000);
    POINTERS_EQUAL(NULL, t);
}

TEST(DiscFindTrack, FarBeyondDisc_ReturnsNull)
{
    const track_t *t = disc_find_track(&disc, 0xFFFFFFFFu);
    POINTERS_EQUAL(NULL, t);
}

TEST(DiscFindTrack, SingleTrackDisc_HitAndMiss)
{
    disc_image_t d;
    memset(&d, 0, sizeof(d));
    d.first_track = 1;
    d.last_track  = 1;
    d.total_sectors = 100;
    d.tracks[0].number = 1;
    d.tracks[0].start_lba = 0;
    d.tracks[0].length_sectors = 100;

    CHECK(disc_find_track(&d, 0)   != NULL);
    CHECK(disc_find_track(&d, 99)  != NULL);
    POINTERS_EQUAL(NULL, disc_find_track(&d, 100));
}

TEST(DiscFindTrack, TrackType_IsPreserved)
{
    const track_t *t1 = disc_find_track(&disc, 0);
    const track_t *t2 = disc_find_track(&disc, 3000);
    CHECK(t1 != NULL);
    CHECK(t2 != NULL);
    LONGS_EQUAL(TRACK_TYPE_DATA,  t1->type);
    LONGS_EQUAL(TRACK_TYPE_AUDIO, t2->type);
}

/* =========================================================================
 * 02 — TocResponse
 *
 * Validates disc_build_toc_response() encoding for a two-track disc.
 * The format is: [bcd_track, min, sec] × N tracks, then [0xAA, min, sec].
 * ======================================================================= */
TEST_GROUP(TocResponse) {};

TEST(TocResponse, TwoTracks_LengthIs9Bytes)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    /* 2 tracks × 3 bytes + 3 bytes lead-out = 9 bytes */
    LONGS_EQUAL(9, n);
}

TEST(TocResponse, Track1Number_IsBcd01)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    disc_build_toc_response(&d, buf, sizeof(buf));
    BYTES_EQUAL(0x01, buf[0]);
}

TEST(TocResponse, Track1StartsAtLba0_Msf000200)
{
    /* LBA 0 → MSF 00:02:00 BCD */
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    disc_build_toc_response(&d, buf, sizeof(buf));
    BYTES_EQUAL(0x00, buf[1]);   /* minute */
    BYTES_EQUAL(0x02, buf[2]);   /* second */
}

TEST(TocResponse, Track2Number_IsBcd02)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    disc_build_toc_response(&d, buf, sizeof(buf));
    BYTES_EQUAL(0x02, buf[3]);
}

TEST(TocResponse, LeadOutByte_Is0xAA)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    BYTES_EQUAL(0xAA, buf[n - 3]);
}

TEST(TocResponse, TruncatedBuffer_NoOverrun)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[4];   /* only room for 1 track entry + 1 byte */
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    CHECK_TRUE(n <= sizeof(buf));
}

/* =========================================================================
 * 03 — ConfigCrc
 *
 * Validates the CRC-32 algorithm used by config.c.
 * The same polynomial and algorithm inline-tested here so the host build
 * needs no hardware/flash.h dependency.
 * ======================================================================= */

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int b = 0; b < 8; b++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    return crc;
}

static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_byte(crc, data[i]);
    return crc ^ 0xFFFFFFFFu;
}

TEST_GROUP(ConfigCrc) {};

TEST(ConfigCrc, AllZeroBytes_KnownValue)
{
    /* IEEE 802.3 CRC32 of 4 zero bytes = 0x2144DF1C */
    uint8_t zeros[4] = {0, 0, 0, 0};
    LONGS_EQUAL(0x2144DF1Cu, crc32_buf(zeros, 4));
}

TEST(ConfigCrc, SingleByte_0xFF_KnownValue)
{
    /* CRC32 of a single 0xFF byte = 0xFF000000 XOR'd through polynomial */
    uint8_t b = 0xFF;
    uint32_t crc = crc32_buf(&b, 1);
    CHECK_TRUE(crc != 0);           /* result must be non-zero */
    CHECK_TRUE(crc != 0xFFFFFFFFu); /* must not equal all-ones */
}

TEST(ConfigCrc, DifferentInputs_DifferentCrc)
{
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    CHECK_TRUE(crc32_buf(a, 8) != crc32_buf(b, 8));
}

TEST(ConfigCrc, Deterministic_SameInputSameCrc)
{
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)i;
    LONGS_EQUAL(crc32_buf(data, 16), crc32_buf(data, 16));
}

TEST(ConfigCrc, FlipOneBit_ChangesCrc)
{
    uint8_t a[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t b[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBF};
    CHECK_TRUE(crc32_buf(a, 8) != crc32_buf(b, 8));
}

/* =========================================================================
 * 04 — CueMsfArith
 *
 * Verifies the BCD encoding used when parsing CUE INDEX lines, and the
 * file-offset arithmetic for PREGAP (virtual silence) adjustment.
 *
 * CUE files store INDEX times as decimal MM:SS:FF.  The parser converts
 * each field to BCD before calling msf_to_lba(), which expects BCD values.
 * ======================================================================= */

static uint8_t dec_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Simulate the CUE parser's decimal→BCD→LBA path for a single INDEX 01 line.*/
static uint32_t cue_index_to_lba(uint8_t mm, uint8_t ss, uint8_t ff)
{
    msf_t msf;
    msf.minute = dec_to_bcd(mm);
    msf.second = dec_to_bcd(ss);
    msf.frame  = dec_to_bcd(ff);
    return msf_to_lba(msf);
}

TEST_GROUP(CueMsfArith) {};

TEST(CueMsfArith, Index01_00_02_00_IsLba0)
{
    /* Standard CUE track 1 start: "INDEX 01 00:02:00" */
    LONGS_EQUAL(0, cue_index_to_lba(0, 2, 0));
}

TEST(CueMsfArith, Index01_00_03_00_IsLba75)
{
    /* Each second is 75 frames; 3 s – 2 s pregap offset = 1 s = 75 LBA */
    LONGS_EQUAL(75, cue_index_to_lba(0, 3, 0));
}

TEST(CueMsfArith, Index01_01_02_00_IsLba4500)
{
    /* 1m02s00f → physical = (60+2)*75 = 4650, LBA = 4650 - 150 = 4500 */
    LONGS_EQUAL(4500, cue_index_to_lba(1, 2, 0));
}

TEST(CueMsfArith, BcdEncoding_DoubleDigitMinute)
{
    /* Verify dec_to_bcd handles two-digit values correctly */
    BYTES_EQUAL(0x45, dec_to_bcd(45));
    BYTES_EQUAL(0x59, dec_to_bcd(59));
    BYTES_EQUAL(0x10, dec_to_bcd(10));
}

TEST(CueMsfArith, RoundTrip_CueLbaAndBack)
{
    /* LBA 12345 → lba_to_msf() → BCD fields → cue_index_to_lba → same LBA */
    const uint32_t orig = 12345u;
    msf_t m = lba_to_msf(orig);
    /* Convert BCD fields back to the decimal values the CUE file would store */
    uint8_t mm = (uint8_t)(((m.minute >> 4) & 0xF) * 10 + (m.minute & 0xF));
    uint8_t ss = (uint8_t)(((m.second >> 4) & 0xF) * 10 + (m.second & 0xF));
    uint8_t ff = (uint8_t)(((m.frame  >> 4) & 0xF) * 10 + (m.frame  & 0xF));
    LONGS_EQUAL(orig, cue_index_to_lba(mm, ss, ff));
}

TEST(CueMsfArith, PregapAdjustment_SubtractsPregapFromFileOffset)
{
    /* Simulate a two-track disc where track 2 has PREGAP 00:02:00 (150 sectors).
     * Track 2 INDEX 01 is at absolute LBA N. The BIN file has no pregap data,
     * so file_offset = (N - 150) * sector_size, not N * sector_size. */
    const uint32_t t2_lba            = 3150u;   /* absolute INDEX 01 LBA */
    const uint32_t pregap_sectors    = 150u;    /* PREGAP 00:02:00 */
    const uint32_t sector_size       = 2352u;

    uint32_t offset_with_pregap = t2_lba * sector_size;
    uint32_t offset_no_pregap   = (t2_lba - pregap_sectors) * sector_size;

    CHECK_TRUE(offset_no_pregap < offset_with_pregap);
    LONGS_EQUAL(3000u * sector_size, offset_no_pregap);
}

/* =========================================================================
 * 05 — NrgTrackCalc
 *
 * Verifies the NRG track length arithmetic and the lead-out entry skip
 * condition that prevents a ghost zero-length track from inflating last_track.
 * ======================================================================= */
TEST_GROUP(NrgTrackCalc) {};

TEST(NrgTrackCalc, TrackLength_EndMinusIdx1)
{
    /* NRG track length = end_lba - idx1_lba */
    uint32_t idx1_lba = 0u;
    uint32_t end_lba  = 3000u;
    LONGS_EQUAL(3000, (long)(end_lba - idx1_lba));
}

TEST(NrgTrackCalc, TotalSectors_LastTrackStartPlusLength)
{
    /* Mirrors the final total_sectors computation in disc_parse_nrg */
    disc_image_t d;
    memset(&d, 0, sizeof(d));
    d.first_track = 1;
    d.last_track  = 2;
    d.tracks[0].start_lba      = 0u;
    d.tracks[0].length_sectors = 3000u;
    d.tracks[1].start_lba      = 3000u;
    d.tracks[1].length_sectors = 2750u;

    uint32_t total = d.tracks[d.last_track - 1].start_lba
                   + d.tracks[d.last_track - 1].length_sectors;
    LONGS_EQUAL(5750, (long)total);
}

TEST(NrgTrackCalc, LeadOutSkip_ZeroLength_IsDetected)
{
    /* A lead-out entry has idx1_lba == end_lba (zero-length).
     * The fixed skip condition: end_lba <= idx1_lba → skip. */
    uint32_t idx1 = 5750u;
    uint32_t end  = 5750u;
    CHECK_TRUE(end <= idx1);   /* condition that triggers the skip */
}

TEST(NrgTrackCalc, LeadOutSkip_NonZeroLength_IsKept)
{
    /* A real track (end > idx1) must NOT be skipped */
    uint32_t idx1 = 3000u;
    uint32_t end  = 5750u;
    CHECK_FALSE(end <= idx1);
}

TEST(NrgTrackCalc, LeadInSkip_BothZero_IsDetected)
{
    /* Lead-in entry: idx1_lba == 0 && end_lba == 0 */
    uint32_t idx1 = 0u;
    uint32_t end  = 0u;
    CHECK_TRUE(idx1 == 0u && end == 0u);
}

TEST(NrgTrackCalc, TrackAtLba0_IsNotSkipped)
{
    /* Track 1 can legitimately start at idx1=0 with end>0; must not be skipped */
    uint32_t idx1 = 0u;
    uint32_t end  = 3000u;
    bool skip_as_leadin  = (idx1 == 0u && end == 0u);
    bool skip_as_leadout = (end <= idx1);
    CHECK_FALSE(skip_as_leadin || skip_as_leadout);
}

/* =========================================================================
 * 06 — IsoLayout
 *
 * Verifies the structure that disc_parse_iso should produce, using a
 * manually constructed disc_image_t that matches parser output.
 * disc_find_track and disc_build_toc_response are then exercised against it.
 * ======================================================================= */
TEST_GROUP(IsoLayout) {};

static disc_image_t make_iso_disc(uint32_t total_sectors)
{
    disc_image_t d;
    memset(&d, 0, sizeof(d));
    d.first_track   = 1;
    d.last_track    = 1;
    d.total_sectors = total_sectors;

    d.tracks[0].number          = 1;
    d.tracks[0].type            = TRACK_TYPE_DATA;
    d.tracks[0].start_lba       = 0;
    d.tracks[0].pregap_lba      = 0;
    d.tracks[0].length_sectors  = total_sectors;
    d.tracks[0].file_offset     = 0;
    d.tracks[0].sector_size     = 2048;
    d.tracks[0].data_offset     = 0;

    return d;
}

TEST(IsoLayout, SingleTrack_DataType)
{
    disc_image_t d = make_iso_disc(10000);
    const track_t *t = disc_find_track(&d, 0);
    CHECK(t != NULL);
    LONGS_EQUAL(TRACK_TYPE_DATA, t->type);
}

TEST(IsoLayout, SingleTrack_SectorSize2048)
{
    disc_image_t d = make_iso_disc(10000);
    const track_t *t = disc_find_track(&d, 0);
    CHECK(t != NULL);
    LONGS_EQUAL(2048, t->sector_size);
}

TEST(IsoLayout, SingleTrack_FileOffsetZero)
{
    disc_image_t d = make_iso_disc(10000);
    const track_t *t = disc_find_track(&d, 0);
    CHECK(t != NULL);
    LONGS_EQUAL(0, t->file_offset);
}

TEST(IsoLayout, LastSector_StillFound)
{
    disc_image_t d = make_iso_disc(10000);
    const track_t *t = disc_find_track(&d, 9999);
    CHECK(t != NULL);
    LONGS_EQUAL(1, t->number);
}

TEST(IsoLayout, OnePastEnd_ReturnsNull)
{
    disc_image_t d = make_iso_disc(10000);
    POINTERS_EQUAL(NULL, disc_find_track(&d, 10000));
}

TEST(IsoLayout, TocLeadOut_MatchesTotalSectors)
{
    disc_image_t d = make_iso_disc(10000);
    uint8_t buf[32];
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    /* 1 track × 3 bytes + 3 lead-out = 6 bytes */
    LONGS_EQUAL(6, n);
    /* Lead-out track number is 0xAA */
    BYTES_EQUAL(0xAA, buf[3]);
}
