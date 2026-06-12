#pragma once
#include <stdint.h>
#include <stdbool.h>

#define VDISC_MAX_ENTRIES  512
#define VDISC_NAME_LEN      32    // 31 chars + NUL (ISO 9660 Level 2 max)

typedef struct {
    char     name[VDISC_NAME_LEN]; // Uppercased ISO 9660 Level 2 charset
    uint32_t lba;                  // Assigned virtual LBA
    uint32_t size;                 // Bytes for files; 0 for dirs (dir size is always 2048)
    uint16_t parent_idx;           // Index of parent entry (0 = root is own parent)
    uint8_t  depth;                // Dir depth: root=0, root's children=1, etc.
    bool     is_dir;
} vdisc_entry_t;

typedef struct {
    uint32_t        total_sectors;
    uint32_t        dir_data_lba_start;   // Always 20
    uint32_t        file_data_lba_start;  // 20 + number_of_dirs
    uint32_t        entry_count;
    vdisc_entry_t   table[VDISC_MAX_ENTRIES];
} vdisc_t;

// Close the cached FIL handle (call before unmounting or switching discs).
void vdisc_invalidate_fil(void);

// Mount partition 2 ("1:/") as a virtual ISO 9660 disc.
// Scans the directory tree and assigns LBAs to all entries.
// Returns false if partition 2 cannot be opened or entry/depth limit exceeded.
bool vdisc_mount(vdisc_t *vd);

// Read one 2048-byte logical block from the virtual disc at 'lba'.
// Returns zeros for LBAs beyond total_sectors or in the system area (0-15).
void vdisc_read_sector(const vdisc_t *vd, uint32_t lba, uint8_t *buf2048);
