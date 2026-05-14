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

const track_t *disc_find_track(const disc_image_t *disc, uint32_t lba)
{
    (void)disc; (void)lba;
    return NULL;
}

uint32_t disc_build_toc_response(const disc_image_t *disc, uint8_t *buf,
                                  uint32_t buf_size)
{
    (void)disc; (void)buf; (void)buf_size;
    return 0;
}
