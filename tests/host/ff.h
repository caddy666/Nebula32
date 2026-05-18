#pragma once
// FatFS simulation header for the parser_tests binary.
// Placed in tests/host/ so it shadows tests/stubs/ff.h when compiled
// with -I. preceding -I../stubs (disc_image.c uses #include "ff.h").
//
// Unlike the inline no-op stubs, FIL here carries a real data pointer so
// fatfs_sim.c can serve controlled byte sequences to the parsers under test.
//
// Also provides FATFS/f_getfree/f_stat for the webserver HTML tests —
// those symbols are used by webserver.c's sd_get_space() and cover_exists().
#include <stdint.h>

typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t FSIZE_t;
typedef unsigned int UINT;
typedef int FRESULT;
typedef char TCHAR;
#define FR_OK      0
#define FR_NO_FILE 4
#define FA_READ    0x01

typedef struct {
    const uint8_t *data;
    FSIZE_t        size;
    FSIZE_t        pos;
} FIL;

// FATFS volume object — fields used by sd_get_space() in webserver.c.
typedef struct {
    WORD  csize;    // Cluster size [sectors]
    DWORD n_fatent; // Number of FAT entries (= total clusters + 2)
} FATFS;

// Controllable free-space state — defined in test_webserver_html.cpp.
extern FATFS g_stub_fatfs;
extern DWORD g_stub_ff_fre_clust;

// Directory support (used by virtual_disc.c)
#define AM_DIR  0x10   // Directory attribute
#define AM_ARC  0x20   // Normal file attribute

typedef struct {
    FSIZE_t fsize;
    uint8_t fattrib;
    char    fname[256];
} FILINFO;

typedef struct {
    int _sim_dir_idx;   // Index into sim's open-directory pool (-1 if unused)
} DIR;

FRESULT f_opendir (DIR *dp, const TCHAR *path);
FRESULT f_readdir (DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

FRESULT  f_open  (FIL *fp, const char *path, int mode);
FRESULT  f_close (FIL *fp);
FSIZE_t  f_size  (FIL *fp);
FSIZE_t  f_tell  (FIL *fp);
FRESULT  f_lseek (FIL *fp, FSIZE_t ofs);
FRESULT  f_read  (FIL *fp, void *buf, UINT btr, UINT *br);
char    *f_gets  (char *buf, int len, FIL *fp);

// cover_exists() checks for cover art; always return NO_FILE in host tests.
static inline FRESULT f_stat(const TCHAR *path, void *fno)
    __attribute__((unused));
static inline FRESULT f_stat(const TCHAR *path, void *fno)
    { (void)path; (void)fno; return FR_NO_FILE; }

// sd_get_space() calls f_getfree; use the controllable stub globals.
static inline FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
    __attribute__((unused));
static inline FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
    { (void)path; *nclst = g_stub_ff_fre_clust; *fatfs = &g_stub_fatfs; return FR_OK; }
