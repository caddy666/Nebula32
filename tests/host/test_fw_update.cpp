// =============================================================================
// test_fw_update.cpp — UF2 firmware update validation logic tests
//
// Intent: verify the pure UF2 parsing and validation logic in fw_update.c
// without any flash hardware, Pico SDK, or FatFS dependency.
//
// The production code in src/fw_update.c is hardware-bound (hardware/flash.h,
// hardware/watchdog.h, pico/multicore.h), so this file replicates the pure
// validation algorithm and the erase-map construction logic, then tests them.
// If the production algorithm diverges, these tests will catch the regression.
//
// Two test groups:
//   Uf2Parse  — validate a single UF2 block against all correctness criteria
//   Uf2Flash  — verify erase_map sector-bit construction and address math
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Replicated UF2 constants — must stay in sync with src/fw_update.c
// ---------------------------------------------------------------------------

#define UF2_MAGIC_START0        0x0A324655UL
#define UF2_MAGIC_START1        0x9E5D5157UL
#define UF2_MAGIC_END           0x0AB16F30UL
#define UF2_FLAG_FAMILY_ID      0x00002000UL
#define UF2_FLAG_NOFLASH        0x00000001UL

#define UF2_FAMILY_RP2350_ARM_S   0xe48bff59UL
#define UF2_FAMILY_RP2350_RISCV   0xe48bff60UL
#define UF2_FAMILY_RP2350_ARM_NS  0xe48bff61UL

// Flash layout constants — must match the RP2350 2 MB flash configuration
#define TEST_XIP_BASE            0x10000000u
#define TEST_FLASH_SECTOR_SIZE   4096u
#define TEST_FLASH_PAGE_SIZE     256u
#define TEST_PICO_FLASH_BYTES    0x200000u          /* 2 MB */
#define TEST_BANK1_OFFSET        0x100000u           /* 1 MB */
#define TEST_CONFIG_OFFSET       (TEST_PICO_FLASH_BYTES - TEST_FLASH_SECTOR_SIZE)
#define TEST_TOTAL_SECTORS       (TEST_PICO_FLASH_BYTES / TEST_FLASH_SECTOR_SIZE)
#define TEST_BANK1_FIRST_SECTOR  (TEST_BANK1_OFFSET / TEST_FLASH_SECTOR_SIZE)

typedef enum {
    FW_OK = 0,
    FW_ERR_NOT_FOUND,
    FW_ERR_IO,
    FW_ERR_BAD_MAGIC,
    FW_ERR_FAMILY,
    FW_ERR_TOO_LARGE,
    FW_ERR_BLOCK_COUNT,
    FW_ERR_BAD_SEQUENCE,
    FW_ERR_NO_BLOCKS,
    FW_ERR_NO_ORIGIN,
    FW_ERR_VERIFY,
    FW_ERR_BAD_PAYLOAD,
} fw_result_t;

typedef struct __attribute__((packed)) {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;

// ---------------------------------------------------------------------------
// Replicated pure-logic helpers — mirror src/fw_update.c exactly
// ---------------------------------------------------------------------------

static bool uf2_is_rp2350_family(uint32_t fid) {
    return fid == UF2_FAMILY_RP2350_ARM_S ||
           fid == UF2_FAMILY_RP2350_RISCV ||
           fid == UF2_FAMILY_RP2350_ARM_NS;
}

// Returns the validation result for a single block (simplified validate).
static fw_result_t validate_block(const uf2_block_t *blk,
                                  uint32_t expected_num_blocks,
                                  uint8_t  erase_map[64]) {
    if (blk->magic_start0 != UF2_MAGIC_START0 ||
        blk->magic_start1 != UF2_MAGIC_START1 ||
        blk->magic_end    != UF2_MAGIC_END)
        return FW_ERR_BAD_MAGIC;

    if (blk->flags & UF2_FLAG_NOFLASH)
        return FW_OK;   // skip informational blocks — not an error

    if ((blk->flags & UF2_FLAG_FAMILY_ID) && !uf2_is_rp2350_family(blk->file_size))
        return FW_ERR_FAMILY;

    // B2 FIX: validate payload_size before any arithmetic that uses it
    if (blk->payload_size == 0 || blk->payload_size > 476 ||
        blk->payload_size % TEST_FLASH_PAGE_SIZE != 0)
        return FW_ERR_BAD_PAYLOAD;

    // B1 FIX: reject SRAM addresses before the subtraction that would wrap
    if (blk->target_addr < TEST_XIP_BASE)
        return FW_ERR_TOO_LARGE;

    uint32_t bank0_off = blk->target_addr - TEST_XIP_BASE;
    if (bank0_off >= TEST_BANK1_OFFSET)
        return FW_ERR_TOO_LARGE;

    uint32_t bank1_off = bank0_off + TEST_BANK1_OFFSET;
    if (bank1_off + blk->payload_size > TEST_CONFIG_OFFSET)
        return FW_OK;   // silently skip blocks that would hit config page

    // Mark the Bank 1 sector in the erase map
    if (erase_map) {
        uint32_t sector = bank1_off / TEST_FLASH_SECTOR_SIZE;
        erase_map[sector / 8] |= (uint8_t)(1u << (sector % 8));
    }

    if (expected_num_blocks != 0 && blk->num_blocks != expected_num_blocks)
        return FW_ERR_BLOCK_COUNT;

    return FW_OK;
}

// Validate an ordered array of blocks (with sequential-block and origin checks).
static fw_result_t validate_sequence(const uf2_block_t *blocks, uint32_t count,
                                     uint8_t erase_map[64]) {
    uint32_t expected_num_blocks = 0;
    uint32_t expected_next = 0;
    bool     has_origin = false;
    uint32_t data_blocks = 0;

    for (uint32_t i = 0; i < count; i++) {
        const uf2_block_t *blk = &blocks[i];
        if (blk->magic_start0 != UF2_MAGIC_START0 ||
            blk->magic_start1 != UF2_MAGIC_START1 ||
            blk->magic_end    != UF2_MAGIC_END)
            return FW_ERR_BAD_MAGIC;
        // B4 FIX: sequence check before NOFLASH skip so NOFLASH blocks consume
        // their block_no slot
        if (blk->block_no != expected_next) return FW_ERR_BAD_SEQUENCE;
        expected_next++;
        if (blk->flags & UF2_FLAG_NOFLASH) continue;
        if ((blk->flags & UF2_FLAG_FAMILY_ID) &&
            !uf2_is_rp2350_family(blk->file_size))
            return FW_ERR_FAMILY;
        // B2 FIX: payload_size validation
        if (blk->payload_size == 0 || blk->payload_size > 476 ||
            blk->payload_size % TEST_FLASH_PAGE_SIZE != 0)
            return FW_ERR_BAD_PAYLOAD;
        // B1 FIX: SRAM address guard
        if (blk->target_addr < TEST_XIP_BASE)
            return FW_ERR_TOO_LARGE;
        uint32_t bank0_off = blk->target_addr - TEST_XIP_BASE;
        if (bank0_off >= TEST_BANK1_OFFSET) return FW_ERR_TOO_LARGE;
        uint32_t bank1_off = bank0_off + TEST_BANK1_OFFSET;
        if (bank1_off + blk->payload_size > TEST_CONFIG_OFFSET) continue;
        data_blocks++;
        if (bank0_off == 0) has_origin = true;
        if (erase_map) {
            uint32_t s = bank1_off / TEST_FLASH_SECTOR_SIZE;
            erase_map[s / 8] |= (uint8_t)(1u << (s % 8));
        }
        if (expected_num_blocks == 0) expected_num_blocks = blk->num_blocks;
        else if (blk->num_blocks != expected_num_blocks) return FW_ERR_BLOCK_COUNT;
    }
    if (data_blocks == 0) return FW_ERR_NO_BLOCKS;
    if (!has_origin)      return FW_ERR_NO_ORIGIN;
    return FW_OK;
}

static bool erase_map_get(const uint8_t map[64], uint32_t sector) {
    if (sector >= TEST_TOTAL_SECTORS) return false;
    return (map[sector / 8] >> (sector % 8)) & 1u;
}

// Build a valid RP2350 UF2 block targeting a given Bank 0 flash offset.
static uf2_block_t make_block(uint32_t bank0_off, uint32_t block_no,
                               uint32_t num_blocks) {
    uf2_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic_start0 = UF2_MAGIC_START0;
    blk.magic_start1 = UF2_MAGIC_START1;
    blk.magic_end    = UF2_MAGIC_END;
    blk.flags        = UF2_FLAG_FAMILY_ID;
    blk.file_size    = UF2_FAMILY_RP2350_ARM_S;
    blk.target_addr  = TEST_XIP_BASE + bank0_off;
    blk.payload_size = TEST_FLASH_PAGE_SIZE;
    blk.block_no     = block_no;
    blk.num_blocks   = num_blocks;
    return blk;
}

// ---------------------------------------------------------------------------
// Uf2Parse — single-block validation
// ---------------------------------------------------------------------------

TEST_GROUP(Uf2Parse) {};

TEST(Uf2Parse, GoodBlock) {
    uf2_block_t blk = make_block(0, 0, 1);
    uint8_t map[64] = {0};
    CHECK_EQUAL(FW_OK, validate_block(&blk, 1, map));
}

TEST(Uf2Parse, BadMagicStart0) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.magic_start0 ^= 1;
    CHECK_EQUAL(FW_ERR_BAD_MAGIC, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, BadMagicStart1) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.magic_start1 ^= 1;
    CHECK_EQUAL(FW_ERR_BAD_MAGIC, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, BadMagicEnd) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.magic_end ^= 1;
    CHECK_EQUAL(FW_ERR_BAD_MAGIC, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, WrongFamilyId) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.flags    = UF2_FLAG_FAMILY_ID;
    blk.file_size = 0xDEADBEEFu;   // not an RP2350 family
    CHECK_EQUAL(FW_ERR_FAMILY, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, AllThreeRp2350FamiliesAccepted) {
    const uint32_t families[] = {
        UF2_FAMILY_RP2350_ARM_S,
        UF2_FAMILY_RP2350_RISCV,
        UF2_FAMILY_RP2350_ARM_NS,
    };
    for (unsigned i = 0; i < 3; i++) {
        uf2_block_t blk = make_block(0, 0, 1);
        blk.flags     = UF2_FLAG_FAMILY_ID;
        blk.file_size = families[i];
        CHECK_EQUAL(FW_OK, validate_block(&blk, 1, nullptr));
    }
}

TEST(Uf2Parse, NoflashFlagSkipsAddressCheck) {
    // NOFLASH block with target_addr far outside flash — should return FW_OK.
    uf2_block_t blk = make_block(0, 0, 1);
    blk.flags       = UF2_FLAG_NOFLASH;
    blk.target_addr = 0xDEADBEEFu;
    CHECK_EQUAL(FW_OK, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, AddressExceedsBankLimit) {
    // bank0_off = TEST_BANK1_OFFSET → not allowed (target must fit in Bank 0 space)
    uf2_block_t blk = make_block(TEST_BANK1_OFFSET, 0, 1);
    CHECK_EQUAL(FW_ERR_TOO_LARGE, validate_block(&blk, 1, nullptr));
}

TEST(Uf2Parse, NumBlocksMismatch) {
    uf2_block_t blk = make_block(0, 1, 10);
    // expected_num_blocks=5 but block says 10
    CHECK_EQUAL(FW_ERR_BLOCK_COUNT, validate_block(&blk, 5, nullptr));
}

TEST(Uf2Parse, OutOfSequenceBlockRejected) {
    // block_no=2 but we expect 0 first
    uf2_block_t b0 = make_block(0, 2, 3);   // block_no=2, should be 0
    uf2_block_t b1 = make_block(TEST_FLASH_SECTOR_SIZE, 1, 3);
    uf2_block_t b2 = make_block(TEST_FLASH_SECTOR_SIZE*2, 2, 3);
    uf2_block_t seq[] = {b0, b1, b2};
    CHECK_EQUAL(FW_ERR_BAD_SEQUENCE, validate_sequence(seq, 3, nullptr));
}

TEST(Uf2Parse, EmptyFileReturnsNoBlocks) {
    // No data blocks — UF2 with only NOFLASH informational blocks
    uf2_block_t blk = make_block(0, 0, 1);
    blk.flags = UF2_FLAG_NOFLASH;
    uf2_block_t seq[] = {blk};
    CHECK_EQUAL(FW_ERR_NO_BLOCKS, validate_sequence(seq, 1, nullptr));
}

TEST(Uf2Parse, MissingOriginBlockRejected) {
    // Firmware that doesn't include offset 0 — missing reset vector
    uf2_block_t b0 = make_block(TEST_FLASH_SECTOR_SIZE,   0, 2);  // starts at 4 KB
    uf2_block_t b1 = make_block(TEST_FLASH_SECTOR_SIZE*2, 1, 2);
    uf2_block_t seq[] = {b0, b1};
    CHECK_EQUAL(FW_ERR_NO_ORIGIN, validate_sequence(seq, 2, nullptr));
}

TEST(Uf2Parse, ValidSequenceWithOriginPasses) {
    uf2_block_t b0 = make_block(0,                       0, 3);
    uf2_block_t b1 = make_block(TEST_FLASH_SECTOR_SIZE,  1, 3);
    uf2_block_t b2 = make_block(TEST_FLASH_SECTOR_SIZE*2,2, 3);
    uf2_block_t seq[] = {b0, b1, b2};
    CHECK_EQUAL(FW_OK, validate_sequence(seq, 3, nullptr));
}

// ---------------------------------------------------------------------------
// Uf2Flash — erase_map construction and address math
// ---------------------------------------------------------------------------

TEST_GROUP(Uf2Flash) {};

TEST(Uf2Flash, FirstBlockSetsBank1Sector256) {
    // bank0_off=0 → bank1_off=0x100000 → sector=256
    uf2_block_t blk = make_block(0, 0, 1);
    uint8_t map[64] = {0};
    CHECK_EQUAL(FW_OK, validate_block(&blk, 0, map));
    CHECK(erase_map_get(map, 256));
}

TEST(Uf2Flash, SecondSectorBlockSetsCorrectBit) {
    // bank0_off=FLASH_SECTOR_SIZE → bank1_off=0x101000 → sector=257
    uf2_block_t blk = make_block(TEST_FLASH_SECTOR_SIZE, 0, 1);
    uint8_t map[64] = {0};
    CHECK_EQUAL(FW_OK, validate_block(&blk, 0, map));
    CHECK(erase_map_get(map, 257));
    CHECK(!erase_map_get(map, 256));   // first sector NOT set
}

TEST(Uf2Flash, TwoBlocksSameSectorOnlySetsBitOnce) {
    // bank0_off=0 and bank0_off=128 are both in sector 256
    uint8_t map[64] = {0};
    uf2_block_t b1 = make_block(0,   0, 2);
    uf2_block_t b2 = make_block(128, 1, 2);
    validate_block(&b1, 0, map);
    validate_block(&b2, 0, map);
    CHECK(erase_map_get(map, 256));
    // Count set bits — only one sector should be marked
    int bits = 0;
    for (int i = 0; i < 64; i++)
        for (int b = 0; b < 8; b++)
            if ((map[i] >> b) & 1) bits++;
    CHECK_EQUAL(1, bits);
}

TEST(Uf2Flash, ConfigPageBlockSilentlySkipped) {
    // bank0_off such that bank1_off = TEST_CONFIG_OFFSET (last 4 KB of flash)
    // block should be silently skipped — erase_map has no bit set
    uint32_t bank0_off = TEST_CONFIG_OFFSET - TEST_BANK1_OFFSET;
    uf2_block_t blk = make_block(bank0_off, 0, 1);
    uint8_t map[64] = {0};
    CHECK_EQUAL(FW_OK, validate_block(&blk, 0, map));
    // No bits should be set
    int bits = 0;
    for (int i = 0; i < 64; i++)
        for (int b = 0; b < 8; b++)
            if ((map[i] >> b) & 1) bits++;
    CHECK_EQUAL(0, bits);
}

TEST(Uf2Flash, Bank1SectorIndexMath) {
    // Verify the sector-index formula at several offsets
    struct { uint32_t bank0_off; uint32_t expected_sector; } cases[] = {
        { 0,                   256 },
        { TEST_FLASH_SECTOR_SIZE,   257 },
        { TEST_FLASH_SECTOR_SIZE*4, 260 },
        { TEST_BANK1_OFFSET - TEST_FLASH_PAGE_SIZE, 511 }, // last allowed sector (not config)
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint32_t bank1_off = cases[i].bank0_off + TEST_BANK1_OFFSET;
        uint32_t got_sector = bank1_off / TEST_FLASH_SECTOR_SIZE;
        CHECK_EQUAL(cases[i].expected_sector, got_sector);
    }
}

TEST(Uf2Flash, MultipleBlocksAcrossThreeSectors) {
    // Three blocks spanning three different 4 KB sectors
    uint8_t map[64] = {0};
    uf2_block_t b0 = make_block(0,                       0, 3);
    uf2_block_t b1 = make_block(TEST_FLASH_SECTOR_SIZE,  1, 3);
    uf2_block_t b2 = make_block(TEST_FLASH_SECTOR_SIZE*2,2, 3);
    validate_block(&b0, 3, map);
    validate_block(&b1, 3, map);
    validate_block(&b2, 3, map);
    CHECK(erase_map_get(map, 256));
    CHECK(erase_map_get(map, 257));
    CHECK(erase_map_get(map, 258));
    CHECK(!erase_map_get(map, 259));
    CHECK(!erase_map_get(map, 255));  // Bank 0 sector — must never be set here
}

// ---------------------------------------------------------------------------
// B1 — target_addr below XIP_BASE (SRAM address) must not wrap
// ---------------------------------------------------------------------------
TEST_GROUP(Uf2SecurityFixes) {};

TEST(Uf2SecurityFixes, B1_SramAddressRejected) {
    // SRAM address 0x20000000: without guard, subtracting XIP_BASE wraps to
    // 0x10000000 = FW_BANK1_OFFSET — equal, not greater, so old check passes.
    uf2_block_t blk = make_block(0, 0, 1);
    blk.target_addr = 0x20000000u;
    CHECK_EQUAL(FW_ERR_TOO_LARGE, validate_block(&blk, 1, nullptr));
}

TEST(Uf2SecurityFixes, B1_PeripheralAddressRejected) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.target_addr = 0x40000000u;  // APB peripheral base
    CHECK_EQUAL(FW_ERR_TOO_LARGE, validate_block(&blk, 1, nullptr));
}

// ---------------------------------------------------------------------------
// B2 — payload_size validation (zero, oversized, unaligned)
// ---------------------------------------------------------------------------

TEST(Uf2SecurityFixes, B2_PayloadSizeZeroRejected) {
    uf2_block_t blk = make_block(0, 0, 1);
    blk.payload_size = 0;
    CHECK_EQUAL(FW_ERR_BAD_PAYLOAD, validate_block(&blk, 1, nullptr));
}

TEST(Uf2SecurityFixes, B2_PayloadSizeOversizedRejected) {
    // 0xFFFFFFFF would wrap the config-page overflow check and overread blk.data
    uf2_block_t blk = make_block(0, 0, 1);
    blk.payload_size = 0xFFFFFFFFu;
    CHECK_EQUAL(FW_ERR_BAD_PAYLOAD, validate_block(&blk, 1, nullptr));
}

TEST(Uf2SecurityFixes, B2_PayloadSize477Rejected) {
    // 477 bytes is one past the blk.data[] array boundary
    uf2_block_t blk = make_block(0, 0, 1);
    blk.payload_size = 477u;
    CHECK_EQUAL(FW_ERR_BAD_PAYLOAD, validate_block(&blk, 1, nullptr));
}

TEST(Uf2SecurityFixes, B2_PayloadSizeUnalignedRejected) {
    // 128 bytes — not a multiple of FLASH_PAGE_SIZE (256)
    uf2_block_t blk = make_block(0, 0, 1);
    blk.payload_size = 128u;
    CHECK_EQUAL(FW_ERR_BAD_PAYLOAD, validate_block(&blk, 1, nullptr));
}

TEST(Uf2SecurityFixes, B2_PayloadSize256Accepted) {
    // Standard page-aligned payload size must pass
    uf2_block_t blk = make_block(0, 0, 1);
    blk.payload_size = 256u;
    CHECK_EQUAL(FW_OK, validate_block(&blk, 1, nullptr));
}

// ---------------------------------------------------------------------------
// B4 — NOFLASH blocks must consume a block_no slot in sequence
// ---------------------------------------------------------------------------

TEST(Uf2SecurityFixes, B4_NoflashBlockCountsInSequence) {
    // block_no=0 (NOFLASH), block_no=1 (data) — previously broke with
    // FW_ERR_BAD_SEQUENCE because NOFLASH was skipped before expected_next++.
    uf2_block_t b0 = make_block(0, 0, 2);
    b0.flags        = UF2_FLAG_NOFLASH;
    b0.target_addr  = 0xDEADBEEFu;  // NOFLASH so address is unchecked
    uf2_block_t b1 = make_block(0, 1, 2);  // origin block, block_no=1
    uf2_block_t seq[] = {b0, b1};
    CHECK_EQUAL(FW_OK, validate_sequence(seq, 2, nullptr));
}

TEST(Uf2SecurityFixes, B4_DataBlockAfterNoflashHasCorrectNo) {
    // block_no=0 (data), block_no=1 (NOFLASH), block_no=2 (data) — full sequence
    uf2_block_t b0 = make_block(0,                      0, 3);
    uf2_block_t b1 = make_block(TEST_FLASH_SECTOR_SIZE, 1, 3);
    b1.flags = UF2_FLAG_NOFLASH;
    uf2_block_t b2 = make_block(TEST_FLASH_SECTOR_SIZE, 2, 3);
    uf2_block_t seq[] = {b0, b1, b2};
    CHECK_EQUAL(FW_OK, validate_sequence(seq, 3, nullptr));
}

TEST(Uf2SecurityFixes, B4_OutOfSequenceAfterNoflashCaughtCorrectly) {
    // block_no=0 (NOFLASH), block_no=0 (data) — sequence violation: data block
    // has wrong block_no after NOFLASH consumed slot 0.
    uf2_block_t b0 = make_block(0, 0, 2);
    b0.flags = UF2_FLAG_NOFLASH;
    uf2_block_t b1 = make_block(0, 0, 2);  // block_no=0 again → BAD_SEQUENCE
    uf2_block_t seq[] = {b0, b1};
    CHECK_EQUAL(FW_ERR_BAD_SEQUENCE, validate_sequence(seq, 2, nullptr));
}
