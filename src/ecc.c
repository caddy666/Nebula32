// =============================================================================
// ecc.c — CD-ROM Mode 1 EDC and ECC computation
// =============================================================================
//
// When the CXD2545Q emulator synthesises a full 2352-byte sector from a
// 2048-byte ISO payload (disc_synthesise_sector in disc_image.c), it must
// populate:
//
//   Bytes 2064–2067 : EDC (Error Detection Code)  — 32-bit
//   Bytes 2068–2075 : Intermediate field (zeroes)
//   Bytes 2076–2351 : ECC (Error Correction Code) — P parity + Q parity
//
// Without correct EDC/ECC the sector looks valid to our emulator, and the
// CD32 akiko normally doesn't verify them (the CXD2545Q chip handles error
// correction transparently).  However, some software reads raw sectors via
// READS or checks the EDC to detect read errors.  This module provides
// correct computation.
//
// REFERENCES:
//   ECMA-130 (ISO 10149) Annex B — EDC polynomial
//   ECMA-130 Annex C         — ECC P/Q parity (Reed-Solomon on GF(2^8))
//   Neill Corlett's CDMage    — public domain ECC implementation reference
//   libmirage                 — GPL-2.0 ECC reference
//
// GF(2^8) field:
//   Generator polynomial: x^8 + x^4 + x^3 + x^2 + 1  (0x11D)
//   Primitive element α = 0x02
// =============================================================================

#include "ecc.h"
#include <string.h>

// ---------------------------------------------------------------------------
// GF(2^8) lookup tables
// ---------------------------------------------------------------------------
// Pre-computed exp and log tables for GF(2^8) with polynomial 0x11D.
// exp_table[i] = α^i mod p(x)
// log_table[v] = i such that α^i = v

static uint8_t gf_exp[512];  // Extended to 512 to avoid modulo in multiply
static uint8_t gf_log[256];
static bool    gf_tables_ready = false;

static void gf_init(void) {
    if (gf_tables_ready) return;

    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i]       = (uint8_t)x;
        gf_exp[i + 255] = (uint8_t)x;  // Duplicate for wrap-around
        gf_log[x]       = (uint8_t)i;
        x = (uint16_t)(x << 1U);
        if (x & 0x100U) x ^= 0x11DU;   // Reduce mod p(x) = x^8+x^4+x^3+x^2+1
    }
    gf_exp[510] = gf_exp[0];
    gf_log[0]   = 0;  // log(0) undefined; set to 0 by convention
    gf_tables_ready = true;
}

// GF(2^8) multiply
static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[(int)gf_log[a] + (int)gf_log[b]];
}

// ---------------------------------------------------------------------------
// EDC — Error Detection Code (CRC-32 variant)
// ---------------------------------------------------------------------------
// The EDC polynomial is x^32 + x^31 + x^16 + x^15 + x^4 + x^3 + x^2 + x + 1
// (ECMA-130 Annex B), which in reversed bit order is 0xD8018001.
// The EDC is computed over bytes 0–2063 of the raw sector (sync + header + data).
//
// Table-driven: each entry is the result of running one byte value through
// 8 bit-iterations.  The update step then becomes one table lookup per byte:
//   edc = table[(edc ^ byte) & 0xFF] ^ (edc >> 8)
// Reduces per-sector work from 16 512 bit-loop iterations to 2 064 table lookups.

static uint32_t edc_table[256];
static bool     edc_table_ready = false;

static void edc_table_init(void)
{
    if (edc_table_ready) return;
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)i;
        for (int b = 0; b < 8; b++)
            v = (v & 1) ? (v >> 1) ^ 0xD8018001U : (v >> 1);
        edc_table[i] = v;
    }
    edc_table_ready = true;
}

static uint32_t edc_compute(const uint8_t *data, size_t len)
{
    edc_table_init();
    uint32_t edc = 0;
    for (size_t i = 0; i < len; i++)
        edc = edc_table[(edc ^ data[i]) & 0xFF] ^ (edc >> 8);
    return edc;
}

// ---------------------------------------------------------------------------
// ECC — Error Correction Code (Reed-Solomon P and Q parity)
// ---------------------------------------------------------------------------
// The ECC field contains two interleaved Reed-Solomon codes:
//   P-parity: 86 codewords × 24 bytes (2 parity bytes each) = 172 bytes
//   Q-parity: 52 codewords × 43 bytes (2 parity bytes each) = 104 bytes
//
// The data to protect (2236 bytes) is the sector payload from byte 12 to 2075:
//   [MSF(3) + mode(1) + data(2048) + EDC(4) + zeroes(8)] = 2064 bytes
// plus the EDC zero field, arranged as a 43×24 matrix for the interleave.
//
// Rather than implementing the full interleaved RS encoder, we use the
// standard M-by-N matrix approach documented in ECMA-130.

// RS(24, 22) P-parity encoder: 24-byte codeword, 2 parity bytes.
// Generator polynomial: g(x) = (x - α^0)(x - α^1) = x^2 + (α^0+α^1)x + α^1
//                             = x^2 + 3x + 2  in GF(2^8)
static void rs_p_encode(const uint8_t *data, int stride, int len,
                         uint8_t *p0, uint8_t *p1) {
    uint8_t r0 = 0, r1 = 0;
    for (int i = len - 1; i >= 0; i--) {
        uint8_t feedback = data[i * stride] ^ r1;
        r1 = r0 ^ gf_mul(feedback, 0x02);  // α^1 = 2
        r0 = gf_mul(feedback, 0x03);        // α^0 + α^1 = 3
    }
    *p0 = r0;
    *p1 = r1;
}

// Accessor for the 2236-byte combined ECC data field D (ECMA-130 Annex C).
//   D[0..2063]   = sector[12..2075]  (header + user data + EDC + zeroes)
//   D[2064..2235]= sector[2076..2247] (P parity, already written before Q)
static inline uint8_t _ecc_d(const uint8_t *sector, int n) {
    return (n < 2064) ? sector[12 + n] : sector[2076 + (n - 2064)];
}

// ---------------------------------------------------------------------------
// ecc_generate — populate P and Q parity bytes in a 2352-byte Mode 1 sector
// ---------------------------------------------------------------------------
// 'sector' points to a 2352-byte buffer where:
//   bytes   0–11 : sync
//   bytes  12–15 : MSF + mode
//   bytes  16–2063 : user data (already filled)
//   bytes 2064–2067 : EDC (already computed by ecc_write_edc)
//   bytes 2068–2075 : zero (intermediate field)
//   bytes 2076–2247 : P-parity (86 × 2 bytes)
//   bytes 2248–2351 : Q-parity (52 × 2 bytes)

void ecc_generate(uint8_t *sector) {
    gf_init();

    // The ECC data source is bytes 12–2075 (2064 bytes), arranged as a
    // 2236-byte virtual data stream after zero-padding the EDC and zeroes.
    // Implementation follows the ECMA-130 Annex C matrix layout.

    // P-parity: 86 codewords × 24 data bytes (ECMA-130 Annex C).
    // Codeword col: D[col + row*86] for row = 0..23.
    // D[n] = sector[12+n] (n<2064), so sector[12+col], sector[12+col+86], ...
    // This covers all bytes 12..2075 including EDC (rows 22–23).
    for (int col = 0; col < 86; col++) {
        uint8_t p0, p1;
        rs_p_encode(sector + 12 + col, 86, 24, &p0, &p1);
        sector[2076 + col * 2    ] = p0;
        sector[2076 + col * 2 + 1] = p1;
    }

    // Q-parity: 52 codewords × 43 data bytes with diagonal interleave.
    // Codeword j: D[(j + k*44) % 2236] for k = 0..42.
    // D spans both the data region and the P-parity bytes just written above,
    // so P must be written before Q.
    for (int j = 0; j < 52; j++) {
        uint8_t tmp[43];
        for (int k = 0; k < 43; k++)
            tmp[k] = _ecc_d(sector, (j + k * 44) % 2236);
        uint8_t q0, q1;
        rs_p_encode(tmp, 1, 43, &q0, &q1);
        sector[2248 + j * 2    ] = q0;
        sector[2248 + j * 2 + 1] = q1;
    }
}

// ---------------------------------------------------------------------------
// ecc_write_edc — compute and write the EDC into a 2352-byte sector buffer
// ---------------------------------------------------------------------------
void ecc_write_edc(uint8_t *sector) {
    // EDC covers bytes 0–2063 of the raw sector
    uint32_t edc = edc_compute(sector, 2064);

    // Store little-endian at bytes 2064–2067
    sector[2064] = (uint8_t)(edc        & 0xFF);
    sector[2065] = (uint8_t)((edc >>  8) & 0xFF);
    sector[2066] = (uint8_t)((edc >> 16) & 0xFF);
    sector[2067] = (uint8_t)((edc >> 24) & 0xFF);

    // Intermediate field (bytes 2068–2075) is always zero in Mode 1
    memset(sector + 2068, 0, 8);
}

// ---------------------------------------------------------------------------
// ecc_verify_edc — check the EDC of an existing raw sector
// ---------------------------------------------------------------------------
bool ecc_verify_edc(const uint8_t *sector) {
    uint32_t computed = edc_compute(sector, 2064);
    uint32_t stored   = (uint32_t)sector[2064]
                      | ((uint32_t)sector[2065] << 8)
                      | ((uint32_t)sector[2066] << 16)
                      | ((uint32_t)sector[2067] << 24);
    return computed == stored;
}

// ---------------------------------------------------------------------------
// ecc_sector_complete — synthesise a full Mode 1 sector with correct EDC+ECC
// ---------------------------------------------------------------------------
// Convenience wrapper: writes EDC then generates P/Q parity in one call.
// Call this after disc_synthesise_sector() fills sync, MSF, mode, and data.
void ecc_sector_complete(uint8_t *sector_2352) {
    ecc_write_edc(sector_2352);
    ecc_generate(sector_2352);  /* gf_init() is called inside ecc_generate */
}
