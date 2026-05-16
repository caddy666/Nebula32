#pragma once
// Test setup API for the FatFS simulation used by test_disc_parser.cpp.
// Call fatfs_sim_register_sidecar() to inject a virtual sidecar file (CUE/MDS)
// that f_open() will serve when its path contains the registered fragment.
// Call fatfs_sim_inject() to fill an already-allocated FIL (e.g. disc->image_file)
// with a virtual image byte sequence.
// Call fatfs_sim_reset() in teardown to clear all registrations.
#include "ff.h"

void fatfs_sim_register_sidecar(const char *path_fragment,
                                const uint8_t *data, FSIZE_t size);
void fatfs_sim_inject(FIL *fp, const uint8_t *data, FSIZE_t size);
void fatfs_sim_reset(void);
