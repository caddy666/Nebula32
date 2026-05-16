// =============================================================================
// test_ecc.cpp — CD-ROM Mode 1 EDC/ECC computation tests
//
// Intent: verify that src/ecc.c correctly computes and validates the Error
// Detection Code (EDC) appended to every Mode 1 raw sector (2352 bytes).
//
// Key invariants under test:
//   - Round-trip: ecc_write_edc() followed by ecc_verify_edc() must pass for
//     any valid Mode 1 sector payload.
//   - Single-bit corruption anywhere in bytes 0-2063 must cause verify to fail.
//   - Sectors built with the standard 12-byte sync header + MSF header +
//     mode byte are accepted; the EDC covers bytes 0-2063 (header + user data).
//   - ecc_verify_edc() returns false for a zeroed (unwritten) sector.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
extern "C" {
#include "ecc.h"
#include "cd_types.h"
#include "disc_image.h"
#include "subcode.h"
}

/* Build a minimal but structurally correct Mode 1 raw sector in buf[2352]. */
static void make_sector(uint8_t *buf, uint32_t lba, uint8_t fill)
{
    static const uint8_t sync[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    memset(buf, 0, 2352);
    memcpy(buf, sync, 12);
    msf_t msf = lba_to_msf(lba);
    buf[12] = msf.minute;
    buf[13] = msf.second;
    buf[14] = msf.frame;
    buf[15] = 0x01;          /* Mode 1 */
    memset(buf + 16, fill, 2048);
}

TEST_GROUP(Ecc) {};

/* Basic round-trip: write EDC then verify. */
TEST(Ecc, WriteVerifyRoundTrip)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0x00);
    ecc_write_edc(sector);
    CHECK_TRUE(ecc_verify_edc(sector));
}

/* Non-trivial payload also passes verify. */
TEST(Ecc, NonTrivialPayloadPassesVerify)
{
    uint8_t sector[2352];
    make_sector(sector, 42, 0xA5);
    ecc_write_edc(sector);
    CHECK_TRUE(ecc_verify_edc(sector));
}

/* EDC field must be non-zero for a non-trivial payload. */
TEST(Ecc, EdcIsNonZeroForNonTrivialPayload)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0xA5);
    ecc_write_edc(sector);
    uint32_t edc = (uint32_t)sector[2064]
                 | ((uint32_t)sector[2065] <<  8)
                 | ((uint32_t)sector[2066] << 16)
                 | ((uint32_t)sector[2067] << 24);
    CHECK_TRUE(edc != 0);
}

/* Flipping one bit in the data area must break the EDC check. */
TEST(Ecc, SingleBitFlipInDataFails)
{
    uint8_t sector[2352];
    make_sector(sector, 100, 0x00);
    ecc_write_edc(sector);
    sector[100] ^= 0x01;
    CHECK_FALSE(ecc_verify_edc(sector));
}

/* Flipping a bit in the sync header must also break the check. */
TEST(Ecc, FlipInSyncFails)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0x00);
    ecc_write_edc(sector);
    sector[1] ^= 0x80;
    CHECK_FALSE(ecc_verify_edc(sector));
}

/* Corrupting the EDC field itself must fail. */
TEST(Ecc, CorruptingEdcFieldFails)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0x00);
    ecc_write_edc(sector);
    sector[2064] ^= 0xFF;
    CHECK_FALSE(ecc_verify_edc(sector));
}

/* ecc_write_edc must zero the intermediate field bytes [2068..2075]. */
TEST(Ecc, IntermediateFieldZeroedByWrite)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0xFF);
    ecc_write_edc(sector);
    for (int i = 2068; i < 2076; i++) {
        BYTES_EQUAL(0x00, sector[i]);
    }
}

/* P-parity region [2076..2247] must have at least one non-zero byte after
   ecc_sector_complete(). */
TEST(Ecc, PParityPopulatedByComplete)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0x00);
    ecc_sector_complete(sector);
    bool parity_nonzero = false;
    for (int i = 2076; i < 2248; i++) {
        if (sector[i]) { parity_nonzero = true; break; }
    }
    CHECK_TRUE(parity_nonzero);
}

/* Different payloads must produce different EDC values. */
TEST(Ecc, DifferentPayloadsDifferentEdc)
{
    uint8_t sec_a[2352], sec_b[2352];
    make_sector(sec_a, 0, 0x00);
    make_sector(sec_b, 0, 0xFF);
    ecc_write_edc(sec_a);
    ecc_write_edc(sec_b);

    uint32_t edc_a = (uint32_t)sec_a[2064] | ((uint32_t)sec_a[2065] <<  8)
                   | ((uint32_t)sec_a[2066] << 16) | ((uint32_t)sec_a[2067] << 24);
    uint32_t edc_b = (uint32_t)sec_b[2064] | ((uint32_t)sec_b[2065] <<  8)
                   | ((uint32_t)sec_b[2066] << 16) | ((uint32_t)sec_b[2067] << 24);
    CHECK_TRUE(edc_a != edc_b);
}

/* Different LBAs (different header MSF) must produce different EDC values. */
TEST(Ecc, LbaVariationProducesDifferentEdc)
{
    uint8_t sec_a[2352], sec_b[2352];
    make_sector(sec_a, 0,   0xAA);
    make_sector(sec_b, 100, 0xAA);
    ecc_write_edc(sec_a);
    ecc_write_edc(sec_b);

    uint32_t edc_a = (uint32_t)sec_a[2064] | ((uint32_t)sec_a[2065] <<  8)
                   | ((uint32_t)sec_a[2066] << 16) | ((uint32_t)sec_a[2067] << 24);
    uint32_t edc_b = (uint32_t)sec_b[2064] | ((uint32_t)sec_b[2065] <<  8)
                   | ((uint32_t)sec_b[2066] << 16) | ((uint32_t)sec_b[2067] << 24);
    CHECK_TRUE(edc_a != edc_b);
}

/* ---------------------------------------------------------------------------
 * Scenario 9/10: sync corruption and header flip
 * The EDC covers bytes 0-2063 (sync + MSF header + mode + user data).
 * A single-bit flip anywhere in that range must break the check.
 * Embedding a valid sync pattern inside the user data must NOT confuse the
 * checker — it verifies the checksum, not the structure.
 * -------------------------------------------------------------------------- */

/* Flipping a byte in the MSF header (bytes 12-14) must break the EDC check. */
TEST(Ecc, FlipInMsfHeader_Fails)
{
    uint8_t sector[2352];
    make_sector(sector, 100, 0xAA);
    ecc_write_edc(sector);
    sector[12] ^= 0x01;   /* corrupt MSF minute byte */
    CHECK_FALSE(ecc_verify_edc(sector));
}

/* Flipping the last data byte (byte 2063) must also fail. */
TEST(Ecc, FlipAtDataPayloadEnd_Fails)
{
    uint8_t sector[2352];
    make_sector(sector, 0, 0x00);
    ecc_write_edc(sector);
    sector[2063] ^= 0x80;   /* last user-data byte before EDC field */
    CHECK_FALSE(ecc_verify_edc(sector));
}

/* Embedding the 12-byte CD sync pattern inside the user data payload must NOT
 * cause ecc_verify_edc to fail — the EDC covers content, not structure. */
TEST(Ecc, SyncPatternInPayload_PassesEdc)
{
    static const uint8_t sync[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    uint8_t sector[2352];
    make_sector(sector, 0, 0x5A);
    /* Overwrite first 12 bytes of user-data area with the sync pattern */
    memcpy(sector + 16, sync, 12);
    ecc_write_edc(sector);
    CHECK_TRUE(ecc_verify_edc(sector));   /* EDC passes — content, not structure */
}

/* =============================================================================
 * SectorLayout — disc_synthesise_sector() output structure
 *
 * selftest.c contained these checks as on-target tests; they belong here
 * because disc_synthesise_sector() is pure (no FatFS I/O) and the stub in
 * disc_image_stub.c now carries the real implementation.
 *
 * Invariants under test:
 *   - Sync pattern (bytes 0-11) is exactly the Red Book value.
 *   - MSF header (bytes 12-14) matches lba_to_msf() output (BCD, +150 offset).
 *   - Mode byte (byte 15) is 0x01 (Mode 1).
 *   - Data payload (bytes 16-2063) equals the 2048 bytes passed in.
 * ========================================================================== */

TEST_GROUP(SectorLayout) {};

TEST(SectorLayout, SyncPattern_IsCorrect)
{
    static const uint8_t expected[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    uint8_t sector[2352] = {0};
    uint8_t data[2048];
    memset(data, 0x00, sizeof(data));
    disc_synthesise_sector(sector, 0, data);
    CHECK(memcmp(sector, expected, 12) == 0);
}

TEST(SectorLayout, MsfHeader_Lba0_Is000200)
{
    /* LBA 0 + 150-frame lead-in = absolute frame 150 = 00:02:00 (BCD) */
    uint8_t sector[2352] = {0};
    uint8_t data[2048] = {0};
    disc_synthesise_sector(sector, 0, data);
    BYTES_EQUAL(0x00, sector[12]);  /* minute */
    BYTES_EQUAL(0x02, sector[13]);  /* second */
    BYTES_EQUAL(0x00, sector[14]);  /* frame  */
}

TEST(SectorLayout, MsfHeader_Lba150_Is000400)
{
    /* LBA 150 + 150 = 300 frames = 4 seconds absolute = 00:04:00 (BCD) */
    uint8_t sector[2352] = {0};
    uint8_t data[2048] = {0};
    disc_synthesise_sector(sector, 150, data);
    BYTES_EQUAL(0x00, sector[12]);
    BYTES_EQUAL(0x04, sector[13]);
    BYTES_EQUAL(0x00, sector[14]);
}

TEST(SectorLayout, ModeByte_Is0x01)
{
    uint8_t sector[2352] = {0};
    uint8_t data[2048] = {0};
    disc_synthesise_sector(sector, 0, data);
    BYTES_EQUAL(0x01, sector[15]);
}

TEST(SectorLayout, DataPayload_CopiedCorrectly)
{
    uint8_t sector[2352] = {0};
    uint8_t data[2048];
    memset(data, 0xA5, sizeof(data));
    disc_synthesise_sector(sector, 0, data);
    CHECK(memcmp(sector + 16, data, 2048) == 0);
}
