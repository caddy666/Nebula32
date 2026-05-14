#include "test_runner.h"
#include "ecc.h"
#include "cd_types.h"
#include <string.h>
#include <stdint.h>

// Build a minimal but realistic Mode 1 sector in buf[2352].
// Sync pattern + MSF for LBA 0 + mode byte + 2048 bytes of payload.
static void make_sector(uint8_t *buf, uint32_t lba, uint8_t fill) {
    memset(buf, 0, 2352);

    // Sync
    const uint8_t sync[12] = {
        0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00
    };
    memcpy(buf, sync, 12);

    // MSF header
    msf_t msf = lba_to_msf(lba);
    buf[12] = msf.minute;
    buf[13] = msf.second;
    buf[14] = msf.frame;
    buf[15] = 0x01;  // Mode 1

    // Data payload
    memset(buf + 16, fill, 2048);
}

void test_ecc(void) {
    uint8_t sector[2352];

    SUITE("ecc: EDC write + verify round-trip");

    make_sector(sector, 0, 0x00);
    ecc_write_edc(sector);
    ASSERT_TRUE(ecc_verify_edc(sector),
                "ecc_verify_edc passes immediately after ecc_write_edc");

    // Non-trivial payload should produce a non-zero EDC
    make_sector(sector, 0, 0xA5);
    ecc_write_edc(sector);
    uint32_t edc = (uint32_t)sector[2064]
                 | ((uint32_t)sector[2065] << 8)
                 | ((uint32_t)sector[2066] << 16)
                 | ((uint32_t)sector[2067] << 24);
    ASSERT_TRUE(edc != 0, "EDC is non-zero for non-trivial payload");
    ASSERT_TRUE(ecc_verify_edc(sector),
                "ecc_verify_edc passes for 0xA5 filled payload");

    SUITE("ecc: EDC detects corruption");

    // Flip a bit in the data region → verify must fail
    make_sector(sector, 100, 0x00);
    ecc_write_edc(sector);
    sector[100] ^= 0x01;
    ASSERT_FALSE(ecc_verify_edc(sector),
                 "ecc_verify_edc fails after 1-bit flip in data");

    // Flip a bit in the sync region → verify must fail
    make_sector(sector, 100, 0x00);
    ecc_write_edc(sector);
    sector[1] ^= 0x80;
    ASSERT_FALSE(ecc_verify_edc(sector),
                 "ecc_verify_edc fails after 1-bit flip in sync");

    // Corrupt the stored EDC itself → verify must fail
    make_sector(sector, 100, 0x00);
    ecc_write_edc(sector);
    sector[2064] ^= 0xFF;
    ASSERT_FALSE(ecc_verify_edc(sector),
                 "ecc_verify_edc fails after EDC field corrupted");

    SUITE("ecc: intermediate field zeroed");

    make_sector(sector, 0, 0xFF);
    ecc_write_edc(sector);
    int all_zero = 1;
    for (int i = 2068; i < 2076; i++) {
        if (sector[i] != 0x00) { all_zero = 0; break; }
    }
    ASSERT_TRUE(all_zero, "bytes 2068-2075 are zero after ecc_write_edc");

    SUITE("ecc: P-parity region is populated");

    make_sector(sector, 0, 0x00);
    ecc_sector_complete(sector);

    // With a non-trivial sector (has 0xFF bytes in sync, non-zero MSF),
    // the P-parity block must contain at least some non-zero bytes.
    int p_nonzero = 0;
    for (int i = 2076; i < 2248; i++) {
        if (sector[i] != 0x00) { p_nonzero = 1; break; }
    }
    ASSERT_TRUE(p_nonzero, "P-parity bytes 2076-2247 are non-zero after ecc_sector_complete");

    SUITE("ecc: different payloads produce different EDC");

    uint8_t sec_a[2352], sec_b[2352];
    make_sector(sec_a, 0, 0x00);
    make_sector(sec_b, 0, 0xFF);
    ecc_write_edc(sec_a);
    ecc_write_edc(sec_b);
    uint32_t edc_a = (uint32_t)sec_a[2064] | ((uint32_t)sec_a[2065] << 8)
                   | ((uint32_t)sec_a[2066] << 16) | ((uint32_t)sec_a[2067] << 24);
    uint32_t edc_b = (uint32_t)sec_b[2064] | ((uint32_t)sec_b[2065] << 8)
                   | ((uint32_t)sec_b[2066] << 16) | ((uint32_t)sec_b[2067] << 24);
    ASSERT_TRUE(edc_a != edc_b,
                "EDC differs for 0x00-filled vs 0xFF-filled payloads");
}
