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
        crc ^= (uint16_t)((unsigned)data[i] << 8U);

        // Process each bit
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000U) {
                // MSB set: shift left and XOR with polynomial
                crc = (uint16_t)(((unsigned)crc << 1U) ^ 0x1021U);
            } else {
                // MSB clear: just shift left
                crc = (uint16_t)(crc << 1U);
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
    buf[0] = (uint8_t)((ctrl << 4U) | Q_ADR_POSITION);

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
        uint32_t rf = rel_lba % 75U;
        uint32_t rs = (rel_lba / 75U) % 60U;
        uint32_t rm = (rel_lba / 75U) / 60U;
        buf[3] = (uint8_t)(((rm / 10U) << 4) | (rm % 10U));
        buf[4] = (uint8_t)(((rs / 10U) << 4) | (rs % 10U));
        buf[5] = (uint8_t)(((rf / 10U) << 4) | (rf % 10U));
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

// Q-channel MCN (ADR=2) and ISRC (ADR=3) modes are optional per Red Book and
// carry no data on this ODE — no disc-image format we parse provides an MCN or
// ISRC — so they are not generated.  Omitting them is spec-compliant (you must
// not advertise a catalog number / recording code you do not have) and the CD32
// akiko does not depend on either.  Removed with the unused builders.

