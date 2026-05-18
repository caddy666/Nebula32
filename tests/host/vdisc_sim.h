#pragma once
// FatFS simulation for the vdisc_tests binary.
// Supports both directory traversal (f_opendir/f_readdir/f_closedir) and
// file reading (f_open/f_read/f_lseek/f_size/f_tell/f_close).
// All paths are matched case-insensitively (FAT32 is case-insensitive).
#include "ff.h"
#include <stdint.h>

// Register a directory in the virtual tree.
// path: full FatFS path e.g. "1:/subdir" or "1:/".
void vdisc_sim_register_dir(const char *path);

// Register a file in the virtual tree.
// data may be NULL (file will read as zeros); size is the file size in bytes.
void vdisc_sim_register_file(const char *path, const uint8_t *data, uint32_t size);

// Reset all registered entries (call in test teardown).
void vdisc_sim_reset(void);
