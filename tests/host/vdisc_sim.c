#include "vdisc_sim.h"
#include "virtual_disc.h"
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdlib.h>    // NULL
#include <stdio.h>     // snprintf

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

#define VDISC_SIM_MAX_ENTRIES   600
#define VDISC_SIM_MAX_OPEN_DIRS   8

typedef struct {
    char           path[256];
    const uint8_t *data;
    uint32_t       size;
    int            is_dir;
    int            in_use;
} vdisc_sim_entry_t;

typedef struct {
    char path[256];   // Directory being listed (normalized, no trailing slash except root)
    int  next_idx;
    int  in_use;
} vdisc_sim_dir_t;

static vdisc_sim_entry_t  s_entries[VDISC_SIM_MAX_ENTRIES];
static int                s_entry_count = 0;
static vdisc_sim_dir_t    s_open_dirs[VDISC_SIM_MAX_OPEN_DIRS];

// Globals required by disc_image.h's inclusion chain
FATFS g_stub_fatfs = {0};
DWORD g_stub_ff_fre_clust = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Normalize a path: strip trailing '/' unless it is exactly "1:/"
static void normalize_path(char *dst, const char *src, size_t bufsz) {
    size_t len = strlen(src);
    if (len == 0) {
        if (bufsz > 0) dst[0] = '\0';
        return;
    }
    if (len >= bufsz) len = bufsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';

    // Strip trailing slash unless this is the root "1:/"
    while (len > 3 && dst[len - 1] == '/') {
        dst[--len] = '\0';
    }
}

// Returns true if child_path is a direct (immediate) child of parent_path.
// Both paths should already be normalized.
static int is_direct_child(const char *child_path, const char *parent_path) {
    size_t plen = strlen(parent_path);
    size_t clen = strlen(child_path);

    // The root is "1:/" (3 chars).  Other dirs do NOT have a trailing slash.
    // A direct child of "1:/" has path "1:/NAME" with no further slash.
    // A direct child of "1:/PARENT" has path "1:/PARENT/NAME" with one more slash.

    const char *prefix;
    size_t prefix_len;

    if (plen == 3 && parent_path[1] == ':' && parent_path[2] == '/') {
        // Parent is root "1:/"
        prefix     = parent_path;
        prefix_len = plen;   // "1:/" = 3 chars; child should be "1:/NAME"
    } else {
        // Non-root parent: child path must be "PARENT/NAME"
        // We'll construct "PARENT/" and check
        // For simplicity: check that child starts with parent + "/"
        prefix_len = plen + 1;  // parent + '/'
        char tmp[256];
        if (plen + 1 >= sizeof(tmp)) return 0;
        memcpy(tmp, parent_path, plen);
        tmp[plen] = '/';
        tmp[plen + 1] = '\0';
        if (clen <= prefix_len) return 0;
        if (strncasecmp(child_path, tmp, prefix_len) != 0) return 0;
        // Make sure there's no further '/' in the suffix
        const char *suffix = child_path + prefix_len;
        if (strchr(suffix, '/') != NULL) return 0;
        return (suffix[0] != '\0') ? 1 : 0;
    }

    // Root case
    if (clen <= prefix_len) return 0;
    if (strncasecmp(child_path, prefix, prefix_len) != 0) return 0;
    const char *suffix = child_path + prefix_len;
    if (strchr(suffix, '/') != NULL) return 0;
    return (suffix[0] != '\0') ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Public registration API
// ---------------------------------------------------------------------------

void vdisc_sim_register_dir(const char *path) {
    if (s_entry_count >= VDISC_SIM_MAX_ENTRIES) return;
    vdisc_sim_entry_t *e = &s_entries[s_entry_count++];
    normalize_path(e->path, path, sizeof(e->path));
    e->data   = NULL;
    e->size   = 0;
    e->is_dir = 1;
    e->in_use = 1;
}

void vdisc_sim_register_file(const char *path, const uint8_t *data, uint32_t size) {
    if (s_entry_count >= VDISC_SIM_MAX_ENTRIES) return;
    vdisc_sim_entry_t *e = &s_entries[s_entry_count++];
    normalize_path(e->path, path, sizeof(e->path));
    e->data   = data;
    e->size   = size;
    e->is_dir = 0;
    e->in_use = 1;
}

void vdisc_sim_reset(void) {
    vdisc_invalidate_fil();
    memset(s_entries,  0, sizeof(s_entries));
    memset(s_open_dirs, 0, sizeof(s_open_dirs));
    s_entry_count = 0;
}

// ---------------------------------------------------------------------------
// FatFS file operations
// ---------------------------------------------------------------------------

FRESULT f_open(FIL *fp, const char *path, int mode) {
    (void)mode;
    char norm[256];
    normalize_path(norm, path, sizeof(norm));
    for (int i = 0; i < s_entry_count; i++) {
        if (!s_entries[i].in_use || s_entries[i].is_dir) continue;
        if (strcasecmp(s_entries[i].path, norm) == 0) {
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

FRESULT f_close(FIL *fp) {
    if (fp) {
        fp->data = NULL;
        fp->size = 0;
        fp->pos  = 0;
    }
    return FR_OK;
}

FSIZE_t f_size(FIL *fp)  { return fp->size; }
FSIZE_t f_tell(FIL *fp)  { return fp->pos;  }

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    fp->pos = (ofs <= fp->size) ? ofs : fp->size;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    FSIZE_t avail = fp->size - fp->pos;
    UINT n = (UINT)((btr < (UINT)avail) ? btr : (UINT)avail);
    if (n > 0 && fp->data) {
        memcpy(buf, fp->data + fp->pos, n);
    } else if (n > 0) {
        // data == NULL means file is all zeros
        memset(buf, 0, n);
    }
    fp->pos += n;
    if (br) *br = n;
    return FR_OK;
}

char *f_gets(char *buf, int len, FIL *fp) {
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

// ---------------------------------------------------------------------------
// FatFS directory operations
// ---------------------------------------------------------------------------

FRESULT f_opendir(DIR *dp, const TCHAR *path) {
    if (!dp) return FR_NO_FILE;

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < VDISC_SIM_MAX_OPEN_DIRS; i++) {
        if (!s_open_dirs[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return FR_NO_FILE;

    char norm[256];
    normalize_path(norm, path, sizeof(norm));

    s_open_dirs[slot].in_use   = 1;
    s_open_dirs[slot].next_idx = 0;
    // Use snprintf to avoid -Wstringop-truncation false positive from strncpy
    snprintf(s_open_dirs[slot].path, sizeof(s_open_dirs[slot].path), "%s", norm);

    dp->_sim_dir_idx = slot;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    if (!dp || dp->_sim_dir_idx < 0) {
        if (fno) fno->fname[0] = '\0';
        return FR_OK;
    }
    int slot = dp->_sim_dir_idx;
    if (!s_open_dirs[slot].in_use) {
        if (fno) fno->fname[0] = '\0';
        return FR_OK;
    }

    const char *dir_path = s_open_dirs[slot].path;
    int start = s_open_dirs[slot].next_idx;

    for (int i = start; i < s_entry_count; i++) {
        if (!s_entries[i].in_use) continue;
        // Skip the root entry itself
        if (strcasecmp(s_entries[i].path, dir_path) == 0) {
            s_open_dirs[slot].next_idx = i + 1;
            continue;
        }
        if (is_direct_child(s_entries[i].path, dir_path)) {
            // Found a direct child
            // Extract the basename (last component)
            const char *p = strrchr(s_entries[i].path, '/');
            const char *basename = p ? p + 1 : s_entries[i].path;

            if (fno) {
                strncpy(fno->fname, basename, 255);
                fno->fname[255] = '\0';
                fno->fsize      = (FSIZE_t)s_entries[i].size;
                fno->fattrib    = s_entries[i].is_dir ? AM_DIR : AM_ARC;
            }
            s_open_dirs[slot].next_idx = i + 1;
            return FR_OK;
        }
    }

    // End of directory
    if (fno) fno->fname[0] = '\0';
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    if (dp && dp->_sim_dir_idx >= 0 && dp->_sim_dir_idx < VDISC_SIM_MAX_OPEN_DIRS) {
        s_open_dirs[dp->_sim_dir_idx].in_use = 0;
        dp->_sim_dir_idx = -1;
    }
    return FR_OK;
}

// Note: f_stat and f_getfree are defined as static inline in ff.h.
// g_stub_fatfs and g_stub_ff_fre_clust are extern in ff.h; define them here.
