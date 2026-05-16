// =============================================================================
// test_disc_parser.cpp — Malformed/corrupt input tests for disc image parsers
// =============================================================================
//
// Exercises disc_parse_iso(), disc_parse_bin(), disc_parse_nrg(), and
// disc_parse_mdf() with crafted byte sequences that represent common
// corruption patterns: empty files, wrong magic, integer-overflow inputs,
// and the four bugs fixed in src/disc_image.c:
//
//   FIX-1  NRG chunk_size=0      → infinite seek-to-same-position loop
//   FIX-2  NRG DAOX chunk_size<22 → uint32_t underflow in bytes_left
//   FIX-3  MDF sector_size=0     → divide-by-zero in track length calc
//   FIX-4  CUE pregap > INDEX 01  → uint32_t underflow in file_offset
//
// Build: make parser_tests
// Run:   ./parser_tests -v
//
// All parsers use FatFS I/O, so they cannot be compiled into the main
// cd32_tests binary (which uses the no-op inline stubs in tests/stubs/ff.h).
// This binary uses tests/host/ff.h + fatfs_sim.c which provide an injectable
// virtual file system.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdint.h>
#include <string.h>
extern "C" {
#include "disc_image.h"
#include "fatfs_sim.h"
}

// ---------------------------------------------------------------------------
// Binary builder helpers
// ---------------------------------------------------------------------------
// Write big-endian values into a byte buffer at a given offset.

static void put_be32(uint8_t *buf, int off, uint32_t v)
{
    buf[off+0] = (uint8_t)(v >> 24);
    buf[off+1] = (uint8_t)(v >> 16);
    buf[off+2] = (uint8_t)(v >>  8);
    buf[off+3] = (uint8_t)(v      );
}

static void put_be64(uint8_t *buf, int off, uint64_t v)
{
    put_be32(buf, off,   (uint32_t)(v >> 32));
    put_be32(buf, off+4, (uint32_t)(v      ));
}

static void put_le16(uint8_t *buf, int off, uint16_t v)
{
    buf[off+0] = (uint8_t)(v     );
    buf[off+1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *buf, int off, uint32_t v)
{
    buf[off+0] = (uint8_t)(v      );
    buf[off+1] = (uint8_t)(v >>  8);
    buf[off+2] = (uint8_t)(v >> 16);
    buf[off+3] = (uint8_t)(v >> 24);
}

// ---------------------------------------------------------------------------
// ISO tests
// ---------------------------------------------------------------------------

TEST_GROUP(ParseIso)
{
    disc_image_t disc;
    void setup()    { memset(&disc, 0, sizeof(disc)); fatfs_sim_reset(); }
    void teardown() { fatfs_sim_reset(); }
};

TEST(ParseIso, EmptyFile_ReturnsFalse)
{
    // size=0 → total_sectors=0 → parser rejects
    static const uint8_t empty[1] = {0};
    fatfs_sim_inject(&disc.image_file, empty, 0);
    CHECK_FALSE(disc_parse_iso(&disc));
}

TEST(ParseIso, ExactlyOneSector_ReturnsTrueWithOneSector)
{
    static uint8_t sector[2048];
    memset(sector, 0xAB, sizeof(sector));
    fatfs_sim_inject(&disc.image_file, sector, sizeof(sector));
    CHECK_TRUE(disc_parse_iso(&disc));
    LONGS_EQUAL(1, disc.total_sectors);
}

TEST(ParseIso, PartialSector_TruncatesToFloor)
{
    // 3000 bytes → 1 full sector (floor(3000/2048)=1)
    static uint8_t buf[3000];
    memset(buf, 0, sizeof(buf));
    fatfs_sim_inject(&disc.image_file, buf, sizeof(buf));
    CHECK_TRUE(disc_parse_iso(&disc));
    LONGS_EQUAL(1, disc.total_sectors);
}

TEST(ParseIso, MultiSector_CorrectCount)
{
    // 10 × 2048 = 20480 bytes
    static uint8_t buf[20480];
    memset(buf, 0, sizeof(buf));
    fatfs_sim_inject(&disc.image_file, buf, sizeof(buf));
    CHECK_TRUE(disc_parse_iso(&disc));
    LONGS_EQUAL(10, disc.total_sectors);
    LONGS_EQUAL(TRACK_TYPE_DATA, disc.tracks[0].type);
    LONGS_EQUAL(2048,            disc.tracks[0].sector_size);
}

// ---------------------------------------------------------------------------
// BIN/CUE tests
// ---------------------------------------------------------------------------

TEST_GROUP(ParseBin)
{
    disc_image_t disc;
    void setup()    { memset(&disc, 0, sizeof(disc)); fatfs_sim_reset(); }
    void teardown() { fatfs_sim_reset(); }
};

// No CUE registered → f_open fails → falls back to raw single-track parse.
TEST(ParseBin, NoCueFile_SingleTrackFallback)
{
    static uint8_t img[2352 * 5];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    // No sidecar registered — f_open(".cue") returns FR_NO_FILE.
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    LONGS_EQUAL(1, disc.first_track);
    LONGS_EQUAL(1, disc.last_track);
    LONGS_EQUAL(5, disc.total_sectors);
}

// CUE with track number 0 must be silently skipped (not crash).
TEST(ParseBin, CueTrackZero_Skipped)
{
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "  TRACK 0 MODE1/2352\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    // Track 0 skipped → first_track remains 0 (no valid track parsed)
    LONGS_EQUAL(0, disc.first_track);
}

// CUE with track number > MAX_TRACKS (99) must be silently skipped.
TEST(ParseBin, CueTrackOverMaxTracks_Skipped)
{
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "  TRACK 100 MODE1/2352\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    LONGS_EQUAL(0, disc.first_track);
}

// CUE with INDEX 01 before any TRACK line must not crash.
TEST(ParseBin, CueIndex01BeforeTrack_Ignored)
{
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "    INDEX 01 00:02:00\r\n"  // no TRACK line above this
        "  TRACK 1 MODE1/2352\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    // The stray INDEX 01 is ignored; track 1 is parsed normally.
    LONGS_EQUAL(1, disc.first_track);
}

// CUE with unknown mode string must default gracefully to MODE1/2352.
TEST(ParseBin, CueUnknownMode_DefaultsToMode1)
{
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "  TRACK 1 BOGUSMODE\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352 * 10];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    LONGS_EQUAL(SECTOR_RAW_BYTES, disc.tracks[0].sector_size);
}

// FIX-4: PREGAP larger than INDEX 01 LBA must not underflow file_offset.
TEST(ParseBin, CuePregapExceedsIndexLba_FileOffsetClampsToZero)
{
    // PREGAP 00:03:00 = 225 sectors; INDEX 01 00:02:00 = LBA 0.
    // 225 > 0 → underflow without the fix; with fix file_offset = 0.
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "  TRACK 1 MODE1/2352\r\n"
        "    PREGAP 00:03:00\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352 * 10];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    // file_offset must not be a huge wrapped value
    CHECK_TRUE(disc.tracks[0].file_offset < (uint32_t)(2352u * 10u));
}

// ---------------------------------------------------------------------------
// NRG tests
// ---------------------------------------------------------------------------
//
// NRG v2 file layout used by helper build_nrg_v2():
//   [0 .. image_bytes-1]  raw image data (zeroed)
//   [image_bytes ..]      chunk list:
//     for each chunk: 4-byte BE id, 4-byte BE size, <size> bytes data
//     END! sentinel:  id=0x454E4421, size=0
//   [last 12 bytes]       NRG v2 footer: magic "NER5" + BE64 offset to chunks

static const uint32_t NRG_MAGIC_V2  = 0x4E455235;  // "NER5"
static const uint32_t NRG_CHUNK_END_ID = 0x454E4421; // "END!"

// Build a minimal NRG v2 file in dst[].  Returns total file size.
// chunk_data/chunk_size: one DAOX chunk payload (may be NULL for no DAOX).
static size_t build_nrg_v2(uint8_t *dst, size_t dst_cap,
                            size_t image_bytes,
                            uint32_t chunk_id,
                            const uint8_t *chunk_data,
                            uint32_t chunk_data_size)
{
    memset(dst, 0, dst_cap);
    size_t off = image_bytes;

    if (chunk_data && chunk_data_size > 0) {
        // Chunk header
        put_be32(dst, (int)off, chunk_id);           off += 4;
        put_be32(dst, (int)off, chunk_data_size);    off += 4;
        memcpy(dst + off, chunk_data, chunk_data_size);
        off += chunk_data_size;
    }

    // END! chunk
    put_be32(dst, (int)off, NRG_CHUNK_END_ID); off += 4;
    put_be32(dst, (int)off, 0);                off += 4;

    // v2 footer (12 bytes)
    uint64_t chunks_start = (uint64_t)image_bytes;
    put_be32(dst, (int)off, NRG_MAGIC_V2);     off += 4;
    put_be64(dst, (int)off, chunks_start);     off += 8;

    return off;
}

TEST_GROUP(ParseNrg)
{
    disc_image_t disc;
    void setup()    { memset(&disc, 0, sizeof(disc)); fatfs_sim_reset(); }
    void teardown() { fatfs_sim_reset(); }
};

TEST(ParseNrg, FileTooSmall_ReturnsFalse)
{
    static const uint8_t tiny[4] = {0x01, 0x02, 0x03, 0x04};
    fatfs_sim_inject(&disc.image_file, tiny, sizeof(tiny));
    CHECK_FALSE(disc_parse_nrg(&disc));
}

TEST(ParseNrg, AllZeros_NoValidMagic_ReturnsFalse)
{
    static uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    fatfs_sim_inject(&disc.image_file, buf, sizeof(buf));
    CHECK_FALSE(disc_parse_nrg(&disc));
}

// FIX-1: chunk_size=0 must not produce an infinite loop.
TEST(ParseNrg, ChunkSizeZero_DoesNotLoop)
{
    // Build a v2 file where chunk list starts with a non-END chunk of size 0.
    // Without FIX-1 this would hang; with the fix it breaks out of the loop.
    static uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    size_t off = 0;
    // One SINF chunk with size=0 (triggers the bug)
    put_be32(buf, (int)off, 0x53494E46); off += 4;  // "SINF"
    put_be32(buf, (int)off, 0);          off += 4;  // size=0 ← malformed
    // END! chunk
    put_be32(buf, (int)off, NRG_CHUNK_END_ID); off += 4;
    put_be32(buf, (int)off, 0);                off += 4;
    // Footer: chunks start at offset 0 (the SINF chunk itself)
    put_be32(buf, (int)off, NRG_MAGIC_V2);     off += 4;
    put_be64(buf, (int)off, 0ULL);             off += 8;

    fatfs_sim_inject(&disc.image_file, buf, (uint32_t)off);
    // Must return without hanging; result is false (no tracks)
    CHECK_FALSE(disc_parse_nrg(&disc));
}

// FIX-2: DAOX chunk with chunk_size < 22 must not underflow bytes_left.
TEST(ParseNrg, DaoxChunkTooShort_NoUnderflow)
{
    // Build a DAOX chunk with size=10 (< 22 bytes session header).
    // Without FIX-2 bytes_left = 10 - 22 wraps to ~4 billion → huge loop.
    static uint8_t chunk_data[10];
    memset(chunk_data, 0, sizeof(chunk_data));

    static uint8_t buf[256];
    size_t sz = build_nrg_v2(buf, sizeof(buf), 0,
                              0x44414F58 /* DAOX */, chunk_data, 10);
    fatfs_sim_inject(&disc.image_file, buf, (uint32_t)sz);
    CHECK_FALSE(disc_parse_nrg(&disc));  // no valid tracks → false; must not loop
}

// A track entry where end_lba <= idx1_lba must be skipped.
TEST(ParseNrg, TrackEntryLeadOut_Skipped)
{
    // Build a DAOX chunk with one track entry where idx1=5000, end=5000
    // (zero-length, the lead-out skip condition).
    static uint8_t entry[42];
    memset(entry, 0, sizeof(entry));
    // sector_size field (bytes 12-13): 0x0920 = 2352
    entry[12] = 0x09; entry[13] = 0x20;
    // idx1_lba (bytes 20-23): 5000
    put_be32(entry, 20, 5000);
    // end_lba  (bytes 24-27): 5000 — triggers skip (end <= idx1)
    put_be32(entry, 24, 5000);

    // DAOX chunk = 22-byte session header + 42-byte entry = 64 bytes
    static uint8_t daox[64];
    memset(daox, 0, sizeof(daox));
    memcpy(daox + 22, entry, 42);

    static uint8_t buf[256];
    size_t sz = build_nrg_v2(buf, sizeof(buf), 0,
                              0x44414F58 /* DAOX */, daox, 64);
    fatfs_sim_inject(&disc.image_file, buf, (uint32_t)sz);
    CHECK_FALSE(disc_parse_nrg(&disc));  // skipped → 0 tracks
}

// A valid DAOX entry (idx1=0, end=3000) must be accepted.
TEST(ParseNrg, ValidTrackEntry_Parsed)
{
    static uint8_t entry[42];
    memset(entry, 0, sizeof(entry));
    entry[12] = 0x09; entry[13] = 0x20;  // sector_size = 2352
    entry[14] = 0x00;                     // data track
    put_be32(entry, 20, 0);               // idx1_lba = 0
    put_be32(entry, 24, 3000);            // end_lba  = 3000

    static uint8_t daox[64];
    memset(daox, 0, sizeof(daox));
    memcpy(daox + 22, entry, 42);

    // Image data: 3000 × 2352 = 7 056 000 bytes
    static uint8_t image_data[2352];  // just enough for the file-size calculation
    memset(image_data, 0, sizeof(image_data));

    static uint8_t buf[300];
    size_t sz = build_nrg_v2(buf, sizeof(buf), 0,
                              0x44414F58, daox, 64);
    fatfs_sim_inject(&disc.image_file, buf, (uint32_t)sz);
    CHECK_TRUE(disc_parse_nrg(&disc));
    LONGS_EQUAL(1,    disc.last_track);
    LONGS_EQUAL(3000, disc.tracks[0].length_sectors);
}

// ---------------------------------------------------------------------------
// MDF/MDS tests
// ---------------------------------------------------------------------------
//
// mds_header_t is 84 bytes (packed).  We build the minimum valid header
// followed by a session block and one track block.

static const char MDS_SIG[17] = "MEDIA DESCRIPTOR";

TEST_GROUP(ParseMdf)
{
    disc_image_t disc;
    void setup()    { memset(&disc, 0, sizeof(disc)); fatfs_sim_reset(); }
    void teardown() { fatfs_sim_reset(); }
};

TEST(ParseMdf, WrongSignature_ReturnsFalse)
{
    static uint8_t mds[88];
    memset(mds, 0, sizeof(mds));
    memcpy(mds, "WRONG SIGNATURE!", 16);
    fatfs_sim_register_sidecar(".mds", mds, sizeof(mds));
    fatfs_sim_inject(&disc.image_file, mds, 0);
    CHECK_FALSE(disc_parse_mdf(&disc, "game.mds"));
}

TEST(ParseMdf, ShortHeader_ReturnsFalse)
{
    // Only 8 bytes — too short to read mds_header_t (88 bytes)
    static uint8_t mds[8];
    memcpy(mds, MDS_SIG, 8);
    fatfs_sim_register_sidecar(".mds", mds, sizeof(mds));
    fatfs_sim_inject(&disc.image_file, mds, 0);
    CHECK_FALSE(disc_parse_mdf(&disc, "game.mds"));
}

TEST(ParseMdf, ZeroSessions_ReturnsFalse)
{
    // Valid signature, session_count=0 → no tracks → returns false.
    // mds_header_t layout (packed, all fields little-endian):
    //   [0..15]   signature
    //   [16..17]  version[2]
    //   [18..19]  medium_type
    //   [20..21]  session_count   ← set to 0
    //   rest zeroed
    static uint8_t mds[88];
    memset(mds, 0, sizeof(mds));
    memcpy(mds, MDS_SIG, 16);
    // session_count at offset 20, LE16 = 0 (already zeroed)
    fatfs_sim_register_sidecar(".mds", mds, sizeof(mds));
    fatfs_sim_inject(&disc.image_file, mds, 0);
    CHECK_FALSE(disc_parse_mdf(&disc, "game.mds"));
}

// FIX-3: A track block with sector_size=0 must not divide by zero.
TEST(ParseMdf, TrackSectorSizeZero_NoDiv0)
{
    // Build a minimal MDS with one session containing one data track.
    // Track block has sector_size=0 to trigger the bug.
    //
    // Offsets confirmed by sizeof/offsetof on the packed structs:
    //   mds_header_t         = 88 bytes; sessions_blocks_offset at byte 80
    //   mds_session_block_t  = 24 bytes; tracks_in_session at byte 10
    //   mds_track_block_t    = 79 bytes; sector_size at byte 18, start_sector at byte 38
    //
    // Layout: [header][session_block][track_block]
    // sessions_blocks_offset = 88 (immediately after header)
    // tracks_blocks_offset   = 88 + 24 = 112

    static const uint32_t HDR_SIZE  = 88;
    static const uint32_t SESS_SIZE = 24;  // sizeof(mds_session_block_t)
    static const uint32_t TBLK_SIZE = 79;  // sizeof(mds_track_block_t)

    static uint8_t mds[300];
    memset(mds, 0, sizeof(mds));

    // Header
    memcpy(mds, MDS_SIG, 16);
    put_le16(mds, 20, 1);               // session_count = 1
    put_le32(mds, 80, HDR_SIZE);        // sessions_blocks_offset (at byte 80)

    // Session block at offset 88
    uint32_t sess_off = HDR_SIZE;
    mds[sess_off + 10] = 1;             // tracks_in_session = 1
    mds[sess_off + 11] = 1;             // tracks_in_session2 = 1
    put_le32(mds, (int)(sess_off + 20), HDR_SIZE + SESS_SIZE); // tracks_blocks_offset

    // Track block at offset 112
    uint32_t tblk_off = HDR_SIZE + SESS_SIZE;
    mds[tblk_off + 5] = 0x01;           // track_number = 1 (not lead-in/lead-out)
    mds[tblk_off + 4] = 0x04;           // adr_ctl: CTL bits 3:2=01 = data
    // sector_size at byte 18 (LE16): leave as 0 ← the malformed value
    put_le32(mds, (int)(tblk_off + 38), 0); // start_sector = 0 (at byte 38)
    // start_offset (8 bytes at byte 42): 0

    uint32_t mds_size = tblk_off + TBLK_SIZE;
    fatfs_sim_register_sidecar(".mds", mds, mds_size);

    // image_file with some size so division produces a finite result
    static uint8_t img[2352 * 10];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));

    // Must not crash or divide by zero; may return true or false
    bool result = disc_parse_mdf(&disc, "game.mds");
    (void)result;
    // If sector_size was fixed to SECTOR_RAW_BYTES (2352), length is computable
    CHECK_TRUE(disc.tracks[0].sector_size != 0);
}

// Lead-in and lead-out track numbers must be filtered.
TEST(ParseMdf, LeadInLeadOutTrackNumbers_Skipped)
{
    static const uint32_t HDR_SIZE  = 88;
    static const uint32_t SESS_SIZE = 24;
    static const uint32_t TBLK_SIZE = 79;

    static uint8_t mds[300];
    memset(mds, 0, sizeof(mds));
    memcpy(mds, MDS_SIG, 16);
    put_le16(mds, 20, 1);
    put_le32(mds, 80, HDR_SIZE);        // sessions_blocks_offset at byte 80

    uint32_t sess_off = HDR_SIZE;
    mds[sess_off + 10] = 3;   // tracks_in_session = 3 (at byte 10)
    mds[sess_off + 11] = 3;   // tracks_in_session2 = 3 (at byte 11)
    put_le32(mds, (int)(sess_off + 20), HDR_SIZE + SESS_SIZE);

    uint32_t tblk_off = HDR_SIZE + SESS_SIZE;
    // Block 0: track_number = 0xA0 (lead-in type)
    mds[tblk_off + 5] = 0xA0;
    // Block 1: track_number = 0xA2 (another lead-in variant)
    mds[tblk_off + TBLK_SIZE + 5] = 0xA2;
    // Block 2: track_number = 0xAA (lead-out)
    mds[tblk_off + TBLK_SIZE * 2 + 5] = 0xAA;

    uint32_t mds_size = tblk_off + TBLK_SIZE * 3;
    fatfs_sim_register_sidecar(".mds", mds, mds_size);
    fatfs_sim_inject(&disc.image_file, mds, 0);

    CHECK_FALSE(disc_parse_mdf(&disc, "game.mds"));  // all three skipped → 0 tracks
}

// ---------------------------------------------------------------------------
// ParseBin — additional boundary tests
// ---------------------------------------------------------------------------

// CUE where Track 2 has a lower INDEX 01 LBA than Track 1 (backwards / malformed).
// Without the underflow guard, track 1's length would wrap to ~4 billion.
// With the guard it is clamped to 0 and the parser still returns true.
TEST(ParseBin, CueOverlappingTracks_LengthClamped)
{
    // 00:04:00 → LBA 300; 00:02:00 → LBA 150. Track 2 starts before Track 1.
    static const char cue[] =
        "FILE \"game.bin\" BINARY\r\n"
        "  TRACK 1 MODE1/2352\r\n"
        "    INDEX 01 00:04:00\r\n"
        "  TRACK 2 MODE1/2352\r\n"
        "    INDEX 01 00:02:00\r\n";
    static uint8_t img[2352 * 300];
    memset(img, 0, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    fatfs_sim_register_sidecar(".cue", (const uint8_t *)cue, (uint32_t)strlen(cue));
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));
    // Track 1 length must not underflow to a huge value
    CHECK_TRUE(disc.tracks[0].length_sectors < 0x80000000u);
}

// ---------------------------------------------------------------------------
// ParseNrg — astronomical end_lba
// ---------------------------------------------------------------------------

// NRG track with end_lba = 0xFFFFFFFF should not crash disc_find_track.
// The length is astronomical but the parser stores it and read requests
// for out-of-file LBAs just get 0 bytes back from the FatFS sim.
TEST(ParseNrg, AstronomicalEndLba_DoesNotCrash)
{
    static uint8_t entry[42];
    memset(entry, 0, sizeof(entry));
    entry[12] = 0x09; entry[13] = 0x20;   // sector_size = 2352
    put_be32(entry, 20, 0);               // idx1_lba = 0
    put_be32(entry, 24, 0xFFFFFFFFu);     // end_lba  = max uint32

    static uint8_t daox[64];
    memset(daox, 0, sizeof(daox));
    memcpy(daox + 22, entry, 42);

    static uint8_t buf[300];
    size_t sz = build_nrg_v2(buf, sizeof(buf), 0, 0x44414F58, daox, 64);
    fatfs_sim_inject(&disc.image_file, buf, (uint32_t)sz);
    CHECK_TRUE(disc_parse_nrg(&disc));
    // length = 0xFFFFFFFF - 0 = 0xFFFFFFFF, stored as-is; must not crash
    LONGS_EQUAL(0, disc.tracks[0].start_lba);
}

// ---------------------------------------------------------------------------
// SectorAccess — disc_read_sector boundary tests
// ---------------------------------------------------------------------------

TEST_GROUP(SectorAccess)
{
    disc_image_t disc;
    uint8_t buf[SECTOR_RAW_BYTES];
    void setup()
    {
        memset(&disc, 0, sizeof(disc));
        memset(buf, 0xAA, sizeof(buf));
        fatfs_sim_reset();
    }
    void teardown() { fatfs_sim_reset(); }
};

// Reading LBA 0 on a 5-sector ISO image must return SECTOR_RAW_BYTES bytes.
TEST(SectorAccess, ReadLba0_ISO_ReturnsRawSector)
{
    static uint8_t img[2048 * 5];
    memset(img, 0x5A, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    disc.file_open = true;
    CHECK_TRUE(disc_parse_iso(&disc));
    uint32_t n = disc_read_sector(&disc, 0, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
}

// Reading LBA == total_sectors (one past the end) must return 0 and zero the buffer.
TEST(SectorAccess, ReadPastEnd_ReturnsZero)
{
    static uint8_t img[2048 * 5];
    memset(img, 0x5A, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    disc.file_open = true;
    CHECK_TRUE(disc_parse_iso(&disc));
    LONGS_EQUAL(5, disc.total_sectors);
    uint32_t n = disc_read_sector(&disc, disc.total_sectors, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(0, n);
    // buf must have been zeroed
    for (int i = 0; i < SECTOR_RAW_BYTES; i++)
        CHECK_EQUAL(0, buf[i]);
}

// Reading from a raw 2352-byte BIN at LBA 0 must return SECTOR_RAW_BYTES bytes.
TEST(SectorAccess, ReadLba0_RawBin_ReturnsRawSector)
{
    // Use NoCueFile path: single 2352-byte track
    static uint8_t img[2352 * 3];
    memset(img, 0xBE, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    disc.file_open = true;
    CHECK_TRUE(disc_parse_bin(&disc, "game.cue"));  // no .cue sidecar → raw fallback
    uint32_t n = disc_read_sector(&disc, 0, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
}

// disc_read_sector must not assume 32-bit buffer alignment.
// UBSan would catch any unaligned multi-byte access to the output buffer.
TEST(SectorAccess, ReadWithUnalignedBuffer_NoFault)
{
    static uint8_t img[2048 * 3];
    memset(img, 0x5A, sizeof(img));
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    disc.file_open = true;
    CHECK_TRUE(disc_parse_iso(&disc));

    // Back the output buffer with one extra byte and use a +1 pointer so the
    // read lands at a byte-aligned but NOT 32-bit-aligned address.
    uint8_t backing[SECTOR_RAW_BYTES + 1];
    uint8_t *unaligned = backing + 1;
    memset(backing, 0xAA, sizeof(backing));

    uint32_t n = disc_read_sector(&disc, 0, unaligned, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
    // Synthesised Mode-1 sector: first byte is CD sync 0x00
    BYTES_EQUAL(0x00, unaligned[0]);
    // Byte before the buffer must be untouched (no overwrite before buf start)
    BYTES_EQUAL(0xAA, backing[0]);
}

// Scenario 13: reads in backwards / non-sequential LBA order must return the
// correct data for each LBA regardless of the read sequence.
// fatfs_sim always seeks before reading, so non-linear access is supported.
// Uses a 10-sector ISO where sector N contains fill byte N; reads 7 → 2 → 5.
TEST(SectorAccess, ReadNonSequential_LbaOrderIndependent)
{
    static uint8_t img[2048 * 10];
    for (int i = 0; i < 10; i++)
        memset(img + (size_t)i * 2048, (uint8_t)i, 2048);
    fatfs_sim_inject(&disc.image_file, img, sizeof(img));
    disc.file_open = true;
    CHECK_TRUE(disc_parse_iso(&disc));
    LONGS_EQUAL(10, disc.total_sectors);

    uint32_t n;

    // Backwards from the end: LBA 7, first data byte of the synthesised sector
    n = disc_read_sector(&disc, 7, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
    BYTES_EQUAL(0x07, buf[16]);   // first user-data byte at offset 16

    // Jump backwards to near the start
    n = disc_read_sector(&disc, 2, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
    BYTES_EQUAL(0x02, buf[16]);

    // Forward again
    n = disc_read_sector(&disc, 5, buf, SECTOR_MODE_RAW);
    LONGS_EQUAL(SECTOR_RAW_BYTES, n);
    BYTES_EQUAL(0x05, buf[16]);
}
