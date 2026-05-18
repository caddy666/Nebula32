# Virtual Directory Feature — Design Specification

## Goal

Allow a dedicated second SD card partition (≤ 650 MB) to appear to Akiko as
a Mode 1 CD-ROM data disc, without a pre-built image file.  The firmware
synthesises a valid ISO 9660 filesystem on-the-fly from the files on that
partition at mount time.

Use case: copy Amiga software, Workbench files, or homebrew tools onto
partition 2 from any desktop OS; the CD32 sees a standard data disc and can
read any file from it.  This works for *data access only* — existing CD32
game titles cannot be re-created this way because they require specific binary
layouts that a synthesised filesystem cannot reproduce.

### Why a dedicated partition rather than a subdirectory

The 650 MB CD capacity limit must be enforced somewhere.  A partition sized
to ≤ 650 MB at creation time makes this a one-time physical constraint —
the firmware never needs to measure content or reject an over-size tree.
It also gives the user a clean mental model: partition 1 holds disc images
and config (unlimited), partition 2 is the virtual CD slot (fixed size).

A raw ISO 9660 partition (pre-built with `mkisofs` on the desktop) was
considered as an alternative.  It was rejected because it requires desktop
ISO tooling for every content update — defeating the purpose of a feature
whose value is that files can be managed directly from a file manager.

---

## ISO 9660 Primer (relevant subset)

An ISO 9660 disc is a flat sequence of 2048-byte logical blocks.  The layout
is fixed at "burn" time and never changes:

```
LBA   0–15   System area       (32 KB, conventionally all-zero)
LBA  16      Primary Volume Descriptor  (PVD)
LBA  17      Volume Descriptor Set Terminator  (VDST)
LBA  18      L-Path Table  (little-endian 32-bit LBA addresses)
LBA  19      M-Path Table  (big-endian 32-bit LBA addresses)
LBA  20      Root directory record sector
LBA  21 …    Subdirectory record sectors  (one sector per directory)
LBA  20+D …  File data sectors  (ceil(file.size / 2048) per file)
```

`D` = total number of directories (including root).

### Key structures

**PVD** (2048 bytes, LBA 16):
- Byte 0: type = `0x01`
- Bytes 1–5: magic `"CD001"`
- Byte 6: version = `0x01`
- Bytes 40–71: volume identifier (32 bytes, space-padded) ← directory name
- Bytes 80–87: volume space size (LE u32 + BE u32) = total sector count
- Bytes 128–131: logical block size (LE+BE) = 2048
- Bytes 132–139: path table size (LE+BE)
- Bytes 140–143: L-path table LBA (LE u32)
- Bytes 148–151: M-path table LBA (BE u32)
- Bytes 156–189: root directory record (34 bytes, embedded in PVD)

**Directory record** (variable length, minimum 34 bytes):
```
[0]     record length
[1]     extended attribute length (0)
[2..9]  extent LBA (LE u32 + BE u32)
[10..17] data length in bytes (LE u32 + BE u32)
[18..24] recording date-time (7 bytes, can be zero)
[25]    file flags: 0x00 = file, 0x02 = directory
[26..27] file unit size + interleave = 0
[28..31] volume sequence number (LE+BE = 1)
[32]    file identifier length N
[33..33+N-1] file identifier
 + 1 padding byte if N is even (record must be even-length)
```

Every directory sector begins with two special records:
- Self-reference: identifier = `0x00` (1 byte), flags = `0x02`
- Parent-reference: identifier = `0x01` (1 byte), flags = `0x02`

Files carry the ISO version suffix: `FILENAME.EXT;1`.
Directories do not.

**Path table entry** (variable, minimum 8 bytes):
```
[0]     length of directory identifier
[1]     extended attribute record length (0)
[2..5]  extent LBA (LE for L-table, BE for M-table)
[6..7]  directory number of parent (root's parent = 1 = root itself)
[8+]    directory identifier (0x00 for root)
 + 1 padding byte if identifier length is odd
```

### ISO 9660 Level

We target **Level 2**: filenames up to 31 characters, uppercase A–Z 0–9
underscore dot.  This covers all practical Amiga filenames and is the
maximum without requiring Joliet extensions (UCS-2), which are not needed
for the CD32 ROM's filesystem driver.

---

## Build-time dependency — FatFS multi-partition

FatFS defaults to treating the SD card as a single volume.  To mount two
partitions simultaneously, `ffconf.h` must have:

```c
#define FF_MULTI_PARTITION  1
```

And `hw_config.c` (or a new `fatfs_partition.c`) must define the
volume-to-partition map:

```c
PARTITION VolToPart[] = {
    {0, 1},   // logical drive "0:/" → physical drive 0, MBR partition 1
    {0, 2},   // logical drive "1:/" → physical drive 0, MBR partition 2
};
```

All existing code that opens files as `"0:/..."` is unaffected.  The virtual
disc feature opens files as `"1:/..."`.  No other FatFS behaviour changes.

The SD card must be partitioned with an MBR partition table (not GPT) and
partition 2 formatted FAT32, sized ≤ 650 MB.  This is a one-time setup step
performed by the user on their desktop; the firmware has no role in
partitioning or formatting.

---

## Constraints

| Constraint | Value | Reason |
|------------|-------|--------|
| Max entries (files + dirs) | 512 | Memory budget (see below) |
| Max directory depth | 8 | ISO 9660 §6.8.2.1 |
| Max filename length | 31 chars | ISO 9660 Level 2 |
| Max disc size | Partition 2 physical size | Enforced at partition creation, not in firmware |
| Filename charset | A–Z 0–9 `_` `.` | ISO 9660 Level 2 (enforced by uppercasing) |
| Audio tracks | None | Data-only disc; Akiko does not require audio |
| CD-ROM XA / Mode 2 | Not supported | Mode 1 only |
| File version suffix | `;1` appended automatically | ISO 9660 §9.1 |

---

## Data Structures

### `vdisc_entry_t`

Flat array; one element per file or directory found during scan.

```c
#define VDISC_MAX_ENTRIES  512
#define VDISC_NAME_LEN      32   // 31 chars + NUL

typedef struct {
    char     name[VDISC_NAME_LEN]; // Uppercased, ISO 9660 L2 charset
    uint32_t lba;                  // Assigned virtual LBA (extent start)
    uint32_t size;                 // Bytes (files) or dir-record bytes (dirs)
    uint16_t parent_idx;           // Index of parent entry (0 = root is own parent)
    uint16_t dir_child_start;      // First child index in sorted list (dirs only)
    uint16_t dir_child_count;      // Number of direct children (dirs only)
    bool     is_dir;
} vdisc_entry_t;
```

Memory: `512 × 68 bytes ≈ 34 KB` — well within the 520 KB RP2350 SRAM.

### `vdisc_t`

Companion structure attached to a `disc_image_t` when format is
`DISC_FORMAT_VDIR`.

```c
typedef struct {
    uint32_t        total_sectors;           // Including system area + metadata
    uint32_t        dir_data_lba_start;      // LBA 20 — first dir sector
    uint32_t        file_data_lba_start;     // LBA after all directory sectors
    uint32_t        entry_count;             // Number of entries in table[]
    vdisc_entry_t   table[VDISC_MAX_ENTRIES];
} vdisc_t;
```

`root_path` is not stored — the source is always `"1:/"` (logical drive 2,
partition 2).  This is a constant, not a parameter.

`vdisc_t` is stored as a member of `disc_image_t` (inside a union with the
existing format-specific fields that don't exist yet, or as a dedicated
field).  Its size is approximately `16 + 34KB ≈ 34 KB`.

---

## LBA Layout Algorithm

### Phase 1 — Scan (at mount time)

1. Recursively walk `"1:/"` using FatFS `f_opendir` / `f_readdir`.
2. For each entry, append a `vdisc_entry_t` to `vdisc->table[]`.
3. Uppercase and sanitise the filename to ISO 9660 Level 2 charset.
4. Log and return false if:
   - entry count exceeds `VDISC_MAX_ENTRIES`
   - directory depth exceeds 8

The 650 MB capacity limit is not checked here — it is enforced by the
physical size of partition 2 at SD card setup time.  If the user somehow
places more data on partition 2 than fits on a CD (which FAT32 would allow
on a large partition), the total_sectors value computed in Phase 2 will
exceed 333,000 and `disc_build_toc_response` will produce an oversized TOC.
That scenario is documented as a user error in the setup instructions, not
handled defensively in firmware.

### Phase 2 — LBA assignment (immediately after scan)

```
lba = 20                                     // skip system area + PVD/VDST/path tables

for each directory entry (root first, then breadth-first):
    entry.lba  = lba
    entry.size = 2048                        // one sector per directory for simplicity
    lba       += 1

vdisc->file_data_lba_start = lba

for each file entry (any order):
    entry.lba  = lba
    lba       += (entry.size + 2047) / 2048  // ceil division

vdisc->total_sectors = lba
```

`vdisc->dir_data_lba_start` = 20 (constant).

Directories are capped at one 2048-byte record sector each.  A single sector
holds approximately 40–60 file entries (averaging 35–50 bytes per record),
which is sufficient for any practical Amiga software tree.  If a directory
has more entries than fit in one sector we cap at that limit and log a
warning; this is noted as a known limitation.

---

## Sector Synthesis (runtime, per-LBA)

`vdisc_read_sector(vdisc, lba, buf2048)` dispatches as follows:

```
lba 0..15   → zero-fill 2048 bytes  (system area)
lba 16      → build_pvd(vdisc, buf)
lba 17      → build_vdst(buf)
lba 18      → build_path_table_l(vdisc, buf)
lba 19      → build_path_table_m(vdisc, buf)
lba 20..    → if lba < file_data_lba_start:
                  i = lba - 20  (directory index)
                  build_dir_sector(vdisc, entry[dir_index[i]], buf)
              else:
                  file, offset = find_file_at_lba(vdisc, lba)
                  f_lseek + f_read 2048 bytes from SD
                  zero-pad remainder if last sector
```

The caller (`disc_read_sector`) wraps the 2048-byte payload in
`disc_synthesise_sector()` as it already does for ISO images, producing a
full 2352-byte raw sector with sync pattern, MSF header, and mode byte.

### `build_pvd` key fields

- Volume identifier: basename of `root_path`, space-padded to 32 bytes
- Volume space size: `vdisc->total_sectors` (LE+BE u32)
- Logical block size: 2048
- Path table size: computed from number of directories
- L-path table LBA: 18 (LE u32)
- M-path table LBA: 18 (BE u32)
- Root directory record (34 bytes): LBA=20, size=2048, flags=0x02, id=`\x00`

### `build_dir_sector`

1. Zero the 2048-byte buffer.
2. Write self-reference record (id=`\x00`, lba=this dir's LBA, size=2048).
3. Write parent-reference record (id=`\x01`, lba=parent dir's LBA).
4. For each direct child of this directory, write a directory record:
   - Files: flags=0x00, size=`entry.size`, id=`"NAME.EXT;1"`
   - Subdirs: flags=0x02, size=2048, id=`"DIRNAME"`
5. Stop when `offset + next_record_len > 2048` (directory full).

---

## Integration with `disc_image_t`

### New format enum value

```c
DISC_FORMAT_VDIR,  // Virtual directory — synthesised ISO 9660
```

### `disc_open` extension

The virtual disc is not opened by path — it is always partition 2.  Add a
new entry point `disc_open_vdir(disc_image_t *disc)` (no path argument) that
mounts `"1:/"` directly.  The caller (webserver UI or commo_bridge) invokes
this explicitly when the user selects "virtual disc" rather than an image
file.  `disc_open` itself is unchanged.

### `disc_read_sector` extension

```c
if (disc->format == DISC_FORMAT_VDIR) {
    uint8_t data2048[SECTOR_DATA_BYTES];
    vdisc_read_sector(&disc->vdisc, lba, data2048);
    disc_synthesise_sector(buf, lba, data2048);
    return (mode == SECTOR_MODE_RAW) ? SECTOR_RAW_BYTES : SECTOR_DATA_BYTES;
}
```

### `disc_build_toc_response` extension

Virtual disc always has one data track (track 1, CTRL=0x04, LBA 0) and a
lead-out.  The existing TOC builder already handles this shape; the
`disc_image_t` `tracks[]` array just needs to be populated correctly by
`disc_open_vdir`.

---

## Memory Budget

| Item | Size |
|------|------|
| `vdisc_t` (512 entries) | ~35 KB |
| Sector cache (8 slots × 2352 B) | ~19 KB |
| DA DMA buffers (2 × ~8 KB) | ~16 KB |
| Stack + misc | ~30 KB |
| Remaining for code + globals | ~420 KB |

Total SRAM: 520 KB.  The 35 KB `vdisc_t` fits without pressure.

If SRAM becomes tight (e.g., a future feature adds a large buffer), reduce
`VDISC_MAX_ENTRIES` to 256 (halves to ~17 KB) — sufficient for all Amiga
floppy archives which rarely exceed 100 files per disc equivalent.

---

## Test Plan

Tests live in `tests/host/test_virtual_disc.cpp` and compile against a
FatFS injectable sim (same pattern as `parser_tests`).

### Group `VdiscMount`  — 7 tests

| Test | Asserts |
|------|---------|
| `MountEmptyDir` | Success, total_sectors == 20+1 (system+metadata+1 empty root dir sector), 1 directory entry |
| `MountSingleFile` | entry count == 2 (root + 1 file), file entry has size == actual file size |
| `MountNestedSubdirs` | 3-level tree, all directories get distinct non-overlapping LBAs |
| `MountExceedMaxEntries` | returns failure when injected entry count > VDISC_MAX_ENTRIES |
| `MountExceedMaxDepth` | returns failure when directory depth > 8 |
| `MountFilenameUppercased` | lowercase filename "readme.txt" becomes "README.TXT" in entry.name |
| `MountFilenameIllegalCharsReplaced` | "my file!.sh" → "MY_FILE_.SH" (spaces and `!` → `_`) |

### Group `VdiscLbaLayout`  — 8 tests

| Test | Asserts |
|------|---------|
| `SystemAreaLba0To15` | `vdisc_sector_type(lba)` returns `VDISC_SECT_SYSTEM` for LBA 0..15 |
| `PvdAtLba16` | `vdisc_sector_type(16)` returns `VDISC_SECT_PVD` |
| `VdstAtLba17` | `vdisc_sector_type(17)` returns `VDISC_SECT_VDST` |
| `PathTableLAtLba18` | `vdisc_sector_type(18)` returns `VDISC_SECT_PATH_L` |
| `PathTableMAtLba19` | `vdisc_sector_type(19)` returns `VDISC_SECT_PATH_M` |
| `RootDirAtLba20` | `vdisc_sector_type(20)` returns `VDISC_SECT_DIR` |
| `FileLbasAfterAllDirLbas` | for all file entries, entry.lba >= file_data_lba_start |
| `LbasNonOverlapping` | no two entries share the same LBA |

### Group `VdiscPvd`  — 7 tests

| Test | Asserts |
|------|---------|
| `PvdMagicBytes` | `buf[1..5]` == `"CD001"` |
| `PvdTypeByte` | `buf[0]` == `0x01` |
| `PvdVersionByte` | `buf[6]` == `0x01` |
| `PvdLogicalBlockSize` | bytes 128–131 encode 2048 in LE+BE |
| `PvdVolumeSizeMatchesTotalSectors` | bytes 80–87 encode `vdisc->total_sectors` in LE+BE |
| `PvdRootDirRecordLba` | bytes 158–161 of PVD == LBA 20 (LE u32) |
| `PvdVolumeIdentifier` | bytes 40–71 == uppercased dirname, space-padded to 32 |

### Group `VdiscVdst`  — 2 tests

| Test | Asserts |
|------|---------|
| `VdstTypeByte` | `buf[0]` == `0xFF` |
| `VdstMagicBytes` | `buf[1..5]` == `"CD001"` |

### Group `VdiscPathTable`  — 5 tests

| Test | Asserts |
|------|---------|
| `PathTableLRootEntryLba` | bytes 2–5 of L-table == 20 (LE u32) |
| `PathTableMRootEntryLba` | bytes 2–5 of M-table == 20 (BE u32) |
| `PathTableRootIdentifier` | identifier byte at offset 8 == `0x00` |
| `PathTableRootParentNumber` | parent directory number field == `0x0001` |
| `PathTableSubdirPresent` | injected subdir appears as second path table entry with correct LBA |

### Group `VdiscDirSector`  — 8 tests

| Test | Asserts |
|------|---------|
| `RootDirSelfRefFirst` | first record in root dir sector has identifier `\x00`, flags 0x02 |
| `RootDirParentRefSecond` | second record has identifier `\x01`, flags 0x02 |
| `FileEntryFlagsZero` | file directory record has flags == 0x00 |
| `DirEntryFlagsTwo` | subdirectory directory record has flags == 0x02 |
| `FileEntryVersionSuffix` | file identifier ends with `;1` |
| `DirEntryNoVersionSuffix` | directory identifier does NOT end with `;1` |
| `FileEntrySizeMatchesActual` | data length field in record == actual file byte count |
| `RecordLengthIsEven` | every directory record has even record-length byte |

### Group `VdiscFileData`  — 6 tests

| Test | Asserts |
|------|---------|
| `FirstSectorOfFileMatchesSdContent` | read LBA == file.lba → first 2048 bytes match injected file data |
| `SecondSectorOfLargeFile` | read LBA == file.lba + 1 → second 2048-byte page matches |
| `LastSectorZeroPadded` | last sector of file with size not a multiple of 2048 has trailing zeros |
| `SystemAreaReturnsZeros` | LBA 0 → all 2048 bytes are zero |
| `UnmappedLbaBeyondTotalReturnsZero` | LBA > total_sectors → returns zero buffer (no crash) |
| `FileReadRespectsParentPath` | nested file at `subdir/file.txt` is reached via correct SD path reconstruction |

### Group `VdiscToc`  — 5 tests

| Test | Asserts |
|------|---------|
| `TocHasOneDataTrack` | `disc->last_track == 1` |
| `TocTrack1IsData` | `disc->tracks[1].type == TRACK_TYPE_DATA` |
| `TocTrack1StartsAtLba0` | `disc->tracks[1].start_lba == 0` |
| `TocLeadOutLbaMatchesTotalSectors` | lead-out `start_lba == vdisc->total_sectors` |
| `TocResponseEncodesCorrectly` | `disc_build_toc_response` produces 4-byte entries matching track 1 + lead-out |

### Group `VdiscIntegration`  — 5 tests

| Test | Asserts |
|------|---------|
| `DiscOpenVdirSetsFormat` | `disc_open_vdir` sets `disc->format == DISC_FORMAT_VDIR` and returns true when partition 2 is present in the injected FatFS sim |
| `DiscReadSectorAtLba16ReturnsPvd` | calling `disc_read_sector(disc, 16, buf, SECTOR_MODE_RAW)` → `buf[13..17]` = `"\x01CD001"` (mode1 sector, PVD at payload offset 16) |
| `DiscReadSectorAtLba0ReturnsMode1` | sync pattern `00 FF×10 00` present in first 12 bytes |
| `DiscReadSectorFileDataRoundTrip` | inject a known 2048-byte payload on SD; read the LBA assigned to that file; payload bytes match |
| `DiscCloseFreesFatfsHandles` | after `disc_close`, no FatFS file handles are open |

---

## Implementation Sequence

1. `include/virtual_disc.h` — `vdisc_entry_t`, `vdisc_t`, public API
2. `src/virtual_disc.c` — mount (scan + LBA assign), sector type dispatch,
   PVD/VDST/path-table/dir-sector builders, file data reader
3. `include/disc_image.h` — add `DISC_FORMAT_VDIR`, add `vdisc_t vdisc` member
4. `src/disc_image.c` — hook `disc_open_vdir` into `disc_open`, handle
   `DISC_FORMAT_VDIR` in `disc_read_sector`
5. `tests/host/test_virtual_disc.cpp` — all groups above
6. `tests/host/Makefile` — add new TU to `cd32_tests` binary
7. `CLAUDE.md` — update file status table and test group table

Estimated new code: ~600 lines in `virtual_disc.c`, ~400 lines in the test
file.  No existing functionality is changed; all new paths are behind the
`DISC_FORMAT_VDIR` branch.

---

## Known Limitations / Out of Scope

- **Joliet extensions** (long filenames > 31 chars in UCS-2): not included;
  Amiga filenames are ASCII and 31 characters is sufficient.
- **Rock Ridge** (POSIX extensions for Unix systems): not needed.
- **Multi-session**: not supported; single-session data disc only.
- **Directories with > ~50 files**: the single-sector-per-directory limit
  means large flat directories will be truncated.  A v2 extension can assign
  multiple sectors, but v1 keeps the implementation minimal.
- **Symlinks / hard links on SD**: FatFS does not support these; they are
  ignored during scan.
- **Real-time directory updates**: the disc is snapshotted at mount time.
  Changes to the SD directory while the disc is "inserted" are not reflected.
  This matches physical disc behaviour.
