// =============================================================================
// virtual_disc.c — On-the-fly ISO 9660 disc synthesiser for partition 2
// =============================================================================
//
// Scans the FAT32 partition "1:/" and synthesises a Mode 1 ISO 9660 disc
// image in RAM.  Directory metadata is built into sector buffers at read
// time; file data is read directly from the SD card on demand.
//
// ISO 9660 structural layout:
//   LBA  0-15  : system area (zeros)
//   LBA 16     : Primary Volume Descriptor (PVD)
//   LBA 17     : Volume Descriptor Set Terminator (VDST)
//   LBA 18     : L-path table (little-endian)
//   LBA 19     : M-path table (big-endian)
//   LBA 20+    : directory data sectors (one 2048-byte sector per directory)
//   LBA 20+N   : file data sectors (packed, one sector per 2048 bytes of file)
// =============================================================================

#include "virtual_disc.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Byte-order helpers
// ---------------------------------------------------------------------------

static void put_le16(uint8_t *buf, uint32_t off, uint16_t val) {
    buf[off + 0] = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)((val >> 8) & 0xFF);
}

static void put_be16(uint8_t *buf, uint32_t off, uint16_t val) {
    buf[off + 0] = (uint8_t)((val >> 8) & 0xFF);
    buf[off + 1] = (uint8_t)(val & 0xFF);
}

static void put_le32(uint8_t *buf, uint32_t off, uint32_t val) {
    buf[off + 0] = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static void put_be32(uint8_t *buf, uint32_t off, uint32_t val) {
    buf[off + 0] = (uint8_t)((val >> 24) & 0xFF);
    buf[off + 1] = (uint8_t)((val >> 16) & 0xFF);
    buf[off + 2] = (uint8_t)((val >> 8) & 0xFF);
    buf[off + 3] = (uint8_t)(val & 0xFF);
}

// Write LE u16 immediately followed by BE u16 (ISO 9660 "both byte order")
static void put_lebe16(uint8_t *buf, uint32_t off, uint16_t val) {
    put_le16(buf, off,     val);
    put_be16(buf, off + 2, val);
}

// Write LE u32 immediately followed by BE u32 (ISO 9660 "both byte order")
static void put_lebe32(uint8_t *buf, uint32_t off, uint32_t val) {
    put_le32(buf, off,     val);
    put_be32(buf, off + 4, val);
}

// ---------------------------------------------------------------------------
// Name sanitiser — produce ISO 9660 Level 2 compatible names
// ---------------------------------------------------------------------------
static void sanitise_name(char *dst, const char *src) {
    int out = 0;
    for (int i = 0; src[i] != '\0' && out < VDISC_NAME_LEN - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') {
            dst[out++] = (char)(c - 'a' + 'A');
        } else if ((c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == '_' || c == '.' || c == '-' || c == ',') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '_';
        }
    }
    dst[out] = '\0';
}

// ---------------------------------------------------------------------------
// Reconstruct FatFS path for an entry
// ---------------------------------------------------------------------------
// Root (idx==0) returns "1:/".
// Others walk parent_idx chain until hitting root (idx==0), then prefix "1:/".
static void entry_path(const vdisc_t *vd, uint32_t idx, char *buf, size_t bufsz) {
    if (idx == 0) {
        // Root
        if (bufsz > 0) {
            buf[0] = '\0';
            strncat(buf, "1:/", bufsz - 1);
        }
        return;
    }

    // Walk chain to root, collecting component indices
    uint32_t chain[9];  // max depth 8 means max 8 components above root
    int depth = 0;
    uint32_t cur = idx;
    while (cur != 0 && depth < 9) {
        chain[depth++] = cur;
        cur = vd->table[cur].parent_idx;
    }

    // Build path from root down
    if (bufsz > 0) buf[0] = '\0';
    strncat(buf, "1:/", bufsz - 1);
    for (int i = depth - 1; i >= 0; i--) {
        size_t len = strlen(buf);
        if (len + 1 < bufsz) {
            strncat(buf, vd->table[chain[i]].name, bufsz - len - 1);
        }
        if (i > 0) {
            size_t len2 = strlen(buf);
            if (len2 + 1 < bufsz) {
                strncat(buf, "/", bufsz - len2 - 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Calculate path table size in bytes
// ---------------------------------------------------------------------------
static uint32_t calc_path_table_size(const vdisc_t *vd) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (!vd->table[i].is_dir) continue;
        uint32_t id_len = (i == 0) ? 1u : (uint32_t)strlen(vd->table[i].name);
        uint32_t entry_sz = 8u + id_len + (id_len & 1u);
        total += entry_sz;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Write one ISO 9660 directory record into buf[offset].
// Returns rec_len (always even) or 0 if it doesn't fit.
// ---------------------------------------------------------------------------
static int write_dir_record(uint8_t *buf, int offset,
                             uint32_t lba, uint32_t size,
                             uint8_t flags,
                             const uint8_t *id, int id_len) {
    int rec_len = 33 + id_len;
    if (rec_len & 1) rec_len++;
    if (offset + rec_len > 2048) return 0;

    uint8_t *r = buf + offset;
    memset(r, 0, (size_t)rec_len);

    r[0] = (uint8_t)rec_len;
    r[1] = 0;                                    // Extended attribute record length
    put_le32(r, 2, lba);                         // Location of extent (LE)
    put_be32(r, 6, lba);                         // Location of extent (BE)
    put_le32(r, 10, size);                       // Data length (LE)
    put_be32(r, 14, size);                       // Data length (BE)
    // Date/time: bytes 18-24 = 7 zeros (unspecified)
    r[25] = flags;                               // File flags
    r[26] = 0;                                   // File unit size
    r[27] = 0;                                   // Interleave gap size
    put_lebe16(r, 28, 1);                        // Volume sequence number
    r[32] = (uint8_t)id_len;
    memcpy(r + 33, id, (size_t)id_len);

    return rec_len;
}

// ---------------------------------------------------------------------------
// Build the Primary Volume Descriptor at LBA 16
// ---------------------------------------------------------------------------
static void build_pvd(const vdisc_t *vd, uint8_t *buf) {
    memset(buf, 0x20, 2048);   // ISO 9660 unused fields are space-filled
    memset(buf, 0,    2048);   // Actually zero-fill first, then we'll space-fill specific fields

    // Re-do: start with zeros, space-fill the identifier fields per ISO 9660
    memset(buf, 0, 2048);

    buf[0] = 0x01;             // Volume Descriptor Type: Primary
    memcpy(buf + 1, "CD001", 5);
    buf[6] = 0x01;             // Version

    // System identifier [8..39] — spaces
    memset(buf + 8, 0x20, 32);

    // Volume identifier [40..71] — "NEBULA32", space-padded to 32
    memset(buf + 40, 0x20, 32);
    memcpy(buf + 40, "NEBULA32", 8);

    // Volume space size [80..87] — both byte order u32
    put_lebe32(buf, 80, vd->total_sectors);

    // Volume set size [120..123] — both byte order u16 = 1
    put_lebe16(buf, 120, 1);

    // Volume sequence number [124..127] — both byte order u16 = 1
    put_lebe16(buf, 124, 1);

    // Logical block size [128..131] — both byte order u16 = 2048
    put_lebe16(buf, 128, 2048);

    // Path table size [132..139] — both byte order u32
    uint32_t pt_size = calc_path_table_size(vd);
    put_lebe32(buf, 132, pt_size);

    // Location of occurrence of Type L Path Table [140..143]
    put_le32(buf, 140, 18);
    // Optional L path table [144..147] = 0 (already zero)

    // Location of occurrence of Type M Path Table [148..151]
    put_be32(buf, 148, 19);
    // Optional M path table [152..155] = 0 (already zero)

    // Root directory record [156..189] — 34 bytes
    // rec_len=34, ext_attr=0, LBA=20 (both BE), size=2048 (both), date=7 zeros,
    // flags=0x02 (directory), unit=0, interleave=0, vol_seq=1, id_len=1, id=0x00
    uint8_t *rdr = buf + 156;
    rdr[0] = 34;               // Directory record length
    rdr[1] = 0;                // Extended attribute record length
    put_le32(rdr, 2, 20);      // Location of extent (LE) = LBA 20
    put_be32(rdr, 6, 20);      // Location of extent (BE)
    put_le32(rdr, 10, 2048);   // Data length (LE)
    put_be32(rdr, 14, 2048);   // Data length (BE)
    // rdr[18..24] = 0 (date/time, already zero)
    rdr[25] = 0x02;            // Flags: directory
    rdr[26] = 0;               // File unit size
    rdr[27] = 0;               // Interleave gap
    put_lebe16(rdr, 28, 1);    // Volume sequence number
    rdr[32] = 1;               // File identifier length
    rdr[33] = 0x00;            // File identifier (root = \x00)

    // File structure version [883]
    buf[883] = 0x01;
}

// ---------------------------------------------------------------------------
// Build the Volume Descriptor Set Terminator at LBA 17
// ---------------------------------------------------------------------------
static void build_vdst(uint8_t *buf) {
    memset(buf, 0, 2048);
    buf[0] = 0xFF;             // Volume Descriptor Type: Set Terminator
    memcpy(buf + 1, "CD001", 5);
    buf[6] = 0x01;
}

// ---------------------------------------------------------------------------
// Build path table (LBA 18 = little-endian, LBA 19 = big-endian)
// ---------------------------------------------------------------------------
static void build_path_table(const vdisc_t *vd, uint8_t *buf, bool big_endian) {
    memset(buf, 0, 2048);

    // Map entry_idx -> path_table_number (1-based)
    uint16_t pt_num[VDISC_MAX_ENTRIES];
    memset(pt_num, 0, sizeof(pt_num));

    int offset = 0;
    uint16_t pt_counter = 1;

    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (!vd->table[i].is_dir) continue;

        pt_num[i] = pt_counter++;

        uint8_t id_buf[32];
        int id_len;
        if (i == 0) {
            id_buf[0] = 0x00;
            id_len = 1;
        } else {
            id_len = (int)strlen(vd->table[i].name);
            if (id_len > 31) id_len = 31;
            memcpy(id_buf, vd->table[i].name, (size_t)id_len);
        }

        uint16_t parent_num = (i == 0) ? 1 : pt_num[vd->table[i].parent_idx];
        uint32_t dir_lba    = vd->table[i].lba;

        int entry_sz = 8 + id_len + (id_len & 1);
        if (offset + entry_sz > 2048) break;

        buf[offset + 0] = (uint8_t)id_len;
        buf[offset + 1] = 0;  // Extended attribute record length

        if (big_endian) {
            put_be32(buf, (uint32_t)(offset + 2), dir_lba);
            put_be16(buf, (uint32_t)(offset + 6), parent_num);
        } else {
            put_le32(buf, (uint32_t)(offset + 2), dir_lba);
            put_le16(buf, (uint32_t)(offset + 6), parent_num);
        }

        memcpy(buf + offset + 8, id_buf, (size_t)id_len);
        // Pad to even if id_len is odd
        if (id_len & 1) {
            buf[offset + 8 + id_len] = 0;
        }

        offset += entry_sz;
    }
}

// ---------------------------------------------------------------------------
// Build directory sector for one directory
// ---------------------------------------------------------------------------
static void build_dir_sector(const vdisc_t *vd, uint32_t dir_idx, uint8_t *buf) {
    memset(buf, 0, 2048);
    int offset = 0;

    // Self-reference "." entry
    uint8_t dot_id = 0x00;
    int n = write_dir_record(buf, offset,
                             vd->table[dir_idx].lba, 2048,
                             0x02, &dot_id, 1);
    if (n > 0) offset += n;

    // Parent ".." entry
    uint32_t parent_lba;
    if (dir_idx == 0) {
        parent_lba = 20;  // root's parent is itself (LBA 20)
    } else {
        parent_lba = vd->table[vd->table[dir_idx].parent_idx].lba;
    }
    uint8_t dotdot_id = 0x01;
    n = write_dir_record(buf, offset,
                         parent_lba, 2048,
                         0x02, &dotdot_id, 1);
    if (n > 0) offset += n;

    // Children
    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (vd->table[i].parent_idx != (uint16_t)dir_idx) continue;
        if (i == 0) continue;  // root is never a child

        if (vd->table[i].is_dir) {
            // Directory: identifier is just the name, flags=0x02
            const char *name = vd->table[i].name;
            n = write_dir_record(buf, offset,
                                 vd->table[i].lba, 2048,
                                 0x02,
                                 (const uint8_t *)name, (int)strlen(name));
        } else {
            // File: identifier is "NAME;1", flags=0x00
            char id_with_ver[VDISC_NAME_LEN + 3];  // name + ";1" + NUL
            size_t nlen = strlen(vd->table[i].name);
            if (nlen > VDISC_NAME_LEN - 1) nlen = VDISC_NAME_LEN - 1;
            memcpy(id_with_ver, vd->table[i].name, nlen);
            id_with_ver[nlen]     = ';';
            id_with_ver[nlen + 1] = '1';
            id_with_ver[nlen + 2] = '\0';
            n = write_dir_record(buf, offset,
                                 vd->table[i].lba, vd->table[i].size,
                                 0x00,
                                 (const uint8_t *)id_with_ver, (int)(nlen + 2));
        }
        if (n > 0) offset += n;
        // If n==0 the record doesn't fit; stop adding entries
        if (n == 0) break;
    }
}

// ---------------------------------------------------------------------------
// Read file data sector
// ---------------------------------------------------------------------------
static void read_file_sector(const vdisc_t *vd, uint32_t entry_idx,
                              uint32_t sector_offset, uint8_t *buf) {
    char path[MAX_PATH_LEN];
    entry_path(vd, entry_idx, path, sizeof(path));

    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_READ);
    if (fr != FR_OK) {
        return;  // buf already zeroed by caller
    }

    FSIZE_t file_off = (FSIZE_t)sector_offset * 2048u;
    f_lseek(&fp, file_off);

    UINT br = 0;
    f_read(&fp, buf, 2048, &br);
    // Zero-pad remainder (already done by caller's memset before this call,
    // but only if br < 2048 the remaining bytes beyond br are already zero)

    f_close(&fp);
}

// ---------------------------------------------------------------------------
// vdisc_mount — Phase 1: BFS scan + Phase 2: LBA assignment
// ---------------------------------------------------------------------------
bool vdisc_mount(vdisc_t *vd) {
    memset(vd, 0, sizeof(*vd));

    // --- Phase 1: BFS directory scan ---

    // Add root entry (index 0)
    vd->table[0].is_dir     = true;
    vd->table[0].parent_idx = 0;
    vd->table[0].depth      = 0;
    vd->table[0].name[0]    = '\0';
    vd->entry_count         = 1;

    bool truncated = false;

    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (!vd->table[i].is_dir) continue;

        char path[MAX_PATH_LEN];
        entry_path(vd, i, path, sizeof(path));

        DIR dir;
        FRESULT fr = f_opendir(&dir, path);
        if (fr != FR_OK) {
            if (i == 0) {
                return false;  // Can't open root partition
            }
            continue;
        }

        for (;;) {
            FILINFO fno;
            fr = f_readdir(&dir, &fno);
            if (fr != FR_OK || fno.fname[0] == '\0') break;  // End of directory

            // Skip current-dir and parent-dir entries
            if (fno.fname[0] == '.') continue;

            // Check entry limit
            if (vd->entry_count >= VDISC_MAX_ENTRIES) {
                printf("[VDISC] Entry limit (%d) reached, truncating\n",
                       VDISC_MAX_ENTRIES);
                truncated = true;
                break;
            }

            // Check depth limit
            uint8_t child_depth = vd->table[i].depth + 1u;
            if (child_depth > 8) {
                printf("[VDISC] Skipping entry at depth %u (max 8): %s\n",
                       (unsigned)child_depth, fno.fname);
                continue;
            }

            // Add entry
            uint32_t ei = vd->entry_count;
            bool is_dir = (fno.fattrib & AM_DIR) != 0;

            sanitise_name(vd->table[ei].name, fno.fname);
            vd->table[ei].is_dir     = is_dir;
            vd->table[ei].parent_idx = (uint16_t)i;
            vd->table[ei].size       = is_dir ? 0u : fno.fsize;
            vd->table[ei].depth      = child_depth;

            vd->entry_count++;
        }

        f_closedir(&dir);

        if (truncated) return false;
    }

    // --- Phase 2: LBA assignment ---

    // Assign LBAs to directories (starting at LBA 20)
    uint32_t dir_lba = 20;
    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (vd->table[i].is_dir) {
            vd->table[i].lba = dir_lba++;
        }
    }

    vd->dir_data_lba_start  = 20;
    vd->file_data_lba_start = dir_lba;

    // Assign LBAs to files (packed after all directories)
    uint32_t file_lba = dir_lba;
    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (!vd->table[i].is_dir) {
            vd->table[i].lba  = file_lba;
            file_lba         += (vd->table[i].size + 2047u) / 2048u;
        }
    }

    vd->total_sectors = file_lba;

    // Count dirs and files for the log message
    uint32_t n_dirs  = 0;
    uint32_t n_files = 0;
    for (uint32_t i = 0; i < vd->entry_count; i++) {
        if (vd->table[i].is_dir) n_dirs++; else n_files++;
    }

    printf("[VDISC] Mounted %u entries (%u dirs, %u files), %u sectors\n",
           vd->entry_count, n_dirs, n_files, vd->total_sectors);
    return true;
}

// ---------------------------------------------------------------------------
// vdisc_read_sector — Dispatch to appropriate sector builder
// ---------------------------------------------------------------------------
void vdisc_read_sector(const vdisc_t *vd, uint32_t lba, uint8_t *buf2048) {
    memset(buf2048, 0, 2048);

    if (lba < 16) return;  // System area — zeros

    if (lba == 16) { build_pvd(vd, buf2048);                           return; }
    if (lba == 17) { build_vdst(buf2048);                               return; }
    if (lba == 18) { build_path_table(vd, buf2048, false);              return; }
    if (lba == 19) { build_path_table(vd, buf2048, true);               return; }

    // Directory sectors
    if (lba >= vd->dir_data_lba_start && lba < vd->file_data_lba_start) {
        for (uint32_t i = 0; i < vd->entry_count; i++) {
            if (vd->table[i].is_dir && vd->table[i].lba == lba) {
                build_dir_sector(vd, i, buf2048);
                return;
            }
        }
        return;  // Unmapped dir LBA — zeros
    }

    // File data sectors
    if (lba >= vd->file_data_lba_start && lba < vd->total_sectors) {
        for (uint32_t i = 0; i < vd->entry_count; i++) {
            if (vd->table[i].is_dir) continue;
            uint32_t file_sectors = (vd->table[i].size + 2047u) / 2048u;
            if (lba >= vd->table[i].lba && lba < vd->table[i].lba + file_sectors) {
                read_file_sector(vd, i, lba - vd->table[i].lba, buf2048);
                return;
            }
        }
        return;  // Unmapped file LBA — zeros
    }
    // Beyond total_sectors — zeros (already done)
}
