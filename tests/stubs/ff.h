#pragma once
// Minimal FatFS stub for host-native test builds.
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t FSIZE_t;
typedef unsigned int UINT;
typedef int FRESULT;
typedef char TCHAR;
#define FR_OK      0
#define FR_NO_FILE 4
#define FA_READ    0x01

typedef struct { int _dummy; } FIL;

// FATFS volume object — only the fields used by sd_get_space() are needed.
typedef struct {
    WORD  csize;    // Cluster size [sectors]
    DWORD n_fatent; // Number of FAT entries (= total clusters + 2)
} FATFS;

// Controllable free-space state for webserver HTML tests.
// Define these in the TU that exercises f_getfree (test_webserver_html.cpp).
extern FATFS g_stub_fatfs;
extern DWORD g_stub_ff_fre_clust;

static inline FRESULT f_open(FIL *fp, const TCHAR *path, int mode)
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
// cover_exists() calls f_stat to check for a cover JPEG — always return NO_FILE
// so tests get the disc-emoji placeholder instead of trying to serve an image.
static inline FRESULT f_stat(const TCHAR *path, void *fno)
    { (void)path; (void)fno; return FR_NO_FILE; }
static inline FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
    { (void)path; *nclst = g_stub_ff_fre_clust; *fatfs = &g_stub_fatfs; return FR_OK; }
