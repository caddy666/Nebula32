#pragma once
// Minimal FatFS stub for host-native test builds.
// Only the types used by disc_image.h / subcode.h need to be defined.
#include <stdint.h>
#include <stdio.h>

typedef uint32_t FSIZE_t;
typedef unsigned int UINT;
typedef int FRESULT;
#define FR_OK 0
#define FA_READ 0x01

typedef struct { int _dummy; } FIL;

static inline FRESULT f_open(FIL *fp, const char *path, int mode)
    { (void)fp; (void)path; (void)mode; return FR_OK; }
static inline FRESULT f_close(FIL *fp)
    { (void)fp; return FR_OK; }
static inline FSIZE_t f_size(FIL *fp)
    { (void)fp; return 0; }
static inline FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
    { (void)fp; (void)ofs; return FR_OK; }
static inline FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br)
    { (void)fp; (void)buf; (void)btr; if (br) *br = 0; return FR_OK; }
static inline char *f_gets(char *buf, int len, FIL *fp)
    { (void)fp; (void)len; (void)buf; return NULL; }
