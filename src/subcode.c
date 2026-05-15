// =============================================================================
// subcode.c — CD Q-channel subcode generation
// =============================================================================
//
// Generates the Q-channel subcode bytes that the CXD2545Q would normally
// recover from the physical disc.  The CD32 akiko reads these to:
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
    // Relative time counts from 00:00:00 at the INDEX 01 point — it must NOT
    // include the 150-sector lead-in offset that lba_to_msf() adds for absolute
    // time.  Compute BCD directly from the frame count.
    uint32_t rel_lba;
    if (index == 0 && disc_lba < track_start_lba) {
        rel_lba = track_start_lba - disc_lba;  // pregap: count down to 00:00:00
    } else {
        rel_lba = (disc_lba >= track_start_lba) ? (disc_lba - track_start_lba) : 0;
    }
    {
        uint32_t rf = rel_lba % 75u;
        uint32_t rs = (rel_lba / 75u) % 60u;
        uint32_t rm = (rel_lba / 75u) / 60u;
        buf[3] = (uint8_t)(((rm / 10u) << 4) | (rm % 10u));
        buf[4] = (uint8_t)(((rs / 10u) << 4) | (rs % 10u));
        buf[5] = (uint8_t)(((rf / 10u) << 4) | (rf % 10u));
    }

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
// required by the Red Book specification.  The CD32 akiko does not depend on
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

// ECMA-130 Table 16: 6-bit character codes for ISRC.
// Digits '0'-'9' → 0x00-0x09; letters 'A'-'Z' → 0x11-0x2A.
static uint8_t isrc_encode_char(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 17u);
    return 0;
}

void subcode_build_q_isrc(uint8_t track_no, bool is_data,
                           const char *isrc, uint8_t *buf)
{
    (void)track_no;   /* not used in ISRC frames — byte 9 carries ISRC tail bits */

    uint8_t ctrl = is_data ? Q_CTRL_DATA : Q_CTRL_AUDIO;
    buf[0] = (ctrl << 4) | Q_ADR_ISRC;

    /* Bytes 1-9: 12 chars × 6 bits = 72 bits, MSB-first — ECMA-130 Table 17 */
    memset(buf + 1, 0, 9);

    if (isrc != NULL) {
        for (int i = 0; i < 12 && isrc[i] != '\0'; i++) {
            uint8_t val     = isrc_encode_char(isrc[i]);
            int     bit_off = i * 6;
            int     byte_off = bit_off / 8;
            int     shift    = 2 - (bit_off % 8);

            if (shift >= 0) {
                buf[1 + byte_off] |= (uint8_t)(val << shift);
            } else {
                buf[1 + byte_off    ] |= (uint8_t)(val >> (-shift));
                buf[1 + byte_off + 1] |= (uint8_t)(val << (8 + shift));
            }
        }
    }

    subcode_append_crc(buf);
}

/* Original implementation kept for reference — three bugs noted:
 *   1. `(uint8_t)isrc[i] & 0x3F` — wrong encoding (ECMA-130 Table 16 is not
 *      the bottom 6 bits of ASCII; digits are 0x00-0x09, letters are 0x11-0x2A).
 *   2. `if (byte_idx < 9)` — drops character 11 (byte_off=8 = buf[9] is valid).
 *   3. `buf[9] = BCD track number` — overwrites the ISRC tail bits; Mode 3
 *      byte 9 carries ISRC data, not track number.
 *
 * void subcode_build_q_isrc_ORIG(uint8_t track_no, bool is_data,
 *                                const char *isrc, uint8_t *buf) {
 *     uint8_t ctrl = is_data ? Q_CTRL_DATA : Q_CTRL_AUDIO;
 *     buf[0] = (ctrl << 4) | Q_ADR_ISRC;
 *     memset(buf + 1, 0, 8);
 *     if (isrc != NULL) {
 *         for (int i = 0; i < 12 && isrc[i] != '\0'; i++) {
 *             uint8_t c = (uint8_t)isrc[i] & 0x3F;
 *             int byte_idx = 1 + (i * 6) / 8;
 *             int bit_shift = 2 - ((i * 6) % 8);
 *             if (byte_idx < 9) {
 *                 if (bit_shift >= 0) {
 *                     buf[byte_idx] |= c << bit_shift;
 *                 } else {
 *                     buf[byte_idx]   |= c >> (-bit_shift);
 *                     buf[byte_idx+1] |= c << (8 + bit_shift);
 *                 }
 *             }
 *         }
 *     }
 *     buf[9] = ((track_no / 10) << 4) | (track_no % 10);
 *     subcode_append_crc(buf);
 * }
 */
