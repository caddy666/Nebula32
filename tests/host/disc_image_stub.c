/*
 * disc_image_stub.c — Minimal stub for disc_image.c functions needed by
 * sector_cache.c at link time during host-native test builds.
 */
#include "disc_image.h"
#include <stdint.h>
#include <string.h>

const uint8_t CD_SYNC_PATTERN[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

uint32_t disc_read_sector(disc_image_t *disc, uint32_t lba,
                          uint8_t *buf, sector_mode_t mode)
{
    (void)disc; (void)lba; (void)buf; (void)mode;
    return 0;
}

void disc_close(disc_image_t *disc) { (void)disc; }

bool disc_open(disc_image_t *disc, const char *path)
{
    (void)disc; (void)path;
    return false;
}

void disc_synthesise_sector(uint8_t *buf, uint32_t lba, const uint8_t *data2048)
{
    (void)buf; (void)lba; (void)data2048;
}

/* Real implementations — these functions are pure (no FatFS I/O). */

const track_t *disc_find_track(const disc_image_t *disc, uint32_t lba)
{
    for (uint8_t i = disc->first_track; i <= disc->last_track; i++) {
        const track_t *trk = &disc->tracks[i - 1];
        if (lba >= trk->start_lba && lba < trk->start_lba + trk->length_sectors)
            return trk;
    }
    return NULL;
}

uint32_t disc_build_toc_response(const disc_image_t *disc, uint8_t *buf,
                                  uint32_t buf_size)
{
    uint32_t pos = 0;
    for (uint8_t i = disc->first_track; i <= disc->last_track; i++) {
        if (pos + 3 > buf_size) break;
        const track_t *trk = &disc->tracks[i - 1];
        msf_t msf = lba_to_msf(trk->start_lba);
        buf[pos++] = (uint8_t)(((i / 10) << 4) | (i % 10));
        buf[pos++] = msf.minute;
        buf[pos++] = msf.second;
    }
    if (pos + 3 <= buf_size) {
        msf_t msf = lba_to_msf(disc->total_sectors);
        buf[pos++] = 0xAA;
        buf[pos++] = msf.minute;
        buf[pos++] = msf.second;
    }
    return pos;
}
