#pragma once
// FatFS simulation header for the parser_tests binary.
// Placed in tests/host/ so it shadows tests/stubs/ff.h when compiled
// with -I. preceding -I../stubs (disc_image.c uses #include "ff.h").
//
// Unlike the inline no-op stubs, FIL here carries a real data pointer so
// fatfs_sim.c can serve controlled byte sequences to the parsers under test.
#include <stdint.h>

typedef uint32_t FSIZE_t;
typedef unsigned int UINT;
typedef int FRESULT;
#define FR_OK     0
#define FR_NO_FILE 4
#define FA_READ   0x01

typedef struct {
    const uint8_t *data;
    FSIZE_t        size;
    FSIZE_t        pos;
} FIL;

FRESULT  f_open  (FIL *fp, const char *path, int mode);
FRESULT  f_close (FIL *fp);
FSIZE_t  f_size  (FIL *fp);
FSIZE_t  f_tell  (FIL *fp);
FRESULT  f_lseek (FIL *fp, FSIZE_t ofs);
FRESULT  f_read  (FIL *fp, void *buf, UINT btr, UINT *br);
char    *f_gets  (char *buf, int len, FIL *fp);
