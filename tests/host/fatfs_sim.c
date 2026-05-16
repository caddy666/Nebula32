#include "fatfs_sim.h"
#include <string.h>

#define MAX_SIM_FILES 8

typedef struct {
    const char    *fragment;
    const uint8_t *data;
    FSIZE_t        size;
} sim_entry_t;

static sim_entry_t s_entries[MAX_SIM_FILES];
static int         s_count = 0;

void fatfs_sim_register_sidecar(const char *fragment,
                                const uint8_t *data, FSIZE_t size)
{
    if (s_count < MAX_SIM_FILES) {
        s_entries[s_count].fragment = fragment;
        s_entries[s_count].data     = data;
        s_entries[s_count].size     = size;
        s_count++;
    }
}

void fatfs_sim_inject(FIL *fp, const uint8_t *data, FSIZE_t size)
{
    fp->data = data;
    fp->size = size;
    fp->pos  = 0;
}

void fatfs_sim_reset(void)
{
    s_count = 0;
}

// ---------------------------------------------------------------------------
// FatFS API implementation
// ---------------------------------------------------------------------------

FRESULT f_open(FIL *fp, const char *path, int mode)
{
    (void)mode;
    for (int i = 0; i < s_count; i++) {
        if (strstr(path, s_entries[i].fragment)) {
            fp->data = s_entries[i].data;
            fp->size = s_entries[i].size;
            fp->pos  = 0;
            return FR_OK;
        }
    }
    fp->data = NULL;
    fp->size = 0;
    fp->pos  = 0;
    return FR_NO_FILE;
}

FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }

FSIZE_t f_size(FIL *fp) { return fp->size; }

FSIZE_t f_tell(FIL *fp) { return fp->pos; }

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    fp->pos = (ofs <= fp->size) ? ofs : fp->size;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br)
{
    FSIZE_t avail = fp->size - fp->pos;
    UINT n = (UINT)((btr < avail) ? btr : avail);
    if (n > 0 && fp->data) memcpy(buf, fp->data + fp->pos, n);
    fp->pos += n;
    if (br) *br = n;
    return FR_OK;
}

char *f_gets(char *buf, int len, FIL *fp)
{
    if (!fp->data || fp->pos >= fp->size || len <= 0) return NULL;
    int n = 0;
    while (n < len - 1 && fp->pos < fp->size) {
        char c = (char)fp->data[fp->pos++];
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return (n > 0) ? buf : NULL;
}
