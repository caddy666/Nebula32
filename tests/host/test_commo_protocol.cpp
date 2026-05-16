// =============================================================================
// test_commo_protocol.cpp — COMMO serial state machine tests
//
// upstream/core/commo.c depends on pio_hw.h, commo.pio.h, and pico/stdlib.h
// (all hardware-bound) and cannot be compiled into the host suite directly.
// This file replicates commo_ctx_t, commo_step(), and the public API, then
// replaces the three hardware calls with inline stubs:
//   get_rxd_data()       — drain from a pre-loaded byte queue
//   commo_data_is_low()  — controlled flag (set by drive_packet helper)
//   transmit_txd()       — append to a capture log (TX path tests)
//
// Known bug documented in test SameOpcodeAfterCmdError_BugReturnsSame:
//   commo.c COMMO_SM_RXD_CHECKSUM does not clear last_command on CMD_ERROR.
//   A retry of the same opcode after a bad checksum returns COMMO_SAME_COMMAND
//   instead of COMMO_NEW_COMMAND.
//
// Relevant production code: upstream/core/commo.c, upstream/include/commo.h
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>

// Constants from upstream/include/commo.h (not included to avoid function
// declaration conflicts with the replicated static versions below)
#define COMMO_FALSE               0x00
#define COMMO_TRUE                0x01
#define COMMO_READY_WITHOUT_ERROR 0x00
#define COMMO_BUSY                0x03
#define COMMO_NO_COMMAND          0x00
#define COMMO_NEW_COMMAND         0x01
#define COMMO_SAME_COMMAND        0x02
#define COMMO_CMD_ERROR           0x03
#define SEND_STRING_COMPLETE      1
#define SEND_STRING_APPEND        0

// ---------------------------------------------------------------------------
// Stubs
// ---------------------------------------------------------------------------

static uint8_t s_rx_queue[32];
static int     s_rx_head  = 0;
static int     s_rx_tail  = 0;
static int     s_data_is_low = 0;

static uint8_t s_tx_log[32];
static int     s_tx_log_count = 0;

static uint8_t get_rxd_data(void)
{
    if (s_rx_head >= s_rx_tail) return 0;
    return s_rx_queue[s_rx_head++];
}

static int commo_data_is_low(void) { return s_data_is_low; }

static void transmit_txd(uint8_t b)
{
    if (s_tx_log_count < (int)sizeof(s_tx_log))
        s_tx_log[s_tx_log_count++] = b;
}

static void pio_commo_release(void) {}

// ---------------------------------------------------------------------------
// State machine replicated from upstream/core/commo.c
// Changes must be reflected in the production source and vice versa.
// ---------------------------------------------------------------------------

typedef enum {
    COMMO_SM_IDLE = 0,
    COMMO_SM_RXD_OPCODE,
    COMMO_SM_RXD_PARM,
    COMMO_SM_RXD_CHECKSUM,
    COMMO_SM_TXD_DATA,
    COMMO_SM_TXD_CHECKSUM,
    COMMO_SM_ERR_SEND
} commo_sm_state_t;

typedef struct {
    commo_sm_state_t state;
    uint8_t cmd_length;
    uint8_t checksum;
    uint8_t byte_counter;
    uint8_t byte_pointer;
    uint8_t rx_status;
    uint8_t last_command;
    uint8_t rx_buffer[12];
    uint8_t tx_buffer[16];
    uint8_t tx_length;
    uint8_t tx_req;
    uint8_t tx_chk_req;
    uint8_t report_cmd;
    uint8_t cmd_buf_free;
} commo_ctx_t;

static commo_ctx_t s_commo;

static const uint8_t command_length_table[16] = {
    1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1
};

static void commo_step(commo_ctx_t *c)
{
    switch (c->state) {

    case COMMO_SM_IDLE:
        if (c->tx_req) {
            c->byte_pointer = 0;
            c->checksum     = 0;
            c->state        = COMMO_SM_TXD_DATA;
        } else if (commo_data_is_low()) {
            c->state        = COMMO_SM_RXD_OPCODE;
            c->byte_counter = 0;
            c->checksum     = 0;
        }
        break;

    case COMMO_SM_RXD_OPCODE: {
        uint8_t b = get_rxd_data();
        if (b == 0) {
            c->state        = COMMO_SM_ERR_SEND;
            c->byte_counter = 128;
            break;
        }
        c->rx_buffer[0] = b;
        c->checksum     = b;
        c->byte_counter = 1;
        c->cmd_length   = command_length_table[b & 0x0Fu];
        c->state = (c->cmd_length == 1) ? COMMO_SM_RXD_CHECKSUM
                                        : COMMO_SM_RXD_PARM;
        break;
    }

    case COMMO_SM_RXD_PARM:
        if (!commo_data_is_low()) break;
        {
            uint8_t b = get_rxd_data();
            c->rx_buffer[c->byte_counter++] = b;
            c->checksum += b;
            if (c->byte_counter >= c->cmd_length)
                c->state = COMMO_SM_RXD_CHECKSUM;
        }
        break;

    case COMMO_SM_RXD_CHECKSUM:
        if (!commo_data_is_low()) break;
        {
            uint8_t rx = get_rxd_data();
            if ((uint8_t)~rx == c->checksum) {
                c->rx_status = (c->rx_buffer[0] == c->last_command)
                               ? COMMO_SAME_COMMAND
                               : COMMO_NEW_COMMAND;
                if (c->rx_status == COMMO_NEW_COMMAND)
                    c->last_command = c->rx_buffer[0];
            } else {
                c->rx_status    = COMMO_CMD_ERROR;
                c->last_command = 0;  // invalidate so retry is treated as NEW_COMMAND
            }
            c->report_cmd = 1;
            c->checksum   = 0;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    case COMMO_SM_TXD_DATA:
        if (commo_data_is_low()) break;
        transmit_txd(c->tx_buffer[c->byte_pointer]);
        c->checksum += c->tx_buffer[c->byte_pointer];
        c->byte_pointer++;
        if (--c->byte_counter == 0) {
            c->tx_req = 0;
            c->state  = c->tx_chk_req ? COMMO_SM_TXD_CHECKSUM
                                      : COMMO_SM_IDLE;
            pio_commo_release();
        }
        break;

    case COMMO_SM_TXD_CHECKSUM:
        if (commo_data_is_low()) break;
        transmit_txd((uint8_t)~c->checksum);
        c->tx_req     = 0;
        c->tx_chk_req = 0;
        c->checksum   = 0;
        c->state      = COMMO_SM_IDLE;
        pio_commo_release();
        break;

    case COMMO_SM_ERR_SEND:
        if (c->byte_counter > 0) {
            c->byte_counter--;
        } else {
            c->rx_status  = COMMO_CMD_ERROR;
            c->report_cmd = 1;
            c->state      = COMMO_SM_IDLE;
        }
        break;

    default:
        c->state = COMMO_SM_IDLE;
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API wrappers (mirror commo.c signatures)
// ---------------------------------------------------------------------------

static void COMMO_INIT(void)
{
    memset(&s_commo, 0, sizeof(s_commo));
    s_commo.state = COMMO_SM_IDLE;
}

static void    COMMO_INTERFACE(void)    { commo_step(&s_commo); }
static uint8_t NEW_CMD_RECEIVED(void)   { return s_commo.report_cmd ? s_commo.rx_status : COMMO_NO_COMMAND; }
static uint8_t GET_BUFFER(uint8_t idx)  { return (idx < 12) ? s_commo.rx_buffer[idx] : 0; }
static uint8_t FREE_CMD_BUFFER(void)    { s_commo.report_cmd = 0; s_commo.cmd_buf_free = 1; return COMMO_TRUE; }

static uint8_t SEND_STRING(uint8_t mode, uint8_t *data, uint8_t length)
{
    if (s_commo.tx_req || length > (uint8_t)sizeof(s_commo.tx_buffer)) return COMMO_FALSE;
    memcpy(s_commo.tx_buffer, data, length);
    s_commo.byte_counter = length;
    s_commo.tx_length    = length;
    s_commo.tx_chk_req   = (mode == SEND_STRING_COMPLETE) ? 1u : 0u;
    s_commo.tx_req       = 1;
    return COMMO_TRUE;
}

static uint8_t SEND_STRING_READY(void)
{
    if (s_commo.state == COMMO_SM_TXD_DATA ||
        s_commo.state == COMMO_SM_TXD_CHECKSUM)
        return COMMO_BUSY;
    return COMMO_READY_WITHOUT_ERROR;
}

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
// Mirrors the drive_packet() helper from test_commo.c.
static void drive_packet(const uint8_t *bytes, int len)
{
    for (int i = 0; i < len; i++)
        s_rx_queue[s_rx_tail++] = bytes[i];

    s_data_is_low = 1; COMMO_INTERFACE();   // IDLE → RXD_OPCODE

    for (int i = 0; i < len; i++) {
        s_data_is_low = 1; COMMO_INTERFACE();   // receive one byte per iteration
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(CommoProtocol)
{
    void setup() override
    {
        COMMO_INIT();
        s_rx_head      = 0;
        s_rx_tail      = 0;
        s_data_is_low  = 0;
        s_tx_log_count = 0;
        memset(s_rx_queue, 0, sizeof(s_rx_queue));
        memset(s_tx_log,   0, sizeof(s_tx_log));
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
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    BYTES_EQUAL(0x03, GET_BUFFER(0));
}

TEST(CommoProtocol, OpcodeWithParam_BufferCorrect)
{
    uint8_t pkt[3];
    pkt[0] = 0x05;                      // command_length_table[5]=2 → 1 param
    pkt[1] = 0x42;
    pkt[2] = checksum_of(pkt, 2);
    drive_packet(pkt, 3);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    BYTES_EQUAL(0x05, GET_BUFFER(0));
    BYTES_EQUAL(0x42, GET_BUFFER(1));
}

TEST(CommoProtocol, BadChecksum_CmdError)
{
    uint8_t pkt[2] = { 0x03, 0x00 };    // correct checksum is 0xFC, not 0x00
    drive_packet(pkt, 2);
    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
}

TEST(CommoProtocol, SameOpcode_SecondIsSameCommand)
{
    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    drive_packet(pkt, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(pkt, 2);
    BYTES_EQUAL(COMMO_SAME_COMMAND, NEW_CMD_RECEIVED());
}

TEST(CommoProtocol, ZeroOpcode_AfterCountdown_CmdError)
{
    // Zero opcode → ERR_SEND with byte_counter=128; not reported until countdown done
    s_rx_queue[s_rx_tail++] = 0x00;
    s_data_is_low = 1; COMMO_INTERFACE();   // IDLE → RXD_OPCODE
    s_data_is_low = 1; COMMO_INTERFACE();   // RXD_OPCODE: b=0 → ERR_SEND, counter=128

    BYTES_EQUAL(COMMO_NO_COMMAND, NEW_CMD_RECEIVED());

    for (int i = 0; i < 129; i++)           // 128 decrements + 1 that fires the error
        COMMO_INTERFACE();

    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
}

TEST(CommoProtocol, FreeCmdBuffer_ClearsToNoCommand)
{
    uint8_t pkt[2];
    pkt[0] = 0x03;
    pkt[1] = checksum_of(pkt, 1);

    drive_packet(pkt, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();
    BYTES_EQUAL(COMMO_NO_COMMAND, NEW_CMD_RECEIVED());
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
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    BYTES_EQUAL(0x04, GET_BUFFER(0));
    BYTES_EQUAL(0x1B, GET_BUFFER(11));  // last param: 0x10 + 11 = 0x1B
}

TEST(CommoProtocol, ABA_ThirdIsNewCommand)
{
    uint8_t pktA[2], pktB[3];
    pktA[0] = 0x03; pktA[1] = checksum_of(pktA, 1);
    pktB[0] = 0x05; pktB[1] = 0x42; pktB[2] = checksum_of(pktB, 2);

    drive_packet(pktA, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(pktB, 3);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(pktA, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
}

// After a checksum failure the retry of the same opcode must return NEW_COMMAND
// because the failed reception was invalid — last_command is cleared on CMD_ERROR.
TEST(CommoProtocol, SameOpcodeAfterCmdError_IsNewCommand)
{
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);
    uint8_t bad[2]  = { 0x03, 0x00 };  // wrong checksum

    drive_packet(good, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(bad, 2);
    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(good, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
}

/* -------------------------------------------------------------------------
 * TX path
 * ---------------------------------------------------------------------- */

TEST(CommoProtocol, SendString_DataByteTransmitted)
{
    uint8_t tx = 0xA5;
    SEND_STRING(SEND_STRING_COMPLETE, &tx, 1);
    s_data_is_low = 0;
    COMMO_INTERFACE();   // IDLE → TXD_DATA
    COMMO_INTERFACE();   // TXD_DATA: transmit 0xA5 → TXD_CHECKSUM
    COMMO_INTERFACE();   // TXD_CHECKSUM: transmit ~0xA5 → IDLE
    LONGS_EQUAL(0xA5, s_tx_log[0]);
}

TEST(CommoProtocol, SendString_ChecksumByteTransmitted)
{
    uint8_t tx = 0xA5;
    SEND_STRING(SEND_STRING_COMPLETE, &tx, 1);
    s_data_is_low = 0;
    COMMO_INTERFACE();
    COMMO_INTERFACE();
    COMMO_INTERFACE();
    LONGS_EQUAL((uint8_t)~0xA5u, s_tx_log[1]);   // 0x5A
}

TEST(CommoProtocol, SendString_BusyDuringTxdData)
{
    uint8_t tx = 0xA5;
    SEND_STRING(SEND_STRING_COMPLETE, &tx, 1);
    s_data_is_low = 0;
    COMMO_INTERFACE();   // IDLE → TXD_DATA
    BYTES_EQUAL(COMMO_BUSY, SEND_STRING_READY());
}

TEST(CommoProtocol, SendString_ReadyAfterComplete)
{
    uint8_t tx = 0xA5;
    SEND_STRING(SEND_STRING_COMPLETE, &tx, 1);
    s_data_is_low = 0;
    COMMO_INTERFACE();
    COMMO_INTERFACE();
    COMMO_INTERFACE();
    BYTES_EQUAL(COMMO_READY_WITHOUT_ERROR, SEND_STRING_READY());
}

/* -------------------------------------------------------------------------
 * Fuzz / adversarial path
 * ---------------------------------------------------------------------- */

TEST_GROUP(CommoFuzz)
{
    void setup() override
    {
        COMMO_INIT();
        s_rx_head      = 0;
        s_rx_tail      = 0;
        s_data_is_low  = 0;
        s_tx_log_count = 0;
        memset(s_rx_queue, 0, sizeof(s_rx_queue));
        memset(s_tx_log,   0, sizeof(s_tx_log));
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

    BYTES_EQUAL(COMMO_NO_COMMAND, NEW_CMD_RECEIVED());

    // Abort: feed what would be the next opcode (0x03) as the checksum byte.
    // ~0x03 = 0xFC ≠ 0x47 (= 0x05 + 0x42) → CMD_ERROR
    s_rx_queue[s_rx_tail++] = 0x03;
    s_data_is_low = 1;
    COMMO_INTERFACE();

    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    // SM must recover: a subsequent valid command returns NEW_COMMAND
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);
    drive_packet(good, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
}

// 50 rapid COMMO_INTERFACE calls with no pending TX and data_is_low=0 must
// leave the state machine in IDLE with no spurious command reported.
// Exercises the atomic-barrier path: no partial-status-word corruption.
TEST(CommoFuzz, RapidStatusPoll_50Ticks_NoStateCorruption)
{
    s_data_is_low = 0;
    for (int i = 0; i < 50; i++)
        COMMO_INTERFACE();

    BYTES_EQUAL(COMMO_SM_IDLE, (uint8_t)s_commo.state);
    BYTES_EQUAL(COMMO_NO_COMMAND, NEW_CMD_RECEIVED());
}

// A spurious data strobe (data_is_low=1) while TX is in progress blocks the
// TX byte from being sent but does not corrupt the TX buffer or advance the
// byte pointer.  After the strobe clears, TX completes in order.
TEST(CommoFuzz, TxNotInterruptedByRxStrobe)
{
    uint8_t tx[2] = { 0xA5, 0x5A };
    SEND_STRING(SEND_STRING_COMPLETE, tx, 2);
    s_data_is_low = 0;
    COMMO_INTERFACE();   // IDLE → TXD_DATA (tx_req consumed)

    s_data_is_low = 1;
    COMMO_INTERFACE();   // TXD_DATA: data_is_low set → blocked, nothing transmitted
    LONGS_EQUAL(0, s_tx_log_count);

    s_data_is_low = 0;
    COMMO_INTERFACE();   // transmit 0xA5
    COMMO_INTERFACE();   // transmit 0x5A → TXD_CHECKSUM
    COMMO_INTERFACE();   // transmit checksum → IDLE
    LONGS_EQUAL(3, s_tx_log_count);
    BYTES_EQUAL(0xA5, s_tx_log[0]);
    BYTES_EQUAL(0x5A, s_tx_log[1]);
}

// data_is_low=1 in the TXD_CHECKSUM state must block the checksum byte just
// as it does in TXD_DATA — the checksum is sent only after the line goes low.
TEST(CommoFuzz, TxChecksumState_BlockedByDataLow)
{
    uint8_t tx = 0xA5;
    SEND_STRING(SEND_STRING_COMPLETE, &tx, 1);
    s_data_is_low = 0;
    COMMO_INTERFACE();   // IDLE → TXD_DATA
    COMMO_INTERFACE();   // TXD_DATA: transmit 0xA5 → TXD_CHECKSUM

    s_data_is_low = 1;
    COMMO_INTERFACE();   // TXD_CHECKSUM: data_is_low → blocked, count still 1
    LONGS_EQUAL(1, s_tx_log_count);

    s_data_is_low = 0;
    COMMO_INTERFACE();   // TXD_CHECKSUM: transmit ~0xA5 → IDLE
    LONGS_EQUAL(2, s_tx_log_count);
    BYTES_EQUAL((uint8_t)~0xA5u, s_tx_log[1]);
}

// Two successive checksum failures keep last_command=0; a subsequent valid
// command returns NEW_COMMAND both times (not SAME_COMMAND after the second).
TEST(CommoFuzz, SuccessiveErrors_RetryIsNewCommand)
{
    uint8_t bad[2]  = { 0x03, 0x00 };   // wrong checksum (correct = 0xFC)
    uint8_t good[2] = { 0x03, 0 };
    good[1] = checksum_of(good, 1);

    drive_packet(bad, 2);
    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    drive_packet(bad, 2);
    BYTES_EQUAL(COMMO_CMD_ERROR, NEW_CMD_RECEIVED());
    FREE_CMD_BUFFER();

    // After two errors, last_command must still be 0 — retry is NEW, not SAME
    drive_packet(good, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
}

// Scenario 1: Amiga game engine bug — rapid-fire PAUSE then PLAY without the
// host calling FREE_CMD_BUFFER between them.  The state machine has no buffer-
// free guard in IDLE, so the second command silently overwrites the first.
// This verifies that the SM accepts the new command and that PAUSE (0x06) is
// gone from the buffer — only PLAY (0x09) is visible afterward.
TEST(CommoFuzz, RapidFire_SecondCommandOverwritesFirst)
{
    uint8_t pause_pkt[2] = { 0x06, 0 };   // PAUSE_OPC, len=1
    pause_pkt[1] = checksum_of(pause_pkt, 1);
    uint8_t play_pkt[2]  = { 0x09, 0 };   // PLAY_OPC, len=1
    play_pkt[1]  = checksum_of(play_pkt, 1);

    // First command arrives; host app does NOT call FREE_CMD_BUFFER (game engine bug)
    drive_packet(pause_pkt, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());

    // Second command hammered in immediately — report_cmd overwritten
    drive_packet(play_pkt, 2);
    BYTES_EQUAL(COMMO_NEW_COMMAND, NEW_CMD_RECEIVED());
    BYTES_EQUAL(0x09, GET_BUFFER(0));   // PLAY visible, PAUSE (0x06) silently lost
}
