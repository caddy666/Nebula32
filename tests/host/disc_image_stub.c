/*
 * disc_image_stub.c — Minimal stub for disc_image.c functions needed by
 * sector_cache.c at link time during host-native test builds.
 *
 * disc_synthesise_sector() is implemented here in full (not stubbed out)
 * because it is pure — no FatFS I/O — so it can run on the host and be
 * covered by the SectorLayout CppUTest group in test_ecc.cpp.
 */
#include "disc_image.h"
#include "ecc.h"
#include "subcode.h"
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

bool disc_open_vdir(disc_image_t *disc, vdisc_t *vd)
{
    (void)disc; (void)vd;
    return false;
}

void disc_synthesise_sector(uint8_t *buf, uint32_t lba, const uint8_t *data2048)
{
    memcpy(buf, CD_SYNC_PATTERN, 12);
    msf_t msf = lba_to_msf(lba);
    buf[12] = msf.minute;
    buf[13] = msf.second;
    buf[14] = msf.frame;
    buf[15] = 0x01;
    memcpy(buf + 16, data2048, 2048);
    ecc_sector_complete(buf);
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

