// =============================================================================
// test_virtual_disc.cpp — Unit tests for virtual_disc.c (ISO 9660 synthesiser)
// =============================================================================
#include <CppUTest/TestHarness.h>
#include <stdint.h>
#include <string.h>
extern "C" {
#include "virtual_disc.h"
#include "disc_image.h"
#include "vdisc_sim.h"
}

// ---------------------------------------------------------------------------
// Helper: read LE u32 from a 4-byte array offset
// ---------------------------------------------------------------------------
static uint32_t read_le32(const uint8_t *buf, int off) {
    return  (uint32_t)buf[off]
          | ((uint32_t)buf[off+1] <<  8)
          | ((uint32_t)buf[off+2] << 16)
          | ((uint32_t)buf[off+3] << 24);
}

static uint32_t read_be32(const uint8_t *buf, int off) {
    return  ((uint32_t)buf[off]   << 24)
          | ((uint32_t)buf[off+1] << 16)
          | ((uint32_t)buf[off+2] <<  8)
          |  (uint32_t)buf[off+3];
}

// ============================================================================
// VdiscMount — directory scan and entry counting
// ============================================================================
TEST_GROUP(VdiscMount) {
    vdisc_t vd;
    void setup() { vdisc_sim_reset(); }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscMount, MountEmptyDir) {
    vdisc_sim_register_dir("1:/");
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    CHECK_EQUAL(1u, vd.entry_count);  // Only root
    CHECK(vd.total_sectors >= 21u);   // At least system area + PVD + VDST + 2 path tables + root dir
}

TEST(VdiscMount, MountSingleFile) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t data[100] = {0};
    vdisc_sim_register_file("1:/README.TXT", data, 100);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    CHECK_EQUAL(2u, vd.entry_count);
    // Find the file entry
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            CHECK_EQUAL(100u, vd.table[i].size);
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST(VdiscMount, MountNestedSubdirs) {
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/SUB");
    vdisc_sim_register_dir("1:/SUB/NESTED");
    static const uint8_t data[50] = {0};
    vdisc_sim_register_file("1:/SUB/NESTED/FILE.TXT", data, 50);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    CHECK_EQUAL(4u, vd.entry_count);
    // All dirs should have distinct, non-overlapping LBAs
    uint32_t lbas[4];
    int ni = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (vd.table[i].is_dir) lbas[ni++] = vd.table[i].lba;
    }
    // Check no duplicate LBAs among dirs
    for (int a = 0; a < ni; a++) {
        for (int b = a+1; b < ni; b++) {
            CHECK(lbas[a] != lbas[b]);
        }
    }
}

TEST(VdiscMount, MountExceedMaxEntries) {
    // Root + 512 files = 513 entries needed -> should fail
    vdisc_sim_register_dir("1:/");
    static const uint8_t dummy[1] = {0};
    char fname[32];
    for (int i = 0; i < 512; i++) {
        snprintf(fname, sizeof(fname), "1:/FILE_%03d.TXT", i);
        vdisc_sim_register_file(fname, dummy, 1);
    }
    bool ok = vdisc_mount(&vd);
    CHECK(!ok);  // Should fail because 513 entries > VDISC_MAX_ENTRIES=512
}

TEST(VdiscMount, MountExceedMaxDepth) {
    // 9-level deep chain: root/a/b/c/d/e/f/g/h/i — depth 9 should be skipped
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/A");
    vdisc_sim_register_dir("1:/A/B");
    vdisc_sim_register_dir("1:/A/B/C");
    vdisc_sim_register_dir("1:/A/B/C/D");
    vdisc_sim_register_dir("1:/A/B/C/D/E");
    vdisc_sim_register_dir("1:/A/B/C/D/E/F");
    vdisc_sim_register_dir("1:/A/B/C/D/E/F/G");
    vdisc_sim_register_dir("1:/A/B/C/D/E/F/G/H");
    // depth 9 = 10th level (root is 0): should be skipped
    vdisc_sim_register_dir("1:/A/B/C/D/E/F/G/H/I");
    static const uint8_t d[1] = {0};
    vdisc_sim_register_file("1:/A/B/C/D/E/F/G/H/I/DEEP.TXT", d, 1);
    bool ok = vdisc_mount(&vd);
    // Mount should succeed; the depth-9 dir and file should be silently skipped
    CHECK(ok);
    // The DEEP.TXT file (at depth 10) should NOT be in the table
    bool found_deep = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir && strcmp(vd.table[i].name, "DEEP.TXT") == 0) {
            found_deep = true;
        }
    }
    CHECK(!found_deep);
}

TEST(VdiscMount, MountFilenameUppercased) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t d[1] = {0};
    vdisc_sim_register_file("1:/readme.txt", d, 1);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            STRCMP_EQUAL("README.TXT", vd.table[i].name);
            found = true;
        }
    }
    CHECK(found);
}

TEST(VdiscMount, MountFilenameIllegalCharsReplaced) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t d[1] = {0};
    vdisc_sim_register_file("1:/my file!.sh", d, 1);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            STRCMP_EQUAL("MY_FILE_.SH", vd.table[i].name);
            found = true;
        }
    }
    CHECK(found);
}

// ============================================================================
// VdiscLbaLayout — LBA assignment correctness
// ============================================================================
TEST_GROUP(VdiscLbaLayout) {
    vdisc_t vd;
    disc_image_t disc;
    uint8_t buf[2048];

    void setup() {
        vdisc_sim_reset();
        // Simple tree: root, subdir, two files
        vdisc_sim_register_dir("1:/");
        vdisc_sim_register_dir("1:/SUBDIR");
        static const uint8_t d1[1024] = {0};
        static const uint8_t d2[512]  = {0};
        vdisc_sim_register_file("1:/FILE.TXT",      d1, 1024);
        vdisc_sim_register_file("1:/SUBDIR/INNER.TXT", d2, 512);
        vdisc_mount(&vd);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscLbaLayout, SystemAreaLba0To15) {
    for (uint32_t lba = 0; lba < 16; lba++) {
        memset(buf, 0xAA, sizeof(buf));
        vdisc_read_sector(&vd, lba, buf);
        for (int i = 0; i < 2048; i++) {
            if (buf[i] != 0) {
                FAIL("System area LBA should be all zeros");
            }
        }
    }
}

TEST(VdiscLbaLayout, PvdAtLba16) {
    vdisc_read_sector(&vd, 16, buf);
    CHECK_EQUAL(0x01, buf[0]);
    CHECK(memcmp(buf + 1, "CD001", 5) == 0);
}

TEST(VdiscLbaLayout, VdstAtLba17) {
    vdisc_read_sector(&vd, 17, buf);
    CHECK_EQUAL((uint8_t)0xFF, buf[0]);
}

TEST(VdiscLbaLayout, PathTableLAtLba18) {
    vdisc_read_sector(&vd, 18, buf);
    // First entry: id_len byte should be 1 (root), id should be 0x00
    CHECK_EQUAL(1, buf[0]);   // id_len = 1
    CHECK_EQUAL(0x00, buf[8]); // id = 0x00 (root)
}

TEST(VdiscLbaLayout, PathTableMAtLba19) {
    vdisc_read_sector(&vd, 19, buf);
    // Same structure but big-endian
    CHECK_EQUAL(1, buf[0]);   // id_len = 1
    CHECK_EQUAL(0x00, buf[8]); // id = 0x00 (root)
}

TEST(VdiscLbaLayout, RootDirAtLba20) {
    CHECK_EQUAL(20u, vd.dir_data_lba_start);
    // Root entry (index 0) should have lba == 20
    CHECK_EQUAL(20u, vd.table[0].lba);
}

TEST(VdiscLbaLayout, FileLbasAfterAllDirLbas) {
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            CHECK(vd.table[i].lba >= vd.file_data_lba_start);
        }
    }
}

TEST(VdiscLbaLayout, LbasNonOverlapping) {
    // For all pairs of entries, their LBA ranges shouldn't overlap
    for (uint32_t a = 0; a < vd.entry_count; a++) {
        uint32_t a_start = vd.table[a].lba;
        uint32_t a_end   = vd.table[a].is_dir
                         ? a_start + 1
                         : a_start + (vd.table[a].size + 2047u) / 2048u;
        if (!vd.table[a].is_dir && vd.table[a].size == 0) continue;  // zero-size file: skip

        for (uint32_t b = a + 1; b < vd.entry_count; b++) {
            uint32_t b_start = vd.table[b].lba;
            uint32_t b_end   = vd.table[b].is_dir
                             ? b_start + 1
                             : b_start + (vd.table[b].size + 2047u) / 2048u;
            if (!vd.table[b].is_dir && vd.table[b].size == 0) continue;

            // Check for overlap: [a_start, a_end) ∩ [b_start, b_end)
            bool overlap = (a_start < b_end) && (b_start < a_end);
            if (overlap) {
                FAIL("Two entries have overlapping LBA ranges");
            }
        }
    }
}

// ============================================================================
// VdiscPvd — Primary Volume Descriptor contents
// ============================================================================
TEST_GROUP(VdiscPvd) {
    vdisc_t vd;
    uint8_t buf[2048];

    void setup() {
        vdisc_sim_reset();
        vdisc_sim_register_dir("1:/");
        static const uint8_t d[512] = {0};
        vdisc_sim_register_file("1:/TEST.BIN", d, 512);
        vdisc_mount(&vd);
        vdisc_read_sector(&vd, 16, buf);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscPvd, PvdMagicBytes) {
    CHECK(memcmp(buf + 1, "CD001", 5) == 0);
}

TEST(VdiscPvd, PvdTypeByte) {
    CHECK_EQUAL(0x01, buf[0]);
}

TEST(VdiscPvd, PvdVersionByte) {
    CHECK_EQUAL(0x01, buf[6]);
}

TEST(VdiscPvd, PvdLogicalBlockSize) {
    // LE: buf[128]=0x00, buf[129]=0x08 = 2048
    CHECK_EQUAL(0x00, buf[128]);
    CHECK_EQUAL(0x08, buf[129]);
    // BE: buf[130]=0x08, buf[131]=0x00
    CHECK_EQUAL(0x08, buf[130]);
    CHECK_EQUAL(0x00, buf[131]);
}

TEST(VdiscPvd, PvdVolumeSizeMatchesTotalSectors) {
    uint32_t vol_size = read_le32(buf, 80);
    CHECK_EQUAL(vd.total_sectors, vol_size);
}

TEST(VdiscPvd, PvdRootDirRecordLba) {
    // Root dir record is at buf[156]; LBA starts at byte 2 (LE u32)
    uint32_t root_lba = read_le32(buf, 158);
    CHECK_EQUAL(20u, root_lba);
}

TEST(VdiscPvd, PvdVolumeIdentifier) {
    // buf[40..47] == "NEBULA32", buf[48] == ' '
    CHECK(memcmp(buf + 40, "NEBULA32", 8) == 0);
    CHECK_EQUAL(0x20, buf[48]);
}

// ============================================================================
// VdiscVdst — Volume Descriptor Set Terminator
// ============================================================================
TEST_GROUP(VdiscVdst) {
    vdisc_t vd;
    uint8_t buf[2048];

    void setup() {
        vdisc_sim_reset();
        vdisc_sim_register_dir("1:/");
        vdisc_mount(&vd);
        vdisc_read_sector(&vd, 17, buf);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscVdst, VdstTypeByte) {
    CHECK_EQUAL((uint8_t)0xFF, buf[0]);
}

TEST(VdiscVdst, VdstMagicBytes) {
    CHECK(memcmp(buf + 1, "CD001", 5) == 0);
}

// ============================================================================
// VdiscPathTable — path table correctness
// ============================================================================
TEST_GROUP(VdiscPathTable) {
    vdisc_t vd;
    uint8_t lbuf[2048];  // L-path (LE)
    uint8_t mbuf[2048];  // M-path (BE)

    void setup() {
        vdisc_sim_reset();
        vdisc_sim_register_dir("1:/");
        vdisc_mount(&vd);
        vdisc_read_sector(&vd, 18, lbuf);
        vdisc_read_sector(&vd, 19, mbuf);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscPathTable, PathTableLRootEntryLba) {
    // LBA at bytes 2..5 (LE u32) should be 20
    uint32_t lba = read_le32(lbuf, 2);
    CHECK_EQUAL(20u, lba);
}

TEST(VdiscPathTable, PathTableMRootEntryLba) {
    // LBA at bytes 2..5 (BE u32) should be 20
    uint32_t lba = read_be32(mbuf, 2);
    CHECK_EQUAL(20u, lba);
}

TEST(VdiscPathTable, PathTableRootIdentifier) {
    CHECK_EQUAL(1, lbuf[0]);     // id_len = 1
    CHECK_EQUAL(0x00, lbuf[8]);  // id = 0x00 (root)
}

TEST(VdiscPathTable, PathTableRootParentNumber) {
    // Parent dir number at bytes 6..7 (LE u16) = 1
    uint16_t parent = (uint16_t)(lbuf[6] | (lbuf[7] << 8));
    CHECK_EQUAL(1u, (unsigned)parent);
}

TEST(VdiscPathTable, PathTableSubdirPresent) {
    vdisc_sim_reset();
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/SUBDIR");
    vdisc_mount(&vd);
    vdisc_read_sector(&vd, 18, lbuf);

    // First entry: root (id_len=1, entry_size = 8+1+1=10)
    int offset = 0;
    int id_len0 = lbuf[offset + 0];
    int entry0_sz = 8 + id_len0 + (id_len0 & 1);
    offset += entry0_sz;

    // Second entry should have id="SUBDIR"
    int id_len1 = lbuf[offset + 0];
    CHECK_EQUAL(6, id_len1);
    CHECK(memcmp(lbuf + offset + 8, "SUBDIR", 6) == 0);
}

// ============================================================================
// VdiscDirSector — directory record contents
// ============================================================================
TEST_GROUP(VdiscDirSector) {
    vdisc_t vd;
    uint8_t buf[2048];

    void setup() {
        vdisc_sim_reset();
        vdisc_sim_register_dir("1:/");
        vdisc_sim_register_dir("1:/SUBDIR");
        static const uint8_t f1[1024] = {0};
        static const uint8_t f2[512]  = {0};
        vdisc_sim_register_file("1:/FILE.TXT",          f1, 1024);
        vdisc_sim_register_file("1:/SUBDIR/INNER.TXT",  f2, 512);
        vdisc_mount(&vd);
        // Read root dir sector (LBA 20)
        vdisc_read_sector(&vd, 20, buf);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscDirSector, RootDirSelfRefFirst) {
    // First record: rec_len at buf[0], flags at buf[25]=0x02, id at buf[33]=0x00
    CHECK_EQUAL(34, buf[0]);   // rec_len = 34 (33 + 1, rounded to even)
    CHECK_EQUAL(0x02, buf[25]);
    CHECK_EQUAL(0x00, buf[33]);
}

TEST(VdiscDirSector, RootDirParentRefSecond) {
    int offset = buf[0];  // Skip first record
    CHECK_EQUAL(0x02, buf[offset + 25]);
    CHECK_EQUAL(0x01, buf[offset + 33]);
}

TEST(VdiscDirSector, FileEntryFlagsZero) {
    // Scan root dir sector for FILE.TXT
    int offset = 0;
    bool found = false;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        int id_len = buf[offset + 32];
        if (id_len >= 8 && memcmp(buf + offset + 33, "FILE.TXT", 8) == 0) {
            CHECK_EQUAL(0x00, buf[offset + 25]);  // flags=0x00 for file
            found = true;
            break;
        }
        offset += rec_len;
    }
    CHECK(found);
}

TEST(VdiscDirSector, DirEntryFlagsTwo) {
    // Scan root dir sector for SUBDIR
    int offset = 0;
    bool found = false;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        int id_len = buf[offset + 32];
        if (id_len == 6 && memcmp(buf + offset + 33, "SUBDIR", 6) == 0) {
            CHECK_EQUAL(0x02, buf[offset + 25]);  // flags=0x02 for dir
            found = true;
            break;
        }
        offset += rec_len;
    }
    CHECK(found);
}

TEST(VdiscDirSector, FileEntryVersionSuffix) {
    // FILE.TXT should appear as "FILE.TXT;1"
    int offset = 0;
    bool found = false;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        int id_len = buf[offset + 32];
        if (id_len == 10 && memcmp(buf + offset + 33, "FILE.TXT;1", 10) == 0) {
            found = true;
            break;
        }
        offset += rec_len;
    }
    CHECK(found);
}

TEST(VdiscDirSector, DirEntryNoVersionSuffix) {
    // SUBDIR should NOT have ";1" in its identifier
    int offset = 0;
    bool found_subdir = false;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        int id_len = buf[offset + 32];
        if (id_len == 6 && memcmp(buf + offset + 33, "SUBDIR", 6) == 0) {
            found_subdir = true;
            // Verify no ';' in identifier
            for (int k = 0; k < id_len; k++) {
                CHECK(buf[offset + 33 + k] != ';');
            }
            break;
        }
        offset += rec_len;
    }
    CHECK(found_subdir);
}

TEST(VdiscDirSector, FileEntrySizeMatchesActual) {
    // FILE.TXT has size 1024; check the data length field
    int offset = 0;
    bool found = false;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        int id_len = buf[offset + 32];
        if (id_len == 10 && memcmp(buf + offset + 33, "FILE.TXT;1", 10) == 0) {
            uint32_t sz = read_le32(buf, offset + 10);
            CHECK_EQUAL(1024u, sz);
            found = true;
            break;
        }
        offset += rec_len;
    }
    CHECK(found);
}

TEST(VdiscDirSector, RecordLengthIsEven) {
    // All records should have even rec_len
    int offset = 0;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        CHECK_EQUAL(0, rec_len & 1);
        offset += rec_len;
    }
}

// ============================================================================
// VdiscFileData — file content delivery
// ============================================================================
TEST_GROUP(VdiscFileData) {
    vdisc_t vd;
    uint8_t buf[2048];

    void setup() { vdisc_sim_reset(); }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscFileData, FirstSectorOfFileMatchesSdContent) {
    static uint8_t pattern[2048];
    for (int i = 0; i < 2048; i++) pattern[i] = (uint8_t)(i & 0xFF);
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/TEST.BIN", pattern, 2048);
    vdisc_mount(&vd);

    // Find file entry
    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) { file_lba = vd.table[i].lba; break; }
    }
    vdisc_read_sector(&vd, file_lba, buf);
    CHECK(memcmp(buf, pattern, 2048) == 0);
}

TEST(VdiscFileData, SecondSectorOfLargeFile) {
    static uint8_t pattern[4096];
    for (int i = 0; i < 4096; i++) pattern[i] = (uint8_t)(i & 0xFF);
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/BIG.BIN", pattern, 4096);
    vdisc_mount(&vd);

    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) { file_lba = vd.table[i].lba; break; }
    }
    vdisc_read_sector(&vd, file_lba + 1, buf);
    CHECK(memcmp(buf, pattern + 2048, 2048) == 0);
}

TEST(VdiscFileData, LastSectorZeroPadded) {
    static uint8_t pattern[100];
    for (int i = 0; i < 100; i++) pattern[i] = 0xCC;
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/SMALL.TXT", pattern, 100);
    vdisc_mount(&vd);

    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) { file_lba = vd.table[i].lba; break; }
    }
    vdisc_read_sector(&vd, file_lba, buf);
    // First 100 bytes should be the pattern
    CHECK(memcmp(buf, pattern, 100) == 0);
    // Rest should be zero
    for (int i = 100; i < 2048; i++) {
        CHECK_EQUAL(0, buf[i]);
    }
}

TEST(VdiscFileData, SystemAreaReturnsZeros) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);
    vdisc_read_sector(&vd, 0, buf);
    for (int i = 0; i < 2048; i++) {
        CHECK_EQUAL(0, buf[i]);
    }
}

TEST(VdiscFileData, UnmappedLbaBeyondTotalReturnsZero) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);
    memset(buf, 0xAA, sizeof(buf));
    vdisc_read_sector(&vd, vd.total_sectors + 5, buf);
    for (int i = 0; i < 2048; i++) {
        CHECK_EQUAL(0, buf[i]);
    }
}

TEST(VdiscFileData, FileReadRespectsParentPath) {
    static uint8_t pattern[200];
    for (int i = 0; i < 200; i++) pattern[i] = (uint8_t)(0xAB);
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/SUBDIR");
    vdisc_sim_register_file("1:/SUBDIR/DEEP.TXT", pattern, 200);
    vdisc_mount(&vd);

    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir && strcmp(vd.table[i].name, "DEEP.TXT") == 0) {
            file_lba = vd.table[i].lba;
            break;
        }
    }
    CHECK(file_lba > 0);
    vdisc_read_sector(&vd, file_lba, buf);
    for (int i = 0; i < 200; i++) {
        CHECK_EQUAL(0xAB, buf[i]);
    }
    for (int i = 200; i < 2048; i++) {
        CHECK_EQUAL(0, buf[i]);
    }
}

// ============================================================================
// VdiscToc — disc_open_vdir TOC population
// ============================================================================
TEST_GROUP(VdiscToc) {
    vdisc_t vd;
    disc_image_t disc;

    void setup() {
        vdisc_sim_reset();
        vdisc_sim_register_dir("1:/");
        static const uint8_t d[512] = {0};
        vdisc_sim_register_file("1:/DATA.BIN", d, 512);
        disc_open_vdir(&disc, &vd);
    }
    void teardown() { vdisc_sim_reset(); }
};

TEST(VdiscToc, TocHasOneDataTrack) {
    CHECK_EQUAL(1, disc.last_track);
}

TEST(VdiscToc, TocTrack1IsData) {
    CHECK_EQUAL(TRACK_TYPE_DATA, disc.tracks[0].type);
}

TEST(VdiscToc, TocTrack1StartsAtLba0) {
    CHECK_EQUAL(0u, disc.tracks[0].start_lba);
}

TEST(VdiscToc, TocLeadOutLbaMatchesTotalSectors) {
    CHECK_EQUAL(vd.total_sectors, disc.total_sectors);
}


// ============================================================================
// VdiscIntegration — disc_open_vdir full round-trip
// ============================================================================
TEST_GROUP(VdiscIntegration) {
    vdisc_t vd;
    disc_image_t disc;
    uint8_t buf[2352];

    void setup() {
        vdisc_sim_reset();
    }
    void teardown() {
        vdisc_sim_reset();
    }
};

TEST(VdiscIntegration, DiscOpenVdirSetsFormat) {
    vdisc_sim_register_dir("1:/");
    bool ok = disc_open_vdir(&disc, &vd);
    CHECK(ok);
    CHECK_EQUAL(DISC_FORMAT_VDIR, disc.format);
}

TEST(VdiscIntegration, DiscReadSectorAtLba16ReturnsPvd) {
    vdisc_sim_register_dir("1:/");
    disc_open_vdir(&disc, &vd);
    disc_read_sector(&disc, 16, buf, SECTOR_MODE_RAW);
    // In a raw 2352-byte sector the data payload starts at buf[16]
    // PVD type byte is at buf[16+0] = buf[16]
    // Magic "CD001" is at buf[17..21]
    CHECK_EQUAL(0x01, buf[16]);
    CHECK(memcmp(buf + 17, "CD001", 5) == 0);
}

TEST(VdiscIntegration, DiscReadSectorAtLba0ReturnsMode1) {
    vdisc_sim_register_dir("1:/");
    disc_open_vdir(&disc, &vd);
    disc_read_sector(&disc, 0, buf, SECTOR_MODE_RAW);
    // Sync pattern: 0x00, 0xFF*10, 0x00
    CHECK_EQUAL(0x00, buf[0]);
    for (int i = 1; i <= 10; i++) CHECK_EQUAL((uint8_t)0xFF, buf[i]);
    CHECK_EQUAL(0x00, buf[11]);
}

TEST(VdiscIntegration, DiscReadSectorFileDataRoundTrip) {
    static uint8_t pattern[2048];
    memset(pattern, 0xAB, sizeof(pattern));
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/DATA.BIN", pattern, 2048);
    disc_open_vdir(&disc, &vd);

    // Find file entry LBA
    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) { file_lba = vd.table[i].lba; break; }
    }
    disc_read_sector(&disc, file_lba, buf, SECTOR_MODE_RAW);
    // Data payload starts at buf[16] in a raw sector
    CHECK_EQUAL(0xAB, buf[16]);
}

TEST(VdiscIntegration, DiscCloseClearsVdiscPointer) {
    vdisc_sim_register_dir("1:/");
    disc_open_vdir(&disc, &vd);
    CHECK(disc.vdisc != NULL);
    disc_close(&disc);
    CHECK(disc.vdisc == NULL);
}

/* ============================================================================
 * Additional gap-filling tests
 * ============================================================================ */

/* Gap 1: sanitise_name() passes hyphen and comma through unchanged. */
TEST(VdiscMount, SanitiseName_HyphenAndCommaPreserved) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t d[1] = {0};
    vdisc_sim_register_file("1:/track-01,2.bin", d, 1);
    vdisc_mount(&vd);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            STRCMP_EQUAL("TRACK-01,2.BIN", vd.table[i].name);
            found = true;
        }
    }
    CHECK(found);
}

/* Gap 2: sanitise_name() stops at VDISC_NAME_LEN-1 = 31 output chars. */
TEST(VdiscMount, SanitiseName_LongNameTruncated) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t d[1] = {0};
    vdisc_sim_register_file("1:/ABCDEFGHIJKLMNOPQRSTUVWXYZ_EXTRA.TXT", d, 1);
    vdisc_mount(&vd);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            CHECK(strlen(vd.table[i].name) <= 31u);
            found = true;
        }
    }
    CHECK(found);
}

/* Gap 3: zero-size file mounts successfully and takes no file-data sectors. */
TEST(VdiscMount, ZeroSizeFile_MountsOk) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t dummy[1] = {0};
    vdisc_sim_register_file("1:/EMPTY.BIN", dummy, 0);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    /* 20 (system+headers+root) + 0 file sectors = 21 */
    LONGS_EQUAL(21, (long)vd.total_sectors);
}

/* Gap 4: PVD file structure version byte at offset 883 must be 0x01. */
TEST(VdiscPvd, PvdFileStructureVersionByte) {
    CHECK_EQUAL(0x01, buf[883]);
}

/* Gap 5: PVD L-path table location [140..143] = LE32(18). */
TEST(VdiscPvd, PvdLPathTableLocation) {
    CHECK_EQUAL(18u, read_le32(buf, 140));
}

/* Gap 6: PVD M-path table location [148..151] = BE32(19) via put_be32. */
TEST(VdiscPvd, PvdMPathTableLocation) {
    CHECK_EQUAL(19u, read_be32(buf, 148));
}

/* Gap 7: VDST version byte at offset 6 must be 0x01. */
TEST(VdiscVdst, VdstVersionByte) {
    CHECK_EQUAL(0x01, buf[6]);
}

/* Gap 8: L-path table subdirectory entry: parent number = 1 (root is #1). */
TEST(VdiscPathTable, PathTableSubdir_ParentNumber) {
    vdisc_sim_reset();
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/SUBDIR");
    vdisc_mount(&vd);
    vdisc_read_sector(&vd, 18, lbuf);

    int id_len0  = lbuf[0];
    int entry0sz = 8 + id_len0 + (id_len0 & 1);
    int off      = entry0sz;

    uint16_t parent_num = (uint16_t)(lbuf[off + 6] | (lbuf[off + 7] << 8));
    CHECK_EQUAL(1u, (unsigned)parent_num);
}

/* Gap 9: L-path table subdirectory LBA matches the entry table value. */
TEST(VdiscPathTable, PathTableSubdir_LbaMatchesEntry) {
    vdisc_sim_reset();
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/SUBDIR");
    vdisc_mount(&vd);
    vdisc_read_sector(&vd, 18, lbuf);

    uint32_t expected_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (vd.table[i].is_dir && strcmp(vd.table[i].name, "SUBDIR") == 0) {
            expected_lba = vd.table[i].lba;
            break;
        }
    }
    CHECK(expected_lba != 0u);

    int id_len0  = lbuf[0];
    int entry0sz = 8 + id_len0 + (id_len0 & 1);
    int off      = entry0sz;

    uint32_t pt_lba = read_le32(lbuf, off + 2);
    CHECK_EQUAL(expected_lba, pt_lba);
}

/* Gap 10: root directory's ".." record (second record) must point to LBA 20. */
TEST(VdiscDirSector, RootDotDot_LbaIs20) {
    int off          = buf[0];   /* skip first record (self-ref, rec_len=34) */
    uint32_t par_lba = read_le32(buf, off + 2);
    CHECK_EQUAL(20u, par_lba);
}

// =============================================================================
// VdiscExtended — supplemental tests (tests.md Section 4, Tests 31-40)
// =============================================================================

TEST_GROUP(VdiscExtended) {
    vdisc_t  vd;
    uint8_t  buf[2048];
    void setup()    { vdisc_sim_reset(); memset(buf, 0xFF, sizeof(buf)); }
    void teardown() { vdisc_sim_reset(); }
};

// Test 31: Nested tree deeper than 8 levels must not crash; entries past
// depth 8 are skipped cleanly and the mount still succeeds.
TEST(VdiscExtended, Vdisc_MaxDirectoryDepth) {
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_dir("1:/L1");
    vdisc_sim_register_dir("1:/L1/L2");
    vdisc_sim_register_dir("1:/L1/L2/L3");
    vdisc_sim_register_dir("1:/L1/L2/L3/L4");
    vdisc_sim_register_dir("1:/L1/L2/L3/L4/L5");
    vdisc_sim_register_dir("1:/L1/L2/L3/L4/L5/L6");
    vdisc_sim_register_dir("1:/L1/L2/L3/L4/L5/L6/L7");
    vdisc_sim_register_dir("1:/L1/L2/L3/L4/L5/L6/L7/L8");
    // Depth 9 — must be skipped, not crash
    vdisc_sim_register_dir("1:/L1/L2/L3/L4/L5/L6/L7/L8/L9TOOMANY");
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    // Max depth 8 means the L8 directory is included; L9TOOMANY is skipped.
    // entry_count must be ≥ 9 (root + L1..L8) and sane (< VDISC_MAX_ENTRIES).
    CHECK(vd.entry_count >= 9u);
    CHECK(vd.total_sectors > 20u);
}

// Test 32: Two files whose names are identical after sanitisation and
// truncation both appear in the entry table (no silent drop, no crash).
// virtual_disc.c does NOT add numeric suffixes; same-name entries coexist.
TEST(VdiscExtended, Vdisc_FilenameTruncationAndClash) {
    static const uint8_t data[50] = {0};
    vdisc_sim_register_dir("1:/");
    // Both files sanitise to the same 8.3 name "FILE.TXT" (case-fold only)
    vdisc_sim_register_file("1:/file.txt",  data, 50);
    vdisc_sim_register_file("1:/FILE.TXT",  data, 50);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    // Both entries must be present (no silent deduplication)
    int file_count = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) file_count++;
    }
    CHECK_EQUAL(2, file_count);
}

// Test 33: Zero-size file gets a valid directory entry with size=0.
TEST(VdiscExtended, Vdisc_ZeroSizeFileHandling) {
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/EMPTY.TXT", NULL, 0);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            CHECK_EQUAL(0u, vd.table[i].size);
            found = true;
        }
    }
    CHECK(found);
}

// Test 34: L-path table must store directory LBAs in LE32; M-path table in BE32.
TEST(VdiscExtended, Vdisc_PathTableEndianness) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);
    uint8_t l_buf[2048], m_buf[2048];
    vdisc_read_sector(&vd, 18, l_buf);   // L-path table
    vdisc_read_sector(&vd, 19, m_buf);   // M-path table

    // Root entry: LE LBA at [2..5], BE LBA at [2..5] in their respective tables
    uint32_t l_lba = read_le32(l_buf, 2);
    uint32_t m_lba = read_be32(m_buf, 2);
    CHECK_EQUAL(20u, l_lba);   // root dir at LBA 20 (LE)
    CHECK_EQUAL(20u, m_lba);   // root dir at LBA 20 (BE)
}

// Test 35: A directory with enough file records to nearly fill a 2048-byte
// sector must have the unused tail bytes zero-padded.  Entries that would
// overflow the sector boundary are skipped, not partially written.
TEST(VdiscExtended, Vdisc_DirectorySectorPadding) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t data[10] = {0};
    // Register 50 files — more than enough to fill a 2048-byte directory sector.
    // Each entry: "FILExx.BIN;1" = 12 chars (even) → rec_len = 33+12 = 45 (odd) → 46 bytes.
    // Self + parent = 34+34 = 68 bytes; (2048-68)/46 ≈ 43 entries fit.
    char path[32];
    for (int i = 0; i < 50; i++) {
        snprintf(path, sizeof(path), "1:/FILE%02d.BIN", i);
        vdisc_sim_register_file(path, data, 10);
    }
    bool ok = vdisc_mount(&vd);
    CHECK(ok);

    // Read the root directory sector (LBA 20)
    vdisc_read_sector(&vd, 20, buf);

    // The last byte of the sector must always be zero (memset before writing)
    CHECK_EQUAL(0, buf[2047]);

    // Walk records to find where entries end; beyond that point must be zeros
    int offset = 0;
    int last_end = 0;
    while (offset < 2048) {
        int rec_len = buf[offset];
        if (rec_len == 0) break;
        last_end = offset + rec_len;
        offset  += rec_len;
    }
    // All bytes from last_end to 2047 must be zero
    for (int i = last_end; i < 2048; i++) {
        CHECK_EQUAL(0, buf[i]);
    }
}

// Test 36: Reading any LBA in the system area [0..15] returns an all-zeros sector.
TEST(VdiscExtended, Vdisc_SystemAreaZeros) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);
    for (uint32_t lba = 0; lba < 16; lba++) {
        vdisc_read_sector(&vd, lba, buf);
        for (int i = 0; i < 2048; i++) {
            CHECK_EQUAL(0, buf[i]);
        }
    }
}

// Test 37: Filenames with illegal ISO 9660 characters are replaced with '_'.
TEST(VdiscExtended, Vdisc_IllegalCharSubstitution) {
    vdisc_sim_register_dir("1:/");
    static const uint8_t data[1] = {0};
    vdisc_sim_register_file("1:/my#file@2024.txt", data, 1);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);
    bool found = false;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) {
            // '#' and '@' must be replaced with '_'; letters uppercased
            const char *n = vd.table[i].name;
            CHECK(strstr(n, "#") == NULL);
            CHECK(strstr(n, "@") == NULL);
            CHECK(strstr(n, "_") != NULL);
            found = true;
        }
    }
    CHECK(found);
}

// Test 38: LBA 16 must contain a valid PVD with "CD001" magic, type=0x01, and
// volume identifier "NEBULA32".
TEST(VdiscExtended, Vdisc_PvdValidation) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);
    vdisc_read_sector(&vd, 16, buf);
    CHECK_EQUAL(0x01, buf[0]);
    CHECK_EQUAL(0, memcmp(buf + 1, "CD001", 5));
    CHECK_EQUAL(0x01, buf[6]);
    CHECK_EQUAL(0, memcmp(buf + 40, "NEBULA32", 8));
}

// Test 39: A file larger than 2048 bytes spans multiple consecutive LBAs.
TEST(VdiscExtended, Vdisc_LargeFileMultiSector) {
    static uint8_t large[3000];
    for (int i = 0; i < 3000; i++) large[i] = (uint8_t)(i & 0xFF);
    vdisc_sim_register_dir("1:/");
    vdisc_sim_register_file("1:/BIG.BIN", large, 3000);
    bool ok = vdisc_mount(&vd);
    CHECK(ok);

    uint32_t file_lba = 0;
    for (uint32_t i = 0; i < vd.entry_count; i++) {
        if (!vd.table[i].is_dir) { file_lba = vd.table[i].lba; break; }
    }
    CHECK(file_lba != 0u);

    // First sector: bytes 0-2047 of the file
    vdisc_read_sector(&vd, file_lba, buf);
    CHECK_EQUAL(large[0],    buf[0]);
    CHECK_EQUAL(large[2047], buf[2047]);

    // Second sector: bytes 2048-2999 of the file (+ zero-pad)
    vdisc_read_sector(&vd, file_lba + 1, buf);
    CHECK_EQUAL(large[2048], buf[0]);
    CHECK_EQUAL(large[2999], buf[951]);
    CHECK_EQUAL(0,           buf[952]);   // zero-padded beyond file end
}

// Test 40: disc_close clears vdisc pointer and resets the disc context.
TEST(VdiscExtended, Vdisc_IntegrationClose) {
    vdisc_sim_register_dir("1:/");
    vdisc_mount(&vd);

    disc_image_t disc;
    disc_open_vdir(&disc, &vd);
    CHECK(disc.vdisc != NULL);
    CHECK_EQUAL(DISC_FORMAT_VDIR, (int)disc.format);

    disc_close(&disc);
    CHECK(disc.vdisc == NULL);
}
