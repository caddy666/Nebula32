#pragma once
// =============================================================================
// disc_image.h — Multi-format CD disc image abstraction
// =============================================================================
//
// Supports:
//   ISO 9660 (.iso)  — 2048-byte sectors, data-only, no audio tracks
//   BIN/CUE          — Raw 2352-byte sectors; separate .cue sheet for TOC
//   Nero (.nrg)      — Nero Burning ROM proprietary format (CUEX/DAOX chunks)
//   Media Descriptor (.mdf/.mds) — Alcohol 120% format
//
// Design principles borrowed from fuseiso (GPL, Heikki Hannikainen 2005) and
// DuckStation's disc image code (GPL-2.0, Connor McLaughlin).
//
// All sector I/O goes through disc_read_sector() which hides the underlying
// file format from the CD-ROM command layer.
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "ff.h"           // FatFS file handle
#include "cd_types.h"     // msf_t, drive_state_t, sector_mode_t

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define MAX_TRACKS        99    // Red Book maximum track count
#define MAX_INDICES        2    // We track index 0 (pregap) and index 1 (data)
#define MAX_PATH_LEN     256    // Maximum full path length on FAT volume

// ---------------------------------------------------------------------------
// Track types (as recorded in the TOC)
// Matches the ADR/CTRL nibble in Q-channel subcode.
// ---------------------------------------------------------------------------
typedef enum {
    TRACK_TYPE_AUDIO    = 0x00,  // CD-DA audio (Red Book)
    TRACK_TYPE_DATA     = 0x04,  // Data track (Yellow Book Mode 1 or 2)
    TRACK_TYPE_XA       = 0x14,  // CD-ROM XA Mode 2 data track
} track_type_t;

// ---------------------------------------------------------------------------
// Disc image formats we can parse
// ---------------------------------------------------------------------------
typedef enum {
    DISC_FORMAT_UNKNOWN  = 0,
    DISC_FORMAT_ISO,    // Plain ISO 9660 — 2048 bytes/sector
    DISC_FORMAT_BIN,    // Raw binary image — 2352 bytes/sector
    DISC_FORMAT_NRG,    // Nero Burning ROM
    DISC_FORMAT_MDF,    // Alcohol 120% MDF
} disc_format_t;

// ---------------------------------------------------------------------------
// One track entry from the disc's Table of Contents
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t       number;        // Track number 1-99
    track_type_t  type;          // Audio or data
    uint32_t      start_lba;     // First sector LBA (after pregap)
    uint32_t      pregap_lba;    // First sector of pregap (index 0)
    uint32_t      length_sectors;// Track length in sectors
    uint32_t      file_offset;   // Byte offset in the image file where this
                                 // track's sector 0 is stored
    uint32_t      sector_size;   // Raw bytes per sector in the file
                                 // (2048 for ISO, 2352 for BIN/NRG/MDF)
    uint32_t      data_offset;   // Byte offset WITHIN a raw sector where
                                 // the 2048-byte data payload begins
                                 // (0 for ISO, 16 for Mode1/BIN, 24 for XA)
} track_t;

// ---------------------------------------------------------------------------
// The full disc TOC + file handles
// ---------------------------------------------------------------------------
typedef struct {
    disc_format_t  format;
    char           image_path[MAX_PATH_LEN];

    // TOC data
    uint8_t        first_track;   // Usually 1
    uint8_t        last_track;    // 1-99
    uint32_t       total_sectors; // Total sectors on disc

    track_t        tracks[MAX_TRACKS + 2]; // +2: lead-in sentinel, lead-out

    // FatFS file handles (one per physical file in multi-file formats)
    FIL            image_file;    // Primary image file (always open)
    bool           file_open;

    // Cached last-read sector LBA to detect sequential reads
    uint32_t       last_read_lba;

} disc_image_t;

// ---------------------------------------------------------------------------
// Sector header structures used during raw sector parsing
// ---------------------------------------------------------------------------

// 12-byte sync pattern that begins every raw CD sector
// Pattern: 0x00, 0xFF*10, 0x00  — unmistakable on the disc surface
#define CD_SYNC_SIZE          12
extern const uint8_t CD_SYNC_PATTERN[CD_SYNC_SIZE];

// Mode 1 sector layout (2352 bytes total):
//   [0..11]  = sync
//   [12..14] = MSF address
//   [15]     = mode (0x01)
//   [16..2063] = 2048 bytes user data
//   [2064..2075] = EDC + ECC
typedef struct __attribute__((packed)) {
    uint8_t  sync[12];
    uint8_t  minute;
    uint8_t  second;
    uint8_t  frame;
    uint8_t  mode;           // 0x01 for Mode 1
    uint8_t  data[2048];
    uint8_t  edc[4];
    uint8_t  reserved[8];
    uint8_t  ecc[276];
} sector_mode1_t;

// Mode 2 Form 1 (CD-ROM XA, 2352 bytes):
//   [0..11]  = sync
//   [12..14] = MSF
//   [15]     = mode (0x02)
//   [16..23] = subheader (8 bytes, repeated twice)
//   [24..2071] = 2048 bytes user data
//   [2072..2351] = EDC + ECC
typedef struct __attribute__((packed)) {
    uint8_t  sync[12];
    uint8_t  minute;
    uint8_t  second;
    uint8_t  frame;
    uint8_t  mode;           // 0x02
    uint8_t  subheader[8];   // file, channel, submode, coding (×2)
    uint8_t  data[2048];
    uint8_t  edc_ecc[280];
} sector_mode2_form1_t;

// ---------------------------------------------------------------------------
// NRG file format structures
// Nero stores metadata in chunks at the end of the file.
// Chunk IDs are 4 ASCII bytes in big-endian order.
// ---------------------------------------------------------------------------
#define NRG_CHUNK_CUEX   0x43554558  // "CUEX" — CD-Text / cue sheet (NRG v2)
#define NRG_CHUNK_CUES   0x43554553  // "CUES" — cue sheet (NRG v1)
#define NRG_CHUNK_DAOX   0x44414F58  // "DAOX" — DAO recording info (NRG v2)
#define NRG_CHUNK_DAOI   0x44414F49  // "DAOI" — DAO recording info (NRG v1)
#define NRG_CHUNK_CDTX   0x43445458  // "CDTX" — CD Text
#define NRG_CHUNK_ETN2   0x45544E32  // "ETN2" — extra track info v2
#define NRG_CHUNK_SINF   0x53494E46  // "SINF" — session info
#define NRG_CHUNK_MTYP   0x4D545950  // "MTYP" — media type
#define NRG_CHUNK_END    0x454E4421  // "END!" — sentinel

typedef struct __attribute__((packed)) {
    uint32_t chunk_id;    // Big-endian 4-byte ASCII ID
    uint32_t chunk_size;  // Big-endian size of chunk data (not including header)
} nrg_chunk_header_t;

// NRG v2 end-of-file pointer (last 12 bytes of .nrg file)
typedef struct __attribute__((packed)) {
    uint32_t magic;       // 0x4E455235 "NER5"
    uint64_t offset;      // Big-endian: byte offset of first chunk from file start
} nrg_footer_v2_t;

// NRG v1 end-of-file pointer (last 8 bytes of .nrg file)
typedef struct __attribute__((packed)) {
    uint32_t magic;       // 0x4E45524F "NERO"
    uint32_t offset;      // Big-endian 32-bit offset
} nrg_footer_v1_t;

// ---------------------------------------------------------------------------
// MDS (Alcohol 120%) format structures
// The .mds file is a metadata file; .mdf is the raw sector data.
// ---------------------------------------------------------------------------
#define MDS_SIGNATURE  "MEDIA DESCRIPTOR"
#define MDS_SIG_LEN    16

typedef struct __attribute__((packed)) {
    char     signature[MDS_SIG_LEN]; // "MEDIA DESCRIPTOR"
    uint8_t  version[2];             // [0]=major [1]=minor
    uint16_t medium_type;            // 0=CD-ROM, 1=CD-R, etc.
    uint16_t session_count;
    uint16_t unknown1[2];
    uint16_t bca_length;
    uint32_t unknown2[2];
    uint32_t bca_offset;
    uint32_t unknown3[6];
    uint32_t disc_structures_offset;
    uint32_t unknown4[3];
    uint32_t sessions_blocks_offset;
    uint32_t dpm_blocks_offset;
} mds_header_t;

typedef struct __attribute__((packed)) {
    int32_t  session_start;      // Start sector (can be negative for lead-in)
    int32_t  session_end;        // End sector
    uint16_t session_number;
    uint8_t  tracks_in_session;
    uint8_t  tracks_in_session2; // (same value repeated)
    uint16_t first_track;
    uint16_t last_track;
    uint32_t unknown;
    uint32_t tracks_blocks_offset;
} mds_session_block_t;

typedef struct __attribute__((packed)) {
    uint8_t  mode;               // Track mode (0xA9=audio, 0xAA=Mode1, etc.)
    uint8_t  unknown1[3];
    uint8_t  adr_ctl;            // ADR/CTL from Q-channel
    uint8_t  track_number;       // 0xAA for lead-out
    uint8_t  point;              // Point value
    uint8_t  unknown2[4];
    uint8_t  minute;
    uint8_t  second;
    uint8_t  frame;
    uint32_t extra_offset;       // Offset to extra track info
    uint16_t sector_size;        // Raw sector size (2048/2352/2448)
    uint8_t  unknown3[18];
    uint32_t start_sector;       // LBA of first sector
    uint64_t start_offset;       // Byte offset in .mdf file
    uint8_t  files_count;
    uint32_t footer_offset;
    uint8_t  unknown4[24];
} mds_track_block_t;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Open a disc image from the SD card.
// Detects format from file extension and parses TOC.
// Returns true on success.
bool disc_open(disc_image_t *disc, const char *path);

// Close the disc image and release file handles.
void disc_close(disc_image_t *disc);

// Read one sector at 'lba' and place the raw data in 'buf'.
// 'buf' must be at least 2352 bytes.
// 'mode' controls whether 2048-byte cooked or 2352-byte raw data is returned.
// Returns number of bytes written to buf (0 on error).
uint32_t disc_read_sector(disc_image_t *disc, uint32_t lba,
                          uint8_t *buf, sector_mode_t mode);

// Fill a 2352-byte sector buffer with a synthesised Mode 1 sector.
// Used when the image only provides 2048-byte data (ISO format).
void disc_synthesise_sector(uint8_t *buf, uint32_t lba, const uint8_t *data2048);

// Find which track contains 'lba'.  Returns NULL if not found.
const track_t *disc_find_track(const disc_image_t *disc, uint32_t lba);

// Build a TOC response buffer as the CD32 Kickstart expects it (GETTD / READTOC).
// 'buf' receives the encoded TOC; returns number of bytes written.
uint32_t disc_build_toc_response(const disc_image_t *disc, uint8_t *buf,
                                 uint32_t buf_size);

// Helpers to parse each supported format (called internally by disc_open)
bool disc_parse_iso (disc_image_t *disc);
bool disc_parse_bin (disc_image_t *disc, const char *cue_path);
bool disc_parse_nrg (disc_image_t *disc);
bool disc_parse_mdf (disc_image_t *disc, const char *mds_path);

