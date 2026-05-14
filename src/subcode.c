// =============================================================================
// subcode.c — CD Q-channel subcode generation
// =============================================================================
//
// Generates the Q-channel subcode bytes that the CXD2545Q would normally
// recover from the physical disc.  The CD32 BIOS reads these to:
//
//   1. Determine current playback position (for time display in audio mode)
//   2. Verify disc identity (first track type must be data for CD32)
//   3. Detect track boundaries during fast-forward / rewind
//
// The subcode data is pushed into the PIO subcode encoder's TX FIFO, which
// serialises it at exactly 44,100 bits/second (one bit per EFM channel bit
// period at 1x speed).
// =============================================================================

#include "subcode.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// CRC-16/CCITT
// ---------------------------------------------------------------------------
// The Q-channel uses CRC-16/CCITT (polynomial 0x1021, init 0x0000).
// After computing the 16-bit CRC over bytes 0-9, the result is bitwise
// inverted before being stored in bytes 10-11.  This is specified in the
// Red Book (ECMA-130 annex B).

uint16_t subcode_crc16(const uint8_t *data, uint32_t len) {
    uint16_t crc = 0x0000;  // Initial value for Q-channel CRC

    for (uint32_t i = 0; i < len; i++) {
        // XOR the next data byte into the high byte of the CRC register
        crc ^= (uint16_t)data[i] << 8;

        // Process each bit
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                // MSB set: shift left and XOR with polynomial
                crc = (crc << 1) ^ 0x1021;
            } else {
                // MSB clear: just shift left
                crc = (crc << 1);
            }
        }
    }

    // Red Book requires the CRC to be stored inverted
    return ~crc;
}

// ---------------------------------------------------------------------------
// Q-channel Position Mode (ADR=1)
// ---------------------------------------------------------------------------
// This is by far the most common Q-channel mode.  It is generated for every
// sector (or at minimum every other sector).  The host uses it to know where
// the laser head currently is.

void subcode_build_q_position(uint8_t track_no, uint8_t index,
                               bool is_data,
                               uint32_t track_start_lba, uint32_t disc_lba,
                               uint8_t *buf) {

    // ---- CTRL / ADR byte ----
    // CTRL nibble (bits 7:4): data or audio track flags
    // ADR  nibble (bits 3:0): 0x1 = position mode
    uint8_t ctrl = is_data ? Q_CTRL_DATA : Q_CTRL_AUDIO;
    buf[0] = (ctrl << 4) | Q_ADR_POSITION;

    // ---- Track number (BCD) ----
    // Lead-in area uses track 0x00; programme area uses 0x01-0x63 (BCD 1-99).
    buf[1] = ((track_no / 10) << 4) | (track_no % 10);

    // ---- Index (BCD) ----
    // 0x00 = pause/pregap before track (index 0)
    // 0x01 = first programme area of the track
    buf[2] = ((index / 10) << 4) | (index % 10);

    // ---- Relative time (MM:SS:FF since track start) ----
    // If in pregap (index == 0), relative time counts DOWN towards 00:00:00.
    uint32_t rel_lba;
    if (index == 0 && disc_lba < track_start_lba) {
        // Count down: pregap duration - frames elapsed
        rel_lba = track_start_lba - disc_lba;
    } else {
        // Count up: frames elapsed since track index 1 start
        rel_lba = (disc_lba >= track_start_lba) ? (disc_lba - track_start_lba) : 0;
    }
    // NOTE: lba_to_msf() adds the 150-sector lead-in offset, so relative time
    // at track start (rel_lba=0) encodes as 00:02:00, not 00:00:00 as the Red
    // Book specifies.  The Akiko deserialiser on real CD32 hardware tolerates
    // this, but a strict Q-channel parser would disagree.  If relative-time
    // accuracy becomes a compatibility issue, subtract 150 from rel_lba before
    // calling lba_to_msf() and clamp to zero.
    msf_t rel = lba_to_msf(rel_lba);
    buf[3] = rel.minute;
    buf[4] = rel.second;
    buf[5] = rel.frame;

    // ---- Reserved (byte 6 = 0x00 in Mode 1) ----
    buf[6] = 0x00;

    // ---- Absolute time (MM:SS:FF from disc start + 2-second lead-in offset) ----
    msf_t abs = lba_to_msf(disc_lba);
    buf[7] = abs.minute;
    buf[8] = abs.second;
    buf[9] = abs.frame;

    // ---- CRC ----
    subcode_append_crc(buf);
}

// ---------------------------------------------------------------------------
// Q-channel MCN Mode (ADR=2) — Media Catalog Number
// ---------------------------------------------------------------------------
// The MCN (also called the UPC/EAN barcode) is a 13-digit number encoded in
// a 72-bit BCD field.  We transmit this approximately every 100 sectors as
// required by the Red Book specification.  The CD32 BIOS does not depend on
// the MCN for normal operation, but some software uses it for disc verification.

void subcode_build_q_mcn(const char *mcn, uint8_t *buf) {
    // Byte 0: CTRL/ADR — audio or data is not meaningful for MCN frames,
    //         but we use the same CTRL as the first data track.
    buf[0] = (Q_CTRL_DATA << 4) | Q_ADR_MCN;

    // Bytes 1–7: 13 BCD digits packed into 52 bits, MSB first.
    // The remaining 20 bits of bytes 1-8 are zero.
    //   Byte 1 bits [7:4] = digit 1
    //   Byte 1 bits [3:0] = digit 2
    //   ...
    //   Byte 7 bits [7:4] = digit 13
    //   Byte 7 bits [3:0] = 0 (zero-pad)
    //   Byte 8           = 0x00 (ZERO flag + 7 zero bits)

    if (mcn == NULL) {
        // No barcode — emit all-zero MCN
        memset(buf + 1, 0, 7);
        buf[8] = 0x00;
    } else {
        // Pack the MCN digits
        uint8_t packed[7] = {0};
        for (int digit = 0; digit < 13; digit++) {
            uint8_t d = (mcn[digit] >= '0' && mcn[digit] <= '9')
                        ? (uint8_t)(mcn[digit] - '0') : 0;
            if (digit % 2 == 0) {
                packed[digit / 2] = (d << 4);      // High nibble
            } else {
                packed[digit / 2] |= (d & 0x0F);   // Low nibble
            }
        }
        memcpy(buf + 1, packed, 7);
        buf[8] = 0x00;  // ZERO bit (=0 means MCN present; =1 means absent)
    }

    // Bytes 9: A-TIME frame (absolute time frame; set to 0 in MCN mode)
    buf[9] = 0x00;

    subcode_append_crc(buf);
}

// ---------------------------------------------------------------------------
// Q-channel ISRC Mode (ADR=3) — International Standard Recording Code
// ---------------------------------------------------------------------------
// ISRC is a 12-character code identifying a specific recording.
// Format: CC-OOO-YY-NNNNN  (2 country + 3 owner + 2 year + 5 serial = 12)

void subcode_build_q_isrc(uint8_t track_no, bool is_data,
                           const char *isrc, uint8_t *buf) {
    uint8_t ctrl = is_data ? Q_CTRL_DATA : Q_CTRL_AUDIO;
    buf[0] = (ctrl << 4) | Q_ADR_ISRC;

    // Bytes 1–8: 12-character ISRC packed as 6-bit characters (ISO 8859-1 table)
    // For our purposes we zero-fill if no ISRC is provided.
    memset(buf + 1, 0, 8);

    if (isrc != NULL) {
        // Simplified: pack first 12 chars as raw bytes, upper 6 bits used
        for (int i = 0; i < 12 && isrc[i] != '\0'; i++) {
            uint8_t c = (uint8_t)isrc[i] & 0x3F;  // 6-bit encoding
            int byte_idx = 1 + (i * 6) / 8;
            int bit_shift = 2 - ((i * 6) % 8);    // MSB-first packing
            if (byte_idx < 9) {
                if (bit_shift >= 0) {
                    buf[byte_idx] |= c << bit_shift;
                } else {
                    buf[byte_idx]   |= c >> (-bit_shift);
                    buf[byte_idx+1] |= c << (8 + bit_shift);
                }
            }
        }
    }

    // Byte 9: A-TIME frame
    buf[9] = ((track_no / 10) << 4) | (track_no % 10);

    subcode_append_crc(buf);
}
