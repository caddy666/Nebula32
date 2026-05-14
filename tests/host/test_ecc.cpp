#include <CppUTest/TestHarness.h>
#include <string.h>
extern "C" {
#include "ecc.h"
#include "cd_types.h"
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
