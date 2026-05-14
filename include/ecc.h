#pragma once
// =============================================================================
// ecc.h — CD-ROM Mode 1 EDC and ECC computation
// =============================================================================
//
// Provides correct EDC (Error Detection Code) and ECC (P/Q parity) for
// Mode 1 sectors synthesised from 2048-byte ISO images.
//
// The CD32 BIOS does not normally verify EDC/ECC — the CXD2545Q chip
// handles error correction transparently.  However, software that reads raw
// sectors via READS (0x1B) may check these fields.
//
// Usage after disc_synthesise_sector():
//   ecc_sector_complete(buf);   // Writes EDC and P/Q parity in one call
//
// Or separately:
//   ecc_write_edc(buf);         // Compute and write EDC at bytes 2064-2067
//   ecc_generate(buf);          // Compute and write P/Q parity at 2076-2351
// =============================================================================


#include <stdint.h>
#include <stdbool.h>

// Compute and write the 32-bit EDC into sector[2064..2067].
// Also zeroes the intermediate field sector[2068..2075].
// 'sector' must point to a 2352-byte buffer with bytes 0–2063 already filled.
void ecc_write_edc(uint8_t *sector);

// Compute and write P-parity (sector[2076..2247]) and
// Q-parity (sector[2248..2351]).
// ecc_write_edc() must be called first.
void ecc_generate(uint8_t *sector);

// Combined convenience function: writes EDC then P/Q ECC.
void ecc_sector_complete(uint8_t *sector_2352);

// Verify the EDC of an existing raw sector. Returns true if valid.
bool ecc_verify_edc(const uint8_t *sector);

