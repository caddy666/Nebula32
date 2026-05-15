#pragma once
// =============================================================================
// subcode.h — CD Subcode Q-channel generation
// =============================================================================
//
// Every raw CD sector carries 98 subcode frames (one per EFM sync block).
// Frames 0–1 are the S0/S1 sync pattern; frames 2–97 form eight channels
// P, Q, R, S, T, U, V, W.  Each channel contributes 1 bit per frame giving
// 96 bits (12 bytes) per channel per sector.
//
// The Q-channel is the only one the CD32 akiko cares about.  It carries:
//   Mode 1 (ADR=1): current track, index, relative time, absolute time
//   Mode 2 (ADR=2): Media Catalog Number (MCN / UPC-EAN barcode)
//   Mode 3 (ADR=3): ISRC code for the current track
//
// We generate Mode 1 Q-channel data for every sector.  Mode 2/3 are emitted
// periodically (every ~100 sectors) as the Red Book specifies, but their
// absence does not cause compatibility problems on real CD32 hardware.
//
// Q-channel data layout (12 bytes, 96 bits):
//   Byte 0  : [CTRL(7:4) | ADR(3:0)]
//               CTRL bits:  bit3=data(1)/audio(0), bit2=copy-permit,
//                           bit1=two-channel(0)/four-channel(1), bit0=pre-emphasis
//               ADR  bits:  0x1 = position mode
//   Byte 1  : Track number (BCD 01-99, 0xAA = lead-out)
//   Byte 2  : Index (BCD, 00 = pregap / pause, 01 = programme area)
//   Byte 3  : Relative MM (BCD) — minutes elapsed since track start
//   Byte 4  : Relative SS (BCD) — seconds
//   Byte 5  : Relative FF (BCD) — frames (0-74)
//   Byte 6  : Zero (reserved in Mode 1)
//   Byte 7  : Absolute MM (BCD) — absolute minutes from disc start
//   Byte 8  : Absolute SS (BCD)
//   Byte 9  : Absolute FF (BCD)
//   Byte 10 : CRC high byte  (CRC-16/CCITT of bytes 0–9, bit-inverted)
//   Byte 11 : CRC low byte
// =============================================================================


#include <stdint.h>
#include <stdbool.h>
#include "cd_types.h"   // msf_t, lba_to_msf
#include "disc_image.h" // track_t

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define QCHANNEL_SIZE   12   // Bytes per Q-channel block
#define SUBCODE_FRAMES  98   // Frames per sector (96 data + 2 sync)

// PIO word packing (3 × 32-bit words) and CRC offset [10..11] depend on these.
CD32_SASSERT(QCHANNEL_SIZE  == 12, "Q-channel is 12 bytes (Red Book 22.3.4)");
CD32_SASSERT(SUBCODE_FRAMES == 98, "98 subcode frames per sector (96 data + 2 sync)");

// Q-channel ADR values
#define Q_ADR_POSITION  0x01   // Current position mode
#define Q_ADR_MCN       0x02   // Media Catalog Number mode
#define Q_ADR_ISRC      0x03   // ISRC mode

// Q-channel CTRL field (upper nibble of byte 0)
//   These match the track descriptor CTRL nibble in the TOC.
#define Q_CTRL_AUDIO        0x00   // 2-channel audio, no pre-emphasis, no copy
#define Q_CTRL_AUDIO_COPY   0x02   // 2-channel audio, copy permitted
#define Q_CTRL_DATA         0x04   // Data track (Mode 1 or Mode 2)
#define Q_CTRL_DATA_COPY    0x06   // Data track, copy permitted

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

// Build a 12-byte Q-channel Position block (ADR=1) for the given sector.
//
//   track_no   — current track number (1-99, binary, not BCD)
//   index      — current index (0=pregap, 1=programme area, binary)
//   is_data    — true for data tracks, false for audio
//   track_lba  — LBA of the start of the current track (for relative time calc)
//   disc_lba   — absolute LBA of the current sector
//   buf        — output buffer, must be at least QCHANNEL_SIZE bytes
void subcode_build_q_position(uint8_t track_no, uint8_t index,
                               bool is_data,
                               uint32_t track_lba, uint32_t disc_lba,
                               uint8_t *buf);

// Build a 12-byte Q-channel MCN (Media Catalog Number) block (ADR=2).
// 'mcn' must point to a 13-byte ASCII string "DDDDDDDDDDDDDD\0".
// If mcn is NULL, all-zero MCN is used (no barcode).
void subcode_build_q_mcn(const char *mcn, uint8_t *buf);

// Build a 12-byte Q-channel ISRC block (ADR=3).
// 'isrc' must point to a 12-byte string (country[2]+owner[3]+year[2]+serial[5]).
// If isrc is NULL, a blank ISRC is encoded.
void subcode_build_q_isrc(uint8_t track_no, bool is_data,
                           const char *isrc, uint8_t *buf);

// Compute the Q-channel CRC-16/CCITT over bytes 0–9.
// The CRC is bit-inverted before storing in bytes 10–11.
// Polynomial: x^16 + x^12 + x^5 + 1  (0x1021), initial value 0x0000.
uint16_t subcode_crc16(const uint8_t *data, uint32_t len);

// Convenience: compute CRC and write it into buf[10..11]
static inline void subcode_append_crc(uint8_t *buf) {
    uint16_t crc = subcode_crc16(buf, 10);
    buf[10] = (crc >> 8) & 0xFF;
    buf[11] =  crc       & 0xFF;
}

// ---------------------------------------------------------------------------
// Subcode push helper (for the PIO subcode encoder)
// ---------------------------------------------------------------------------
// Push all 12 bytes of a Q-channel block into the PIO subcode encoder FIFO.
// 'pio' and 'sm' identify the subcode encoder state machine.
// Returns false if the FIFO was full and data could not be pushed.
#include "hardware/pio.h"
static inline bool subcode_push_to_pio(PIO pio, uint sm, const uint8_t *buf) {
    // Push as three 32-bit words (12 bytes = 96 bits)
    for (int word = 0; word < 3; word++) {
        uint32_t w = ((uint32_t)buf[word*4 + 0] << 24) |
                     ((uint32_t)buf[word*4 + 1] << 16) |
                     ((uint32_t)buf[word*4 + 2] <<  8) |
                     ((uint32_t)buf[word*4 + 3]      );
        if (pio_sm_is_tx_fifo_full(pio, sm)) return false;
        pio_sm_put(pio, sm, w);
    }
    return true;
}

