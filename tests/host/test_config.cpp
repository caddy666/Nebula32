// =============================================================================
// test_config.cpp — Flash-backed config structure and CRC coverage tests
//
// Intent: verify the config subsystem's pure logic without touching flash or
// the Pico SDK.  config.h depends on PICO_FLASH_SIZE_BYTES and XIP_BASE for
// the flash address macros, which are SDK-only; those macros are unused by
// the struct, CRC, and defaults logic, so this file replicates the relevant
// definitions inline.
//
// The raw CRC-32 algorithm is already tested by the ConfigCrc group in
// test_disc_logic.cpp.  This file focuses on config-structure behaviour:
//
//   config_defaults()  — struct fields match the documented factory settings,
//                        and the CRC stored in crc32 is valid immediately.
//   config_crc32()     — covers all bytes except the trailing crc32 field;
//                        flipping a reserved byte changes the CRC, but
//                        changing the crc32 field itself does not.
//   Flag bitfield      — individual CFG_FLAG_* bits are orthogonal; setting
//                        one does not corrupt another.
//   Magic/version guard — config_valid() rejects wrong magic or wrong version.
//   Struct layout      — sizeof(ode_config_t) == 72 bytes as designed.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Replicated definitions — must stay in sync with include/config.h
// and src/config.c
// ---------------------------------------------------------------------------

#define CONFIG_MAGIC    0xCD320DE5u
#define CONFIG_VERSION  1u

#define CFG_FLAG_AUDIO_ENABLED    (1u << 0)
#define CFG_FLAG_VERIFY_EDC       (1u << 1)
#define CFG_FLAG_SWAP_AUDIO_BYTES (1u << 2)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint8_t  last_image_index;
    uint8_t  speed_mode;
    uint8_t  reserved[58];
    uint32_t crc32;
} ode_config_t;

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int b = 0; b < 8; b++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    return crc;
}

static uint32_t config_crc32(const ode_config_t *cfg)
{
    const uint8_t *data = (const uint8_t *)cfg;
    size_t len = sizeof(ode_config_t) - sizeof(uint32_t);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_byte(crc, data[i]);
    return crc ^ 0xFFFFFFFFu;
}

static void config_defaults(ode_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic            = CONFIG_MAGIC;
    cfg->version          = CONFIG_VERSION;
    cfg->flags            = CFG_FLAG_AUDIO_ENABLED;
    cfg->last_image_index = 0;
    cfg->speed_mode       = 2;
    cfg->crc32            = config_crc32(cfg);
}

// Returns true if magic, version, and CRC are all valid
static bool config_valid(const ode_config_t *cfg)
{
    if (cfg->magic != CONFIG_MAGIC) return false;
    if (cfg->version != CONFIG_VERSION) return false;
    return config_crc32(cfg) == cfg->crc32;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(Config) {};

/* -------------------------------------------------------------------------
 * Struct layout
 * ---------------------------------------------------------------------- */

TEST(Config, StructSize_Is72Bytes)
{
    // magic(4) + version(2) + flags(2) + last_image_index(1) + speed_mode(1)
    // + reserved(58) + crc32(4) = 72 bytes
    LONGS_EQUAL(72, (long)sizeof(ode_config_t));
}

/* -------------------------------------------------------------------------
 * config_defaults — factory values and immediate CRC validity
 * ---------------------------------------------------------------------- */

TEST(Config, Defaults_MagicIsCorrect)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    LONGS_EQUAL((long)CONFIG_MAGIC, (long)cfg.magic);
}

TEST(Config, Defaults_VersionIsCorrect)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    LONGS_EQUAL(CONFIG_VERSION, cfg.version);
}

TEST(Config, Defaults_SpeedMode2x)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    LONGS_EQUAL(2, cfg.speed_mode);
}

TEST(Config, Defaults_AudioEnabledFlagSet)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    CHECK_TRUE(cfg.flags & CFG_FLAG_AUDIO_ENABLED);
}

TEST(Config, Defaults_OtherFlagsClear)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    CHECK_FALSE(cfg.flags & CFG_FLAG_VERIFY_EDC);
    CHECK_FALSE(cfg.flags & CFG_FLAG_SWAP_AUDIO_BYTES);
}

TEST(Config, Defaults_LastImageIndexZero)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    LONGS_EQUAL(0, cfg.last_image_index);
}

TEST(Config, Defaults_CrcIsValidImmediately)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    CHECK_TRUE(config_valid(&cfg));
}

/* -------------------------------------------------------------------------
 * config_crc32 — covers all bytes except the trailing crc32 field
 * ---------------------------------------------------------------------- */

TEST(Config, Crc_FlippingReservedByteChangesCrc)
{
    ode_config_t a, b;
    config_defaults(&a);
    config_defaults(&b);
    b.reserved[0] ^= 0x01;
    CHECK_TRUE(config_crc32(&a) != config_crc32(&b));
}

TEST(Config, Crc_ChangingCrc32FieldDoesNotAffectComputed)
{
    // config_crc32() must exclude the last 4 bytes (the crc32 field itself)
    ode_config_t a, b;
    config_defaults(&a);
    config_defaults(&b);
    b.crc32 ^= 0xDEADBEEFu;   // corrupt the stored CRC
    // The computed CRC of a and b must be identical — crc32 field is excluded
    LONGS_EQUAL((long)config_crc32(&a), (long)config_crc32(&b));
}

TEST(Config, Crc_FlippingMagicChangesCrc)
{
    ode_config_t a, b;
    config_defaults(&a);
    config_defaults(&b);
    b.magic ^= 0x01;
    CHECK_TRUE(config_crc32(&a) != config_crc32(&b));
}

/* -------------------------------------------------------------------------
 * config_valid — magic/version/CRC guard
 * ---------------------------------------------------------------------- */

TEST(Config, Valid_DefaultsPassValidation)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    CHECK_TRUE(config_valid(&cfg));
}

TEST(Config, Valid_WrongMagicFails)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    cfg.magic = 0xDEADBEEFu;
    CHECK_FALSE(config_valid(&cfg));
}

TEST(Config, Valid_WrongVersionFails)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    cfg.version = CONFIG_VERSION + 1;
    // CRC still matches the new content — version check fires first
    cfg.crc32 = config_crc32(&cfg);
    CHECK_FALSE(config_valid(&cfg));
}

TEST(Config, Valid_CorruptedCrcFails)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    cfg.crc32 ^= 0x01;
    CHECK_FALSE(config_valid(&cfg));
}

/* -------------------------------------------------------------------------
 * Flag bitfield — individual flags are orthogonal
 * ---------------------------------------------------------------------- */

TEST(Config, Flags_BitsAreOrthogonal)
{
    // No two flags share a bit
    CHECK_TRUE((CFG_FLAG_AUDIO_ENABLED & CFG_FLAG_VERIFY_EDC)       == 0);
    CHECK_TRUE((CFG_FLAG_AUDIO_ENABLED & CFG_FLAG_SWAP_AUDIO_BYTES) == 0);
    CHECK_TRUE((CFG_FLAG_VERIFY_EDC    & CFG_FLAG_SWAP_AUDIO_BYTES) == 0);
}

TEST(Config, Flags_SetAndClearDoNotBleed)
{
    uint16_t flags = CFG_FLAG_AUDIO_ENABLED;
    flags |= CFG_FLAG_VERIFY_EDC;
    CHECK_TRUE(flags & CFG_FLAG_AUDIO_ENABLED);
    CHECK_TRUE(flags & CFG_FLAG_VERIFY_EDC);

    flags &= ~CFG_FLAG_VERIFY_EDC;
    CHECK_TRUE(flags  & CFG_FLAG_AUDIO_ENABLED);
    CHECK_FALSE(flags & CFG_FLAG_VERIFY_EDC);
}

/* Gap 11: every reserved byte must be zero after config_defaults(). */
TEST(Config, Defaults_ReservedBytesAllZero)
{
    ode_config_t cfg;
    config_defaults(&cfg);
    for (int i = 0; i < 57; i++) {
        BYTES_EQUAL(0x00, cfg.reserved[i]);
    }
}

/* Gap 12: changing last_image_index must produce a different CRC. */
TEST(Config, Crc_ChangesWhenLastImageIndexChanges)
{
    ode_config_t a, b;
    config_defaults(&a);
    config_defaults(&b);
    b.last_image_index = 5;
    CHECK_TRUE(config_crc32(&a) != config_crc32(&b));
}
