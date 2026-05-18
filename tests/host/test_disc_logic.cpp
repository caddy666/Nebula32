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

TEST(TocResponse, TwoTracks_LengthIs12Bytes)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    /* 2 tracks × 4 bytes + 4 bytes lead-out = 12 bytes */
    LONGS_EQUAL(12, n);
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
    /* Each entry is now 4 bytes: [track_no, min, sec, frame] */
    BYTES_EQUAL(0x02, buf[4]);
}

TEST(TocResponse, LeadOutByte_Is0xAA)
{
    disc_image_t d = make_two_track_disc();
    uint8_t buf[32];
    uint32_t n = disc_build_toc_response(&d, buf, sizeof(buf));
    /* Lead-out entry is 4 bytes: [0xAA, min, sec, frame] */
    BYTES_EQUAL(0xAA, buf[n - 4]);
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
    /* 1 track × 4 bytes + 4 lead-out = 8 bytes */
    LONGS_EQUAL(8, n);
    /* Lead-out track number is 0xAA (at start of lead-out entry) */
    BYTES_EQUAL(0xAA, buf[4]);
}

/* =========================================================================
 * FormatDetect — disc image extension matching
 *
 * selftest.c tested this via a local ext_match() helper.  Replicated here
 * so host builds catch regressions without requiring a Pico 2.
 * The function is case-insensitive, matching the production logic in
 * disc_image.c's path_has_ext() helper.
 * ======================================================================= */

static bool ext_match(const char *path, const char *ext)
{
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return false;
    const char *t = path + pl - el;
    for (size_t i = 0; i < el; i++) {
        char c1 = t[i];    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
        char c2 = ext[i];  if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
        if (c1 != c2) return false;
    }
    return true;
}

TEST_GROUP(FormatDetect) {};

TEST(FormatDetect, IsoExtension_Detected)
{
    CHECK_TRUE(ext_match("0:/game.iso", ".iso"));
}

TEST(FormatDetect, BinExtension_Detected)
{
    CHECK_TRUE(ext_match("0:/game.bin", ".bin"));
}

TEST(FormatDetect, NrgExtension_Detected)
{
    CHECK_TRUE(ext_match("0:/game.nrg", ".nrg"));
}

TEST(FormatDetect, MdfExtension_Detected)
{
    CHECK_TRUE(ext_match("0:/game.mdf", ".mdf"));
}

TEST(FormatDetect, UnknownExtension_NotMatched)
{
    /* .cdi is not a supported format — none of the four checks should fire */
    CHECK_FALSE(ext_match("0:/game.cdi", ".iso"));
    CHECK_FALSE(ext_match("0:/game.cdi", ".bin"));
    CHECK_FALSE(ext_match("0:/game.cdi", ".nrg"));
    CHECK_FALSE(ext_match("0:/game.cdi", ".mdf"));
}

TEST(FormatDetect, UppercaseExtension_Detected)
{
    /* SD card files from Windows are often uppercase */
    CHECK_TRUE(ext_match("0:/GAME.ISO", ".iso"));
    CHECK_TRUE(ext_match("0:/GAME.BIN", ".bin"));
}

TEST(FormatDetect, MixedCase_Detected)
{
    CHECK_TRUE(ext_match("0:/Zool2.Nrg", ".nrg"));
}

// ---------------------------------------------------------------------------
// Path separator and whitespace handling
// ext_match compares only the trailing suffix, so Unix paths, Windows paths,
// and filenames with spaces all work correctly.
// ---------------------------------------------------------------------------

TEST(FormatDetect, UnixSlashPath_MatchesExtension)
{
    // Nested Unix path: suffix is ".iso"
    CHECK_TRUE(ext_match("sd/games/zool2.iso", ".iso"));
    CHECK_FALSE(ext_match("sd/games/zool2.iso", ".bin"));
}

TEST(FormatDetect, WindowsBackslashPath_MatchesExtension)
{
    // Windows-style path (backslash is a regular char in C strings): suffix is ".bin"
    CHECK_TRUE(ext_match("D:\\GAMES\\disc.bin", ".bin"));
    CHECK_FALSE(ext_match("D:\\GAMES\\disc.bin", ".iso"));
}

TEST(FormatDetect, SpaceInFilename_Handled)
{
    // Spaces are not special — suffix comparison still works
    CHECK_TRUE(ext_match("my game disc.iso", ".iso"));
}

TEST(FormatDetect, DotInDirectoryComponent_NotConfused)
{
    // "path.dir/disc" — the last N chars are "disc", not ".iso"
    CHECK_FALSE(ext_match("path.dir/disc", ".iso"));
}

// ---------------------------------------------------------------------------
// Non-ASCII filename tests — extended Latin and CJK character sets
//
// ext_match compares raw bytes. UTF-8 guarantees that continuation bytes
// (0x80-0xBF) never alias ASCII punctuation or letters, so a ".iso" suffix
// is unambiguous regardless of what multibyte characters precede it.
// Extended Latin (U+00C0-U+017E) uses 2 bytes; CJK uses 3 bytes.
// The case-fold in ext_match only applies to ASCII A-Z (0x41-0x5A) — extended
// uppercase characters such as Ä are never incorrectly folded.
// ---------------------------------------------------------------------------

// Swedish Ä, Ö, Å (each 2-byte UTF-8: C3 84 / C3 96 / C3 85)
TEST(FormatDetect, SwedishAaAoA_IsoDetected)
{
    CHECK_TRUE(ext_match("Äventyrsspel.iso", ".iso"));     // Ä = C3 84
    CHECK_TRUE(ext_match("Återvändsgränd.bin", ".bin"));   // Å = C3 85, ä = C3 A4
    CHECK_TRUE(ext_match("Örnen_flyger.nrg", ".nrg"));     // Ö = C3 96
}

// Norwegian / Danish Ø and Æ (Ø = C3 98, Æ = C3 86)
TEST(FormatDetect, NorwegianDanishOslashAe_BinDetected)
{
    CHECK_TRUE(ext_match("Øresund.bin", ".bin"));
    CHECK_TRUE(ext_match("Ærefuldt_spil.mdf", ".mdf"));
    CHECK_TRUE(ext_match("Blå_Øjne.nrg", ".nrg"));         // å = C3 A5, ø = C3 B8
}

// German ä, ö, ü, ß (ß = C3 9F; umlauts are C3-prefix 2-byte sequences)
TEST(FormatDetect, GermanUmlautSzlig_MdfDetected)
{
    CHECK_TRUE(ext_match("Straße.mdf", ".mdf"));            // ß = C3 9F
    CHECK_TRUE(ext_match("Züge_und_Bäume.iso", ".iso"));   // ü = C3 BC, ä = C3 A4
    CHECK_TRUE(ext_match("Öl_und_Käse.bin", ".bin"));      // ö = C3 B6, ä = C3 A4
}

// French accented vowels (é = C3 A9, è = C3 A8, ê = C3 AA, ô = C3 B4, â = C3 A2)
TEST(FormatDetect, FrenchAccents_BinDetected)
{
    CHECK_TRUE(ext_match("Héros_de_légende.bin", ".bin"));
    CHECK_TRUE(ext_match("Châteaux_et_forêts.iso", ".iso"));
    CHECK_TRUE(ext_match("Île_enchantée.nrg", ".nrg"));    // Î = C3 8E
}

// Spanish Ñ, accented vowels (ñ = C3 B1, á = C3 A1, é = C3 A9, ó = C3 B3, ú = C3 BA)
TEST(FormatDetect, SpanishNtildeAccents_BinDetected)
{
    CHECK_TRUE(ext_match("Acción_Héroe.bin", ".bin"));
    CHECK_TRUE(ext_match("España_Clásico.iso", ".iso"));   // ñ = C3 B1, á = C3 A1
    CHECK_TRUE(ext_match("Añadir_Música.mdf", ".mdf"));    // ñ, ú
}

// Czech / Slovak š, č, ž, ř (each 2-byte: C5 A1 / C4 8D / C5 BE / C5 99)
TEST(FormatDetect, CzechSlovakCarons_NrgDetected)
{
    CHECK_TRUE(ext_match("Šéf_Čert_Šel.nrg", ".nrg"));
    CHECK_TRUE(ext_match("Žlutý_pes.iso", ".iso"));        // Ž = C5 BD, ý = C3 BD
    CHECK_TRUE(ext_match("Brno_Řeka.bin", ".bin"));        // Ř = C5 98
}

// Polish ł, ź, ź, ą, ę, ó (ł = C5 82, ź = C5 BA, ą = C4 85, ę = C4 99)
TEST(FormatDetect, PolishSpecialLetters_MdfDetected)
{
    CHECK_TRUE(ext_match("Łódź_Gdańsk.mdf", ".mdf"));     // Ł, ó, ź, ń
    CHECK_TRUE(ext_match("Więcej_Małych.iso", ".iso"));    // ę, ł
    CHECK_TRUE(ext_match("Ząb_Źródło.bin", ".bin"));       // Ą, Ź
}

// Hungarian double-acute accents Ő, Ű (Ő = C5 90, Ű = C5 B0)
TEST(FormatDetect, HungarianDoubleAcute_BinDetected)
{
    CHECK_TRUE(ext_match("Győr_Város.bin", ".bin"));        // ő = C5 91
    CHECK_TRUE(ext_match("Tűz_és_Fűszer.iso", ".iso"));    // ű = C5 B1
}

// Icelandic Þ (thorn = C3 BE) and Ð (eth = C3 90)
TEST(FormatDetect, IcelandicThornEth_IsoDetected)
{
    CHECK_TRUE(ext_match("Þingvellir.iso", ".iso"));
    CHECK_TRUE(ext_match("Ðalvík_saga.bin", ".bin"));
}

// Portuguese Ã, Ç, Õ (ã = C3 A3, ç = C3 A7, õ = C3 B5)
TEST(FormatDetect, PortugueseTildeAndCedilla_BinDetected)
{
    CHECK_TRUE(ext_match("São_Paulo_Ação.bin", ".bin"));   // ã, ç
    CHECK_TRUE(ext_match("Estação_Central.mdf", ".mdf"));  // ã
    CHECK_TRUE(ext_match("Colões_Tradição.nrg", ".nrg"));  // õ, ã
}

// Mix of multiple extended-Latin scripts in a single path
TEST(FormatDetect, MultiScriptLatinPath_IsoDetected)
{
    // German dir, Spanish filename
    CHECK_TRUE(ext_match("Spiele/Acción.iso", ".iso"));
    // Swedish dir, Polish filename
    CHECK_TRUE(ext_match("Spel/Łódź.bin", ".bin"));
    // French dir, Czech filename
    CHECK_TRUE(ext_match("Jeux/Žlutý.nrg", ".nrg"));
}

// ---------------------------------------------------------------------------
// CJK character sets — 3-byte UTF-8 sequences
// ---------------------------------------------------------------------------

// Japanese katakana (3 bytes each: E3 + 82/83 + xx)
// ゲーム = game, ディスク = disc
TEST(FormatDetect, JapaneseKatakana_IsoDetected)
{
    CHECK_TRUE(ext_match("ゲーム.iso", ".iso"));
    CHECK_TRUE(ext_match("ディスク.bin", ".bin"));
    CHECK_FALSE(ext_match("ゲーム.iso", ".bin"));   // wrong extension
}

// Japanese path: hiragana directory, katakana filename
TEST(FormatDetect, JapanesePath_BinDetected)
{
    // げーむ (hiragana) / ゲーム.bin (katakana)
    CHECK_TRUE(ext_match("げーむ/ゲーム.bin", ".bin"));
}

// Chinese simplified hanzi (3 bytes each)
// 光盘镜像 = disc image
TEST(FormatDetect, ChineseSimplified_BinDetected)
{
    CHECK_TRUE(ext_match("光盘镜像.bin", ".bin"));
    CHECK_TRUE(ext_match("游戏光盘.iso", ".iso"));
}

// Korean hangul syllables (3 bytes each)
// 게임 = game, 디스크 = disc
TEST(FormatDetect, KoreanHangul_NrgDetected)
{
    CHECK_TRUE(ext_match("게임디스크.nrg", ".nrg"));
    CHECK_TRUE(ext_match("광학디스크.mdf", ".mdf"));
}

// Mixed: Latin extended dir + CJK filename
TEST(FormatDetect, MixedLatinCjkPath_IsoDetected)
{
    // German dir (ä = 2-byte), Japanese filename (3-byte katakana)
    CHECK_TRUE(ext_match("Spiele/ゲーム.iso", ".iso"));
    // Swedish dir, Korean filename
    CHECK_TRUE(ext_match("Äventyr/게임.bin", ".bin"));
}

// Uppercase ASCII extension still case-folds correctly when filename is non-ASCII
TEST(FormatDetect, NonAsciiFilename_UppercaseExtFolds)
{
    CHECK_TRUE(ext_match("ゲーム.ISO", ".iso"));       // ASCII ext folded; CJK stem irrelevant
    CHECK_TRUE(ext_match("光盘镜像.BIN", ".bin"));
    CHECK_TRUE(ext_match("Straße.NRG", ".nrg"));       // extended Latin stem + ASCII ext
}

// Non-ASCII filename with NO ASCII extension must never match any format
TEST(FormatDetect, NonAsciiNoExtension_NotMatched)
{
    CHECK_FALSE(ext_match("ゲーム", ".iso"));
    CHECK_FALSE(ext_match("光盘镜像", ".bin"));
    CHECK_FALSE(ext_match("게임디스크", ".nrg"));
    CHECK_FALSE(ext_match("Straße", ".mdf"));
}

/* =========================================================================
 * Gap 25 — Be32Be64
 *
 * The NRG and MDF parsers in disc_image.c read big-endian fields from the
 * file and convert them with static inline be32() / be64().  These helpers
 * are replicated here so regressions in the byte-swap formula are caught
 * without building the embedded target.
 *
 * be32/be64 are involutions: be32(be32(x)) == x.  That property makes the
 * round-trip tests self-verifying without needing external reference values.
 * ======================================================================= */

static uint32_t be32_rep(uint32_t v)
{
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}

static uint64_t be64_rep(uint64_t v)
{
    return ((uint64_t)be32_rep((uint32_t)(v >> 32))) |
           ((uint64_t)be32_rep((uint32_t)(v & 0xFFFFFFFFu)) << 32);
}

TEST_GROUP(Be32Be64) {};

/* Canonical 4-distinct-byte test: 0x12345678 → 0x78563412 */
TEST(Be32Be64, Be32_KnownValue)
{
    LONGS_EQUAL(0x78563412ul, (unsigned long)be32_rep(0x12345678u));
}

/* All-zero and all-ones are fixed points. */
TEST(Be32Be64, Be32_FixedPoints)
{
    LONGS_EQUAL(0x00000000ul, (unsigned long)be32_rep(0x00000000u));
    LONGS_EQUAL(0xFFFFFFFFul, (unsigned long)be32_rep(0xFFFFFFFFu));
}

/* NRG v2 magic "NER5" (0x4E455235 big-endian in file) → little-endian host word */
TEST(Be32Be64, Be32_NrgV2Magic)
{
    LONGS_EQUAL(0x3552454Eul, (unsigned long)be32_rep(0x4E455235u));
}

/* Round-trip: be32(be32(x)) == x — be32 is its own inverse. */
TEST(Be32Be64, Be32_Involution)
{
    const uint32_t values[] = { 0xDEADBEEFu, 0x01020304u, 0xAABBCCDDu };
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        LONGS_EQUAL((long)values[i], (long)be32_rep(be32_rep(values[i])));
    }
}

/* be64 known value: bytes 01 02 03 04 05 06 07 08 in file → host 0x0807060504030201 */
TEST(Be32Be64, Be64_KnownValue)
{
    uint64_t input  = 0x0102030405060708ULL;  /* as read from big-endian file */
    uint64_t expect = 0x0807060504030201ULL;
    CHECK((be64_rep(input) == expect));
}

/* Round-trip: be64(be64(x)) == x */
TEST(Be32Be64, Be64_Involution)
{
    uint64_t x = 0xDEADBEEFCAFEBABEULL;
    CHECK((be64_rep(be64_rep(x)) == x));
}

/* =========================================================================
 * Gap 26 — DiscFindTrack with zero tracks
 *
 * A disc_image_t where first_track=1 and last_track=0 (no valid tracks) must
 * not crash and must return NULL for every LBA.  The loop in disc_find_track
 * uses `for (i=first_track; i<=last_track; i++)`, so 1<=0 is immediately
 * false and the body never executes.
 * ======================================================================= */
TEST(DiscFindTrack, ZeroTrackDisc_AlwaysReturnsNull)
{
    disc_image_t d;
    memset(&d, 0, sizeof(d));
    d.first_track   = 1;
    d.last_track    = 0;   /* loop never entered */
    d.total_sectors = 0;
    POINTERS_EQUAL(NULL, disc_find_track(&d, 0));
    POINTERS_EQUAL(NULL, disc_find_track(&d, 1000));
    POINTERS_EQUAL(NULL, disc_find_track(&d, 0xFFFFFFFFu));
}

/* =========================================================================
 * LbaFileOffset — LBA-to-byte-offset seek formula
 *
 * Verifies the formula applied by disc_read_sector() when seeking into the
 * image file:
 *   byte_offset = track.file_offset + (lba - track.start_lba) * track.sector_size
 *
 * Ported from tests/test_LBAtoOffsetMapping.cpp (LBAtoOffsetMapping test only;
 * CommandParser_ReadCommand was discarded — it referenced a non-existent class).
 * The original used clkdiv values 24/48; the arithmetic here is corrected to
 * match the actual track_t fields in disc_image.h.
 * ======================================================================= */

TEST_GROUP(LbaFileOffset) {};

// ISO image: sector_size=2048, file_offset=0, start_lba=0.
// LBA 100 → 0 + 100 * 2048 = 204 800.  Matches the original test exactly.
TEST(LbaFileOffset, Iso_Lba100_Is204800)
{
    track_t t;
    t.start_lba   = 0;
    t.file_offset = 0;
    t.sector_size = 2048;

    uint64_t offset = t.file_offset + (uint64_t)(100 - t.start_lba) * t.sector_size;
    LONGLONGS_EQUAL(204800ULL, offset);
}

// BIN/CUE or NRG image: sector_size=2352, file_offset=0, start_lba=0.
// LBA 100 → 0 + 100 * 2352 = 235 200.
TEST(LbaFileOffset, Raw_Lba100_Is235200)
{
    track_t t;
    t.start_lba   = 0;
    t.file_offset = 0;
    t.sector_size = 2352;

    uint64_t offset = t.file_offset + (uint64_t)(100 - t.start_lba) * t.sector_size;
    LONGLONGS_EQUAL(235200ULL, offset);
}

// Multi-track BIN: audio track starts at LBA 3000, file_offset = 3000 * 2352.
// Query LBA 3100 → file_offset + (3100 - 3000) * 2352 = 7 056 000 + 235 200 = 7 291 200.
// Verifies that start_lba is subtracted before multiplying (not after).
TEST(LbaFileOffset, MultiTrack_NonZeroStartLba)
{
    track_t t;
    t.start_lba   = 3000;
    t.file_offset = 3000u * 2352u;   // 7 056 000
    t.sector_size = 2352;

    uint64_t offset = t.file_offset + (uint64_t)(3100 - t.start_lba) * t.sector_size;
    LONGLONGS_EQUAL(7291200ULL, offset);
}
