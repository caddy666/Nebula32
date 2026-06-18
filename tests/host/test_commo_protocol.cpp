// =============================================================================
// test_commo_protocol.cpp — COMMO serial state machine tests
//
// Tests run against src/commo.c compiled with commo_hal_stub.c replacing the
// PIO/GPIO hardware calls.
//
// Relevant production code: src/commo.c, include/commo.h
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "commo.h"
#include "commo_hal_stub.h"
}

// ---------------------------------------------------------------------------
// Module-level context (one per file — groups share it via setup())
// ---------------------------------------------------------------------------

static commo_ctx_t s_ctx;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static uint8_t checksum_of(const uint8_t *buf, int len)
{
    uint8_t s = 0;
    for (int i = 0; i < len; i++) s += buf[i];
    return (uint8_t)~s;
}

// Queue bytes then step through the state machine to deliver a complete packet.
// data_is_low() auto-asserts while the queue is non-empty, so no manual
// set_data_low() calls are needed for normal packet delivery.
static void drive_packet(const uint8_t *bytes, int len)
{
    for (int i = 0; i < len; i++)
        commo_hal_stub_push(bytes[i]);

    commo_tick(&s_ctx);              // IDLE → RXD_OPCODE (queue non-empty → data_is_low)
    for (int i = 0; i < len; i++)
        commo_tick(&s_ctx);          // one tick per byte
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(CommoProtocol)
{
    void setup() override
    {
        commo_init(&s_ctx);
        commo_hal_stub_reset();
    }
};

/* -------------------------------------------------------------------------
 * RX path
 * ---------------------------------------------------------------------- */

TEST(CommoProtocol, SingleByteOpcode_NewCommand)
{
    uint8_t pkt[2];
    pkt[0] = 0x03;                      // STOP_OPC — command_length_table[3]=1
    pkt[1] = checksum_of(pkt, 1);
    drive_packet(pkt, 2);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x03, cmd.bytes[0]);
}

TEST(CommoProtocol, OpcodeWithParam_BufferCorrect)
{
    uint8_t pkt[3];
    pkt[0] = 0x05;                      // command_length_table[5]=2 → 1 param
    pkt[1] = 0x42;
    pkt[2] = checksum_of(pkt, 2);
    drive_packet(pkt, 3);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x05, cmd.bytes[0]);
    BYTES_EQUAL(0x42, cmd.bytes[1]);
}

TEST(CommoProtocol, BadChecksum_CmdError)
{
    uint8_t pkt[2] = { 0x03, 0x00 };    // correct checksum is 0xFC, not 0x00
    drive_packet(pkt, 2);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
}

TEST(CommoProtocol, SameOpcode_SecondIsSameCommand)
{
    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    commo_cmd_t cmd;
    drive_packet(pkt, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(pkt, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_SAME, cmd.status);
}

TEST(CommoProtocol, ZeroOpcode_AfterCountdown_CmdError)
{
    // Zero opcode → ERR_SEND with byte_counter=128; not reported until countdown done
    commo_hal_stub_push(0x00);
    commo_tick(&s_ctx);  // IDLE → RXD_OPCODE (auto data_is_low from queue)
    commo_tick(&s_ctx);  // RXD_OPCODE: b==0 explicit check → ERR_SEND, counter=128

    commo_cmd_t cmd;
    CHECK_FALSE(commo_cmd_pending(&s_ctx, &cmd));

    for (int i = 0; i < 129; i++)          // 128 decrements + 1 that fires the error
        commo_tick(&s_ctx);

    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
}

TEST(CommoProtocol, FreeCmdBuffer_ClearsToNoCommand)
{
    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    commo_cmd_t cmd;
    drive_packet(pkt, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    commo_cmd_consumed(&s_ctx);
    CHECK_FALSE(commo_cmd_pending(&s_ctx, &cmd));
}

TEST(CommoProtocol, MaxParamOpcode_AllParamsInBuffer)
{
    // opcode 0x04 → command_length_table[4]=12 → 11 param bytes + checksum
    uint8_t pkt[13];
    pkt[0] = 0x04;
    for (int i = 1; i <= 11; i++)
        pkt[i] = (uint8_t)(0x10 + i);
    pkt[12] = checksum_of(pkt, 12);

    drive_packet(pkt, 13);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x04, cmd.bytes[0]);
    BYTES_EQUAL(0x1B, cmd.bytes[11]);  // last param: 0x10 + 11 = 0x1B
}

TEST(CommoProtocol, ABA_ThirdIsNewCommand)
{
    uint8_t pktA[2], pktB[3];
    pktA[0] = 0x03; pktA[1] = checksum_of(pktA, 1);
    pktB[0] = 0x05; pktB[1] = 0x42; pktB[2] = checksum_of(pktB, 2);

    commo_cmd_t cmd;
    drive_packet(pktA, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(pktB, 3);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(pktA, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
}

// After a checksum failure the retry of the same opcode must return NEW_COMMAND
// because the failed reception was invalid — last_command is cleared on CMD_ERROR.
TEST(CommoProtocol, SameOpcodeAfterCmdError_IsNewCommand)
{
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);
    uint8_t bad[2]  = { 0x03, 0x00 };  // wrong checksum

    commo_cmd_t cmd;
    drive_packet(good, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(bad, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(good, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
}

/* -------------------------------------------------------------------------
 * TX path
 * ---------------------------------------------------------------------- */

TEST(CommoProtocol, SendString_DataByteTransmitted)
{
    uint8_t tx = 0xA5;
    CHECK(commo_send(&s_ctx, &tx, 1, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // IDLE → TXD_DATA
    commo_tick(&s_ctx);   // TXD_DATA: transmit 0xA5 → TXD_CHECKSUM (release #1)
    commo_tick(&s_ctx);   // TXD_CHECKSUM: transmit ~0xA5 → IDLE (release #2)
    LONGS_EQUAL(0xA5, commo_hal_stub_tx_byte(0));
    LONGS_EQUAL(2, commo_hal_stub_release_count());
}

TEST(CommoProtocol, SendString_ChecksumByteTransmitted)
{
    uint8_t tx = 0xA5;
    CHECK(commo_send(&s_ctx, &tx, 1, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);
    commo_tick(&s_ctx);
    commo_tick(&s_ctx);
    LONGS_EQUAL((uint8_t)~0xA5u, commo_hal_stub_tx_byte(1));  // 0x5A
}

TEST(CommoProtocol, SendString_BusyDuringTxdData)
{
    uint8_t tx = 0xA5;
    CHECK(commo_send(&s_ctx, &tx, 1, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // IDLE → TXD_DATA
    CHECK_FALSE(commo_send_ready(&s_ctx));
}

TEST(CommoProtocol, SendString_ReadyAfterComplete)
{
    uint8_t tx = 0xA5;
    CHECK(commo_send(&s_ctx, &tx, 1, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);
    commo_tick(&s_ctx);
    commo_tick(&s_ctx);
    CHECK(commo_send_ready(&s_ctx));
    LONGS_EQUAL(2, commo_hal_stub_release_count());
}

/* -------------------------------------------------------------------------
 * Fuzz / adversarial path
 * ---------------------------------------------------------------------- */

TEST_GROUP(CommoFuzz)
{
    void setup() override
    {
        commo_init(&s_ctx);
        commo_hal_stub_reset();
    }
};

// After sending opcode 0x05 + its one param byte, the state machine is in
// RXD_CHECKSUM waiting for ~(sum).  If the sender "aborts" and the next byte
// is a new opcode (0x03), the SM reads it as the checksum: ~0x03=0xFC but
// accumulated checksum=0x47 → mismatch → CMD_ERROR.  The SM must then recover
// and accept a subsequent valid command.
TEST(CommoFuzz, AbortedCommand_ByteConsumedAsChecksum)
{
    uint8_t partial[2] = { 0x05, 0x42 };
    drive_packet(partial, 2);              // opcode + param; SM now in RXD_CHECKSUM

    commo_cmd_t cmd;
    CHECK_FALSE(commo_cmd_pending(&s_ctx, &cmd));

    // Abort: feed what would be the next opcode (0x03) as the checksum byte.
    // ~0x03 = 0xFC ≠ 0x47 (= 0x05 + 0x42) → CMD_ERROR
    commo_hal_stub_push(0x03);
    commo_tick(&s_ctx);  // data_is_low auto-asserts from queue

    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
    commo_cmd_consumed(&s_ctx);

    // SM must recover: a subsequent valid command returns NEW_COMMAND
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);
    drive_packet(good, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
}

// 50 rapid commo_tick calls with no pending TX and data_is_low=0 must
// leave the state machine in IDLE with no spurious command reported.
TEST(CommoFuzz, RapidStatusPoll_50Ticks_NoStateCorruption)
{
    commo_hal_stub_set_data_low(0);
    for (int i = 0; i < 50; i++)
        commo_tick(&s_ctx);

    CHECK(commo_send_ready(&s_ctx));
    commo_cmd_t cmd;
    CHECK_FALSE(commo_cmd_pending(&s_ctx, &cmd));
}

// A spurious data strobe (data_is_low=1) while TX is in progress blocks the
// TX byte from being sent but does not corrupt the TX buffer or advance the
// byte pointer.  After the strobe clears, TX completes in order.
TEST(CommoFuzz, TxNotInterruptedByRxStrobe)
{
    uint8_t tx[2] = { 0xA5, 0x5A };
    CHECK(commo_send(&s_ctx, tx, 2, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // IDLE → TXD_DATA (tx_req consumed)

    commo_hal_stub_set_data_low(1);
    commo_tick(&s_ctx);   // TXD_DATA: data_is_low set → blocked, nothing transmitted
    LONGS_EQUAL(0, commo_hal_stub_tx_count());

    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // transmit 0xA5
    commo_tick(&s_ctx);   // transmit 0x5A → TXD_CHECKSUM (release #1)
    commo_tick(&s_ctx);   // transmit checksum → IDLE (release #2)
    LONGS_EQUAL(3, commo_hal_stub_tx_count());
    BYTES_EQUAL(0xA5, commo_hal_stub_tx_byte(0));
    BYTES_EQUAL(0x5A, commo_hal_stub_tx_byte(1));
    LONGS_EQUAL(2, commo_hal_stub_release_count());
}

// data_is_low=1 in the TXD_CHECKSUM state must block the checksum byte just
// as it does in TXD_DATA — the checksum is sent only after the line goes low.
TEST(CommoFuzz, TxChecksumState_BlockedByDataLow)
{
    uint8_t tx = 0xA5;
    CHECK(commo_send(&s_ctx, &tx, 1, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // IDLE → TXD_DATA
    commo_tick(&s_ctx);   // TXD_DATA: transmit 0xA5 → TXD_CHECKSUM

    commo_hal_stub_set_data_low(1);
    commo_tick(&s_ctx);   // TXD_CHECKSUM: data_is_low → blocked, count still 1
    LONGS_EQUAL(1, commo_hal_stub_tx_count());

    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);   // TXD_CHECKSUM: transmit ~0xA5 → IDLE
    LONGS_EQUAL(2, commo_hal_stub_tx_count());
    BYTES_EQUAL((uint8_t)~0xA5u, commo_hal_stub_tx_byte(1));
}

// Two successive checksum failures keep last_command=0; a subsequent valid
// command returns NEW_COMMAND both times (not SAME_COMMAND after the second).
TEST(CommoFuzz, SuccessiveErrors_RetryIsNewCommand)
{
    uint8_t bad[2]  = { 0x03, 0x00 };   // wrong checksum (correct = 0xFC)
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);

    commo_cmd_t cmd;
    drive_packet(bad, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
    commo_cmd_consumed(&s_ctx);

    drive_packet(bad, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
    commo_cmd_consumed(&s_ctx);

    // After two errors, last_command must still be 0 — retry is NEW, not SAME
    drive_packet(good, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
}

/* =========================================================================
 * CommoPowerOn — wire-level byte sequences from the real power-on handshake
 *
 * Real logic-analyzer captures (pon-poff-idle.csv / zool2.csv) show:
 *   HOST → DRIVE:  15 00 EA          (SPINDLE_MOTOR_OFF_OPC, param=0x00)
 *   HOST → DRIVE:  12 ED             (FOCUS_ON_OPC, no param)
 *   DRIVE → HOST:  27 D8 …           (Chinon frame-header + focus-error)
 *
 * Tests 1-3: HOST→DRIVE byte sequences are accepted by the COMMO SM.
 * Tests 4-5: A 15-byte STATUS packet (COMMO_SEND_COMPLETE) produces 16 wire
 *            bytes with a valid COMMO additive checksum.
 * Test 6:    0x27+0xD8=0xFF shows why the SM cannot distinguish a Chinon
 *            frame-header/status pair from a valid Pico ODE one-byte exchange.
 * ======================================================================= */

TEST_GROUP(CommoPowerOn)
{
    void setup() override
    {
        commo_init(&s_ctx);
        commo_hal_stub_reset();
    }
};

// SPINDLE_MOTOR_OFF_OPC=0x15: lower nibble 5, command_length_table[5]=2 →
// opcode + 1 param byte on the wire.  Checksum = ~(0x15+0x00)&0xFF = 0xEA.
TEST(CommoPowerOn, SpindleMotorOff_ParsesAsNewCommand)
{
    uint8_t pkt[3] = { 0x15, 0x00, 0xEA };
    drive_packet(pkt, 3);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x15, cmd.bytes[0]);
    BYTES_EQUAL(0x00, cmd.bytes[1]);
}

// Verify the checksum byte for SPINDLE_MOTOR_OFF(param=0) at compile time.
TEST(CommoPowerOn, SpindleMotorOff_ChecksumIs0xEA)
{
    BYTES_EQUAL(0xEA, (uint8_t)(~(0x15u + 0x00u)));
}

// FOCUS_ON_OPC=0x12: lower nibble 2, command_length_table[2]=1 → opcode only.
// Checksum = ~0x12 & 0xFF = 0xED.
TEST(CommoPowerOn, FocusOn_ParsesAsNewCommand)
{
    uint8_t pkt[2] = { 0x12, 0xED };
    drive_packet(pkt, 2);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x12, cmd.bytes[0]);
}

// A STATUS packet (15 data bytes + COMMO_SEND_COMPLETE) must produce exactly
// 16 wire bytes: 15 data bytes then 1 COMMO additive checksum.
TEST(CommoPowerOn, StatusPacket_Produces16WireBytes)
{
    uint8_t pkt[15];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00;   // status packet type
    pkt[2] = 0x04;   // DRIVE_STATUS_DISC — disc present

    CHECK(commo_send(&s_ctx, pkt, 15, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);
    for (int i = 0; i < 15; i++) commo_tick(&s_ctx);
    commo_tick(&s_ctx);
    LONGS_EQUAL(16, commo_hal_stub_tx_count());
}

// The 16th wire byte must satisfy the COMMO additive checksum invariant:
// (sum of all 16 transmitted bytes) & 0xFF == 0xFF.
TEST(CommoPowerOn, StatusPacket_AdditiveChecksumValid)
{
    uint8_t pkt[15];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00;
    pkt[2] = 0x04;   // DRIVE_STATUS_DISC

    CHECK(commo_send(&s_ctx, pkt, 15, COMMO_SEND_COMPLETE));
    commo_hal_stub_set_data_low(0);
    commo_tick(&s_ctx);
    for (int i = 0; i < 15; i++) commo_tick(&s_ctx);
    commo_tick(&s_ctx);

    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) sum += commo_hal_stub_tx_byte(i);
    BYTES_EQUAL(0xFF, sum);
}

// 0x27 (Chinon frame header) + 0xD8 (focus-error status) = 0xFF in 8-bit
// arithmetic — a valid COMMO additive checksum pair.  This is why the decoder
// cannot classify Chinon drive responses as errors: they look like a
// one-data-byte Pico ODE packet with a correct checksum.
TEST(CommoPowerOn, ChinonFrameHeader_PlusFocusError_IsValidChecksumPair)
{
    BYTES_EQUAL(0xFF, (uint8_t)(0x27u + 0xD8u));
}

// A data-phase noise spike corrupts the param byte of a 2-byte command.
// opcode=0x15 (SPINDLE_MOTOR_OFF) arrives cleanly; param=0xAA arrives (glitched;
// should be 0x00); stale checksum 0xEA (~(0x15+0x00)) then arrives.
// Accumulated checksum = 0x15+0xAA = 0xBF; ~0xEA = 0x15 ≠ 0xBF → CMD_ERROR.
// last_command must be cleared; clean retry must return NEW_COMMAND.
TEST(CommoFuzz, GlitchedParamByte_CmdError_ThenRecovery)
{
    uint8_t glitched[3] = { 0x15, 0xAA, 0xEA };
    drive_packet(glitched, 3);
    commo_cmd_t cmd;
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_ERROR, cmd.status);
    commo_cmd_consumed(&s_ctx);

    uint8_t good[3] = { 0x15, 0x00, 0xEA };
    drive_packet(good, 3);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x15, cmd.bytes[0]);
    BYTES_EQUAL(0x00, cmd.bytes[1]);
}

// Scenario 1: Amiga game engine bug — rapid-fire PAUSE then PLAY without the
// host calling commo_cmd_consumed between them.  The state machine has no
// buffer-free guard in IDLE, so the second command silently overwrites the
// first.  This verifies that the SM accepts the new command and that PAUSE
// (0x06) is gone from the buffer — only PLAY (0x09) is visible afterward.
TEST(CommoFuzz, RapidFire_SecondCommandOverwritesFirst)
{
    uint8_t pause_pkt[2] = { 0x06, 0 };   // PAUSE_OPC, len=1
    pause_pkt[1] = checksum_of(pause_pkt, 1);
    uint8_t play_pkt[2]  = { 0x09, 0 };   // PLAY_OPC, len=1
    play_pkt[1]  = checksum_of(play_pkt, 1);

    // First command arrives; host app does NOT call commo_cmd_consumed (game engine bug)
    commo_cmd_t cmd;
    drive_packet(pause_pkt, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);

    // Second command hammered in immediately — report_cmd overwritten
    drive_packet(play_pkt, 2);
    CHECK(commo_cmd_pending(&s_ctx, &cmd));
    LONGS_EQUAL(COMMO_CMD_NEW, cmd.status);
    BYTES_EQUAL(0x09, cmd.bytes[0]);   // PLAY visible, PAUSE (0x06) silently lost
}
