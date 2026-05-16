// =============================================================================
// disc_image.c — Multi-format disc image parser
// =============================================================================
//
// Parsing logic for ISO, BIN/CUE, NRG and MDF disc images.
//
// Design inspired by:
//   - fuseiso (GPL, Heikki Hannikainen 2005)  — ISO/BIN/NRG parsing
//   - DuckStation CDImageBin/CDImageCue.cpp (GPL-2.0, Connor McLaughlin) — CUE
//   - libmirage (GPL-2.0, Rok Mandeljc) — NRG/MDF format research
//
// All multi-byte integer fields in NRG/MDF are big-endian on disc but we
// convert to host (little-endian RP2350) byte order at parse time.
// =============================================================================

#include "disc_image.h"
#include "sector_cache.h" // SECTOR_RAW_SIZE

#include "ff.h"          // FatFS
#include "ecc.h"         // EDC/ECC for synthesised sectors
#include "logger.h"      // SD card activity logging
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

// 12-byte CD sync mark
const uint8_t CD_SYNC_PATTERN[CD_SYNC_SIZE] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Byte-swap a 32-bit big-endian value to little-endian
static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF000000) >> 24) |
           ((v & 0x00FF0000) >>  8) |
           ((v & 0x0000FF00) <<  8) |
           ((v & 0x000000FF) << 24);
}

// Byte-swap a 64-bit big-endian value to little-endian
static inline uint64_t be64(uint64_t v) {
    return ((uint64_t)be32((uint32_t)(v >> 32))) |
           ((uint64_t)be32((uint32_t)(v & 0xFFFFFFFF)) << 32);
}

// Case-insensitive file extension match: returns true if path ends with ext
static bool path_has_ext(const char *path, const char *ext) {
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char *p = path + plen - elen;
    for (size_t i = 0; i < elen; i++) {
        if (tolower((uint8_t)p[i]) != tolower((uint8_t)ext[i])) return false;
    }
    return true;
}

// Replace a file extension in 'src', writing result to 'dst' (size dst_size)
static void replace_ext(char *dst, size_t dst_size,
                         const char *src, const char *new_ext) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    // Find last '.'
    char *dot = strrchr(dst, '.');
    if (dot) {
        strncpy(dot, new_ext, dst_size - (size_t)(dot - dst) - 1);
    }
}

// ---------------------------------------------------------------------------
// disc_open — detect format and dispatch to parser
// ---------------------------------------------------------------------------

bool disc_open(disc_image_t *disc, const char *path) {
    memset(disc, 0, sizeof(*disc));
    strncpy(disc->image_path, path, MAX_PATH_LEN - 1);

    // Open the primary file
    FRESULT fr = f_open(&disc->image_file, path, FA_READ);
    if (fr != FR_OK) {
        printf("[DISC] Cannot open '%s': FatFS error %d\n", path, fr);
        return false;
    }
    disc->file_open = true;

    bool ok = false;

    if (path_has_ext(path, ".iso")) {
        printf("[DISC] Format: ISO 9660\n");
        ok = disc_parse_iso(disc);
    } else if (path_has_ext(path, ".bin")) {
        // Look for an accompanying .cue file
        char cue_path[MAX_PATH_LEN];
        replace_ext(cue_path, sizeof(cue_path), path, ".cue");
        printf("[DISC] Format: BIN/CUE (%s)\n", cue_path);
        ok = disc_parse_bin(disc, cue_path);
    } else if (path_has_ext(path, ".nrg")) {
        printf("[DISC] Format: Nero NRG\n");
        ok = disc_parse_nrg(disc);
    } else if (path_has_ext(path, ".mdf")) {
        char mds_path[MAX_PATH_LEN];
        replace_ext(mds_path, sizeof(mds_path), path, ".mds");
        printf("[DISC] Format: MDF/MDS (%s)\n", mds_path);
        ok = disc_parse_mdf(disc, mds_path);
    } else {
        printf("[DISC] Unknown extension, attempting ISO parse\n");
        ok = disc_parse_iso(disc);
    }

    if (!ok) {
        disc_close(disc);
        return false;
    }

    printf("[DISC] Opened: %d tracks, %u total sectors\n",
           disc->last_track, disc->total_sectors);
    return true;
}

void disc_close(disc_image_t *disc) {
    if (disc->file_open) {
        f_close(&disc->image_file);
        disc->file_open = false;
    }
}

// ---------------------------------------------------------------------------
// ISO parser
// ---------------------------------------------------------------------------
// An ISO file contains raw 2048-byte data sectors with no sync headers,
// no audio tracks, and always a single data track.
// We synthesise Mode 1 sectors on the fly when the host requests raw reads.

bool disc_parse_iso(disc_image_t *disc) {
    // Get file size
    FSIZE_t fsize = f_size(&disc->image_file);
    uint32_t total_sectors = (uint32_t)(fsize / SECTOR_DATA_BYTES);

    if (total_sectors == 0) {
        printf("[ISO] File too small\n");
        return false;
    }

    disc->first_track    = 1;
    disc->last_track     = 1;
    disc->total_sectors  = total_sectors;

    // Single data track
    track_t *trk = &disc->tracks[0];
    trk->number          = 1;
    trk->type            = TRACK_TYPE_DATA;
    trk->start_lba       = 0;
    trk->pregap_lba      = 0;
    trk->length_sectors  = total_sectors;
    trk->file_offset     = 0;
    trk->sector_size     = SECTOR_DATA_BYTES;  // Stored as 2048-byte sectors
    trk->data_offset     = 0;                  // Data starts at byte 0 of file sector

    printf("[ISO] %u sectors (%.1f MB)\n",
           total_sectors, (float)(fsize) / (1024.0f * 1024.0f));
    return true;
}

// ---------------------------------------------------------------------------
// BIN/CUE parser
// ---------------------------------------------------------------------------
// The .cue sheet is a plain text file describing the track layout.
// The .bin file contains raw 2352-byte sectors (or 2048 for some rips).
//
// CUE keywords we handle:
//   FILE "filename" BINARY
//   TRACK n MODE1/2352 | MODE2/2352 | AUDIO
//   INDEX 01 MM:SS:FF
//   PREGAP MM:SS:FF

bool disc_parse_bin(disc_image_t *disc, const char *cue_path) {
    FIL cue_file;
    FRESULT fr = f_open(&cue_file, cue_path, FA_READ);
    if (fr != FR_OK) {
        // No CUE sheet — assume single track, 2352 bytes/sector
        printf("[BIN] No CUE found, assuming single 2352-byte track\n");

        FSIZE_t fsize = f_size(&disc->image_file);
        uint32_t total = (uint32_t)(fsize / SECTOR_RAW_BYTES);

        disc->first_track   = 1;
        disc->last_track    = 1;
        disc->total_sectors = total;

        track_t *trk = &disc->tracks[0];
        trk->number         = 1;
        trk->type           = TRACK_TYPE_DATA;
        trk->start_lba      = 0;
        trk->length_sectors = total;
        trk->file_offset    = 0;
        trk->sector_size    = SECTOR_RAW_BYTES;
        trk->data_offset    = 16;  // Mode 1: data starts at byte 16 of raw sector
        return true;
    }

    // Parse the CUE sheet
    char line[256];
    uint8_t current_track = 0;
    // Running total of PREGAP sectors declared so far (not stored in BIN file).
    // INDEX 01 LBAs include virtual pregap, so we subtract this from file offsets.
    uint32_t accumulated_pregap = 0;

    while (f_gets(line, sizeof(line), &cue_file)) {
        // Trim leading whitespace
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "TRACK ", 6) == 0) {
            // "TRACK n MODE1/2352"
            uint8_t tnum = 0;
            char mode_str[32] = {0};
            sscanf(p + 6, "%hhu %31s", &tnum, mode_str);

            if (tnum < 1 || tnum > MAX_TRACKS) {
                printf("[CUE] invalid track number %u — skipping\n", tnum);
                current_track = 0;
                continue;
            }

            current_track = tnum;
            disc->last_track = tnum;
            if (disc->first_track == 0) disc->first_track = tnum;

            track_t *trk = &disc->tracks[tnum - 1];
            trk->number      = tnum;
            trk->file_offset = 0;  // Will be refined by INDEX 01

            // Determine sector layout from mode string
            if (strncmp(mode_str, "AUDIO", 5) == 0) {
                trk->type         = TRACK_TYPE_AUDIO;
                trk->sector_size  = SECTOR_RAW_BYTES;
                trk->data_offset  = 0;  // Audio: data IS the full sector
            } else if (strncmp(mode_str, "MODE1/2352", 10) == 0) {
                trk->type         = TRACK_TYPE_DATA;
                trk->sector_size  = SECTOR_RAW_BYTES;
                trk->data_offset  = 16; // Mode 1 data at offset 16
            } else if (strncmp(mode_str, "MODE2/2352", 10) == 0) {
                trk->type         = TRACK_TYPE_XA;
                trk->sector_size  = SECTOR_RAW_BYTES;
                trk->data_offset  = 24; // Mode 2 XA data at offset 24
            } else if (strncmp(mode_str, "MODE1/2048", 10) == 0) {
                trk->type         = TRACK_TYPE_DATA;
                trk->sector_size  = SECTOR_DATA_BYTES;
                trk->data_offset  = 0;
            } else {
                printf("[CUE] Unknown track mode '%s', assuming MODE1/2352\n", mode_str);
                trk->type         = TRACK_TYPE_DATA;
                trk->sector_size  = SECTOR_RAW_BYTES;
                trk->data_offset  = 16;
            }

        } else if (strncmp(p, "PREGAP ", 7) == 0 && current_track > 0) {
            // "PREGAP MM:SS:FF" — virtual silence NOT stored in BIN file.
            // Accumulate so INDEX 01 file offsets are adjusted correctly.
            uint8_t mm, ss, ff;
            sscanf(p + 7, "%hhu:%hhu:%hhu", &mm, &ss, &ff);
            accumulated_pregap += (uint32_t)mm * 60u * 75u
                                + (uint32_t)ss * 75u
                                + ff;

        } else if (strncmp(p, "INDEX 00 ", 9) == 0 && current_track > 0) {
            // "INDEX 00 MM:SS:FF" — pregap start (silence IS in BIN file).
            uint8_t mm, ss, ff;
            sscanf(p + 9, "%hhu:%hhu:%hhu", &mm, &ss, &ff);
            msf_t msf;
            msf.minute = ((mm / 10) << 4) | (mm % 10);
            msf.second = ((ss / 10) << 4) | (ss % 10);
            msf.frame  = ((ff / 10) << 4) | (ff % 10);
            disc->tracks[current_track - 1].pregap_lba = msf_to_lba(msf);

        } else if (strncmp(p, "INDEX 01 ", 9) == 0 && current_track > 0) {
            // "INDEX 01 MM:SS:FF" — track start in disc time
            uint8_t mm, ss, ff;
            sscanf(p + 9, "%hhu:%hhu:%hhu", &mm, &ss, &ff);

            msf_t msf;
            msf.minute = ((mm / 10) << 4) | (mm % 10);
            msf.second = ((ss / 10) << 4) | (ss % 10);
            msf.frame  = ((ff / 10) << 4) | (ff % 10);

            uint32_t lba = msf_to_lba(msf);
            track_t *trk = &disc->tracks[current_track - 1];
            trk->start_lba  = lba;
            if (trk->pregap_lba == 0) trk->pregap_lba = lba;
            // Subtract accumulated virtual pregap: those sectors aren't in the file.
            // Guard against malformed CUE where pregap exceeds the INDEX 01 LBA.
            uint32_t file_lba = (lba >= accumulated_pregap) ? (lba - accumulated_pregap) : 0;
            trk->file_offset = file_lba * trk->sector_size;
        }
    }

    f_close(&cue_file);

    if (disc->first_track == 0) return true;  // all tracks skipped — no UB in loop below

    // Calculate track lengths
    for (uint8_t i = disc->first_track; i <= disc->last_track; i++) {
        track_t *trk = &disc->tracks[i - 1];
        uint32_t next_lba;
        if (i < disc->last_track) {
            next_lba = disc->tracks[i].start_lba;
        } else {
            FSIZE_t fsize = f_size(&disc->image_file);
            next_lba = trk->start_lba + (uint32_t)((fsize - trk->file_offset)
                       / trk->sector_size);
        }
        trk->length_sectors = (next_lba > trk->start_lba)
                            ? (next_lba - trk->start_lba) : 0;
    }

    disc->total_sectors = disc->tracks[disc->last_track - 1].start_lba
                        + disc->tracks[disc->last_track - 1].length_sectors;
    return true;
}

// ---------------------------------------------------------------------------
// NRG parser (Nero Burning ROM)
// ---------------------------------------------------------------------------
// NRG files have a header at the END of the file.
// The last 12 bytes (v2) or 8 bytes (v1) contain a magic and an offset to
// the first chunk.  Chunks are read sequentially; DAOX/DAOI chunks contain
// track information.

bool disc_parse_nrg(disc_image_t *disc) {
    FSIZE_t fsize = f_size(&disc->image_file);
    if (fsize < 12) return false;

    uint64_t chunk_offset = 0;
    bool     is_v2 = false;

    // Try NRG v2 footer (last 12 bytes)
    {
        nrg_footer_v2_t footer;
        UINT br;
        f_lseek(&disc->image_file, fsize - sizeof(footer));
        f_read(&disc->image_file, &footer, sizeof(footer), &br);
        if (br == sizeof(footer) && be32(footer.magic) == 0x4E455235) { // "NER5"
            is_v2        = true;
            chunk_offset = be64(footer.offset);
            printf("[NRG] Detected v2 header at offset %llu\n", (unsigned long long)chunk_offset);
        }
    }

    // Try NRG v1 footer (last 8 bytes)
    if (!is_v2) {
        nrg_footer_v1_t footer;
        UINT br;
        f_lseek(&disc->image_file, fsize - sizeof(footer));
        f_read(&disc->image_file, &footer, sizeof(footer), &br);
        if (br == sizeof(footer) && be32(footer.magic) == 0x4E45524F) { // "NERO"
            chunk_offset = be32(footer.offset);
            printf("[NRG] Detected v1 header at offset %llu\n", (unsigned long long)chunk_offset);
        } else {
            printf("[NRG] No valid NRG footer found\n");
            return false;
        }
    }

    // Walk the chunk list
    f_lseek(&disc->image_file, chunk_offset);
    disc->first_track = 1;

    uint8_t track_idx = 0;

    for (;;) {
        nrg_chunk_header_t hdr;
        UINT br;
        f_read(&disc->image_file, &hdr, sizeof(hdr), &br);
        if (br < sizeof(hdr)) break;

        uint32_t chunk_id   = be32(hdr.chunk_id);
        uint32_t chunk_size = be32(hdr.chunk_size);

        if (chunk_id == NRG_CHUNK_END) break;
        if (chunk_size == 0) break;  /* malformed: zero-size chunk loops forever */

        FSIZE_t chunk_data_pos = f_tell(&disc->image_file);

        if (chunk_id == NRG_CHUNK_DAOX || chunk_id == NRG_CHUNK_DAOI) {
            // DAO (Disc-At-Once) track information chunk
            // Structure: 22-byte header, then variable track entries
            // Each entry is 42 bytes for DAOX (v2) or 30 bytes for DAOI (v1)

            if (chunk_size < 22) {              /* malformed: underflows bytes_left */
                f_lseek(&disc->image_file, chunk_data_pos + chunk_size);
                continue;
            }
            // Skip 22-byte DAO session header
            f_lseek(&disc->image_file, chunk_data_pos + 22);
            uint32_t bytes_left = chunk_size - 22;
            uint32_t entry_size = is_v2 ? 42 : 30;

            while (bytes_left >= entry_size && track_idx < MAX_TRACKS) {
                uint8_t entry[42];
                f_read(&disc->image_file, entry, entry_size, &br);
                if (br < entry_size) break;

                // Parse track entry fields (offsets from Nero SDK documentation)
                // Bytes 0–11: ISRC
                // Byte 12: sector size (0x0800=2048, 0x0920=2352, etc.)
                // Byte 14: track mode
                // Bytes 16–19: index 0 LBA
                // Bytes 20–23: index 1 LBA  (track start)
                // Bytes 24–27: track end LBA
                // Bytes 28–35 (v2): image file start offset (64-bit)
                // Bytes 28–31 (v1): image file start offset (32-bit)

                uint16_t raw_sector_size = (uint16_t)((entry[12] << 8) | entry[13]);
                uint8_t  track_mode      = entry[14];

                uint32_t idx0_lba = be32(*(uint32_t*)(entry + 16));
                uint32_t idx1_lba = be32(*(uint32_t*)(entry + 20));
                uint32_t end_lba  = be32(*(uint32_t*)(entry + 24));

                uint64_t file_off;
                if (is_v2) {
                    uint64_t raw64;
                    memcpy(&raw64, entry + 28, sizeof(raw64));
                    file_off = be64(raw64);
                } else {
                    // v1 (DAOI): 30-byte entries have no file-offset field.
                    // Data is laid out sequentially from byte 0 of the image.
                    file_off = (uint64_t)idx0_lba * raw_sector_size;
                }

                // Skip lead-in (idx1=0, end=0) and lead-out (idx1==end, zero-length)
                if (idx1_lba == 0 && end_lba == 0) {
                    bytes_left -= entry_size;
                    continue;
                }
                if (end_lba <= idx1_lba) {
                    bytes_left -= entry_size;
                    continue;
                }

                track_t *trk = &disc->tracks[track_idx];
                trk->number         = track_idx + 1;
                trk->start_lba      = idx1_lba;
                trk->pregap_lba     = idx0_lba;
                trk->length_sectors = end_lba - idx1_lba;
                trk->file_offset    = (uint32_t)file_off;
                trk->sector_size    = (raw_sector_size == 0x0800) ? 2048
                                    : (raw_sector_size == 0x0920) ? 2352
                                    : 2352;  // Default to 2352

                if (track_mode == 0x07) {  // Audio
                    trk->type        = TRACK_TYPE_AUDIO;
                    trk->data_offset = 0;
                } else {
                    trk->type        = TRACK_TYPE_DATA;
                    trk->data_offset = (trk->sector_size == 2352) ? 16 : 0;
                }

                printf("[NRG] Track %d: LBA %u-%u, size %u\n",
                       trk->number, trk->start_lba,
                       trk->start_lba + trk->length_sectors, trk->sector_size);

                track_idx++;
                bytes_left -= entry_size;
            }
        }

        // Skip to next chunk
        f_lseek(&disc->image_file, chunk_data_pos + chunk_size);
    }

    disc->last_track    = track_idx;
    disc->total_sectors = (track_idx > 0)
        ? disc->tracks[track_idx - 1].start_lba + disc->tracks[track_idx - 1].length_sectors
        : 0;

    return track_idx > 0;
}

// ---------------------------------------------------------------------------
// MDF/MDS parser (Alcohol 120%)
// ---------------------------------------------------------------------------
// The .mds file describes the disc structure; .mdf is the raw sector data.

bool disc_parse_mdf(disc_image_t *disc, const char *mds_path) {
    FIL mds_file;
    FRESULT fr = f_open(&mds_file, mds_path, FA_READ);
    if (fr != FR_OK) {
        printf("[MDF] Cannot open MDS file '%s'\n", mds_path);
        return false;
    }

    // Read and validate MDS header
    mds_header_t hdr;
    UINT br;
    f_read(&mds_file, &hdr, sizeof(hdr), &br);
    if (br < sizeof(hdr) || strncmp(hdr.signature, MDS_SIGNATURE, MDS_SIG_LEN) != 0) {
        printf("[MDF] Invalid MDS signature\n");
        f_close(&mds_file);
        return false;
    }

    printf("[MDF] MDS version %d.%d\n", hdr.version[0], hdr.version[1]);

    // Read session blocks to find tracks
    uint32_t sessions_offset = hdr.sessions_blocks_offset;
    uint16_t session_count   = hdr.session_count;

    uint8_t track_idx = 0;

    for (uint16_t si = 0; si < session_count && si < 1; si++) {
        // Seek to session block
        f_lseek(&mds_file, sessions_offset + si * sizeof(mds_session_block_t));

        mds_session_block_t sess;
        f_read(&mds_file, &sess, sizeof(sess), &br);
        if (br < sizeof(sess)) break;

        printf("[MDF] Session %d: %d tracks\n", si + 1, sess.tracks_in_session);

        // Read track blocks for this session
        f_lseek(&mds_file, sess.tracks_blocks_offset);

        for (uint8_t ti = 0; ti < sess.tracks_in_session && track_idx < MAX_TRACKS; ti++) {
            mds_track_block_t tblk;
            f_read(&mds_file, &tblk, sizeof(tblk), &br);
            if (br < sizeof(tblk)) break;

            // Skip lead-in/lead-out entries
            if (tblk.track_number == 0x00 || tblk.track_number == 0xA0 ||
                tblk.track_number == 0xA1 || tblk.track_number == 0xA2 ||
                tblk.track_number == 0xAA) {
                continue;
            }

            track_t *trk = &disc->tracks[track_idx];
            trk->number  = tblk.track_number;
            trk->start_lba  = tblk.start_sector;
            trk->file_offset = (uint32_t)(tblk.start_offset & 0xFFFFFFFF);
            trk->sector_size = tblk.sector_size;

            // Determine track type from ADR/CTL
            if ((tblk.adr_ctl & 0x0C) == 0x00) {  // CTL bits 3:2 = 00 = audio
                trk->type        = TRACK_TYPE_AUDIO;
                trk->data_offset = 0;
            } else {
                trk->type        = TRACK_TYPE_DATA;
                trk->data_offset = (trk->sector_size == 2352) ? 16 : 0;
            }

            printf("[MDF] Track %d: LBA %u sector_size %u\n",
                   trk->number, trk->start_lba, trk->sector_size);

            if (disc->first_track == 0) disc->first_track = trk->number;
            disc->last_track = trk->number;
            track_idx++;
        }
    }

    // Calculate track lengths
    for (uint8_t i = 0; i < track_idx; i++) {
        if (i + 1 < track_idx) {
            uint32_t end_lba = disc->tracks[i + 1].start_lba;
            uint32_t beg_lba = disc->tracks[i].start_lba;
            disc->tracks[i].length_sectors = (end_lba > beg_lba) ? (end_lba - beg_lba) : 0;
        } else {
            // Last track: length from file size
            if (disc->tracks[i].sector_size == 0)   /* malformed: avoid divide-by-zero */
                disc->tracks[i].sector_size = SECTOR_RAW_BYTES;
            FSIZE_t fsize = f_size(&disc->image_file);
            disc->tracks[i].length_sectors =
                (uint32_t)((fsize - disc->tracks[i].file_offset) / disc->tracks[i].sector_size);
        }
    }

    disc->total_sectors = (track_idx > 0)
        ? disc->tracks[track_idx - 1].start_lba + disc->tracks[track_idx - 1].length_sectors
        : 0;

    f_close(&mds_file);
    return track_idx > 0;
}

// ---------------------------------------------------------------------------
// Sector reading
// ---------------------------------------------------------------------------

// Synthesise a full 2352-byte Mode 1 sector from 2048 bytes of payload data.
// This is necessary when the image only stores 2048-byte sectors (plain ISO).
void disc_synthesise_sector(uint8_t *buf, uint32_t lba, const uint8_t *data2048) {
    // Sync pattern
    memcpy(buf, CD_SYNC_PATTERN, CD_SYNC_SIZE);

    // MSF address (physical = LBA + 150 frames pregap)
    msf_t msf = lba_to_msf(lba);
    buf[12] = msf.minute;
    buf[13] = msf.second;
    buf[14] = msf.frame;
    buf[15] = 0x01;  // Mode 1

    // Data payload
    memcpy(buf + 16, data2048, 2048);

    // Compute and write correct EDC (bytes 2064-2067) and P/Q ECC parity
    // (bytes 2076-2351).  Some CD32 software reads raw sectors via READS
    // and may verify the EDC.  Without correct values, such software would
    // report read errors even though the data is fine.
    ecc_sector_complete(buf);
}

// Read one sector from the disc image.
// Returns number of bytes placed in buf (up to SECTOR_RAW_BYTES).
uint32_t disc_read_sector(disc_image_t *disc, uint32_t lba,
                           uint8_t *buf, sector_mode_t mode) {
    if (!disc->file_open) return 0;

    // Find the track this LBA belongs to
    const track_t *trk = disc_find_track(disc, lba);
    if (!trk) {
        // Beyond disc — return a blank sector
        memset(buf, 0, SECTOR_RAW_SIZE);
        return 0;
    }

    // Sector's byte offset in the image file
    uint32_t sector_idx  = lba - trk->start_lba;
    uint32_t file_offset = trk->file_offset + sector_idx * trk->sector_size;

    // Seek to the sector in the file
    FRESULT fr = f_lseek(&disc->image_file, file_offset);
    if (fr != FR_OK) {
        printf("[DISC] Seek error at offset %u\n", file_offset);
        LOG_ERROR_MSG("disc f_lseek failed at offset=%lu lba=%lu fr=%d",
                      (unsigned long)file_offset, (unsigned long)lba, fr);
        return 0;
    }

    UINT br;

    if (trk->sector_size == SECTOR_DATA_BYTES) {
        // ISO-style: read 2048 bytes and synthesise a full sector
        uint8_t data2048[SECTOR_DATA_BYTES];
        fr = f_read(&disc->image_file, data2048, SECTOR_DATA_BYTES, &br);
        if (fr != FR_OK || br < SECTOR_DATA_BYTES) {
            LOG_ERROR_MSG("disc f_read ISO fail lba=%lu fr=%d br=%u",
                          (unsigned long)lba, fr, (unsigned)br);
            return 0;
        }
        disc_synthesise_sector(buf, lba, data2048);
    } else {
        // Raw sector: read directly (2352 bytes)
        fr = f_read(&disc->image_file, buf, SECTOR_RAW_BYTES, &br);
        if (fr != FR_OK || br < SECTOR_RAW_BYTES) {
            LOG_ERROR_MSG("disc f_read RAW fail lba=%lu fr=%d br=%u",
                          (unsigned long)lba, fr, (unsigned)br);
            return 0;
        }
    }

    uint32_t deliver_bytes = (mode == SECTOR_MODE_RAW)
                             ? SECTOR_RAW_BYTES : SECTOR_DATA_BYTES;

    disc->last_read_lba = lba;
    return deliver_bytes;
}

// ---------------------------------------------------------------------------
// Track lookup
// ---------------------------------------------------------------------------

const track_t *disc_find_track(const disc_image_t *disc, uint32_t lba) {
    for (uint8_t i = disc->first_track; i <= disc->last_track; i++) {
        const track_t *trk = &disc->tracks[i - 1];
        if (lba >= trk->start_lba && lba < trk->start_lba + trk->length_sectors) {
            return trk;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// TOC response builder
// ---------------------------------------------------------------------------
// The CD32 akiko reads the TOC as a series of bytes in the format returned
// by READTOC / GETTD commands.  Each entry is: [track_no, min, sec] (BCD).

uint32_t disc_build_toc_response(const disc_image_t *disc,
                                  uint8_t *buf, uint32_t buf_size) {
    uint32_t pos = 0;

    for (uint8_t i = disc->first_track; i <= disc->last_track; i++) {
        if (pos + 3 > buf_size) break;
        const track_t *trk = &disc->tracks[i - 1];
        msf_t msf = lba_to_msf(trk->start_lba);

        // Encode track number as BCD
        buf[pos++] = ((i / 10) << 4) | (i % 10);
        buf[pos++] = msf.minute;
        buf[pos++] = msf.second;
    }

    // Lead-out entry (track 0xAA)
    if (pos + 3 <= buf_size) {
        msf_t msf = lba_to_msf(disc->total_sectors);
        buf[pos++] = 0xAA;
        buf[pos++] = msf.minute;
        buf[pos++] = msf.second;
    }

    return pos;
}
