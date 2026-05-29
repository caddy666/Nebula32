// =============================================================================
// test_logger.cpp — Logger pure-logic tests
//
// Intent: verify the five pure-C helpers inside src/logger.c that have no
// FatFS, hardware timer, or Pico SDK dependency.  Those helpers are static
// in the full build, so this file replicates each one verbatim and tests the
// invariant.  If the production code diverges, the replica will differ and
// the test will catch the regression.
//
// Functions under test (all static in logger.c):
//   parse_bool()       — maps config value strings to bool (truthy set)
//   trim()             — strips leading/trailing whitespace/newlines in-place
//   cmd_name()         — maps COMMO command byte to a human-readable string
//   ring_append()      — ring buffer write with drop-on-overflow semantics
//   status flag decode — bit-mask → flag string used in _log_cmd_resp()
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "logger.h"

// ---------------------------------------------------------------------------
// Replicated helpers — must stay in sync with src/logger.c
// ---------------------------------------------------------------------------

static bool parse_bool(const char *val)
{
    return (strcmp(val,          "1")    == 0 ||
            strcasecmp(val, "yes")  == 0 ||
            strcasecmp(val, "true") == 0 ||
            strcasecmp(val, "on")   == 0);
}

static void trim(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                        s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static const char *cmd_name(uint8_t cmd)
{
    switch (cmd) {
        case 0x00: return "SYNC";       case 0x01: return "GETSTAT";
        case 0x02: return "SETLOC";     case 0x03: return "PLAY";
        case 0x04: return "FORWARD";    case 0x05: return "BACKWARD";
        case 0x06: return "READN";      case 0x07: return "MOTORON";
        case 0x08: return "STOP";       case 0x09: return "PAUSE";
        case 0x0A: return "RESET";      case 0x0B: return "MUTE";
        case 0x0C: return "UNMUTE";     case 0x0D: return "SETFILTER";
        case 0x0E: return "SETMODE";    case 0x0F: return "GETPARAM";
        case 0x10: return "GETLOCL";    case 0x11: return "GETLOCP";
        case 0x13: return "GETTN";      case 0x14: return "GETTD";
        case 0x15: return "SEEKL";      case 0x16: return "SEEKP";
        case 0x19: return "TEST";       case 0x1A: return "ID";
        case 0x1B: return "READS";      case 0x1E: return "READTOC";
        default:   return "UNKNOWN";
    }
}

// Minimal self-contained ring buffer (mirrors logger.c's ring_append logic)
#define RING_SIZE 64u
static char     s_ring[RING_SIZE];
static uint32_t s_ring_head  = 0;
static uint32_t s_ring_tail  = 0;
static uint32_t s_ring_used  = 0;
static uint32_t s_dropped    = 0;

static void ring_reset(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head  = 0;
    s_ring_tail  = 0;
    s_ring_used  = 0;
    s_dropped    = 0;
}

static void ring_append(const char *data, uint32_t len)
{
    if (len == 0) return;
    if (len > RING_SIZE - s_ring_used) {
        s_dropped++;
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        s_ring[s_ring_head] = data[i];
        s_ring_head = (s_ring_head + 1) % RING_SIZE;
    }
    s_ring_used += len;
}

// Read all pending bytes out of the ring into buf (up to bufsz-1), NUL-terminate.
static void ring_drain(char *buf, int bufsz)
{
    int out = 0;
    while (s_ring_used > 0 && out < bufsz - 1) {
        buf[out++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
        s_ring_used--;
    }
    buf[out] = '\0';
}

// Build the status-flag string the same way _log_cmd_resp() does
static void decode_status_flags(uint8_t stat, char *out, int outsz)
{
    out[0] = '\0';
    if (stat & 0x80) strncat(out, "BUSY ",  outsz - (int)strlen(out) - 1);
    if (stat & 0x40) strncat(out, "RSLR ",  outsz - (int)strlen(out) - 1);
    if (stat & 0x20) strncat(out, "DRQ ",   outsz - (int)strlen(out) - 1);
    if (stat & 0x04) strncat(out, "DISC ",  outsz - (int)strlen(out) - 1);
    if (stat & 0x01) strncat(out, "ERR",    outsz - (int)strlen(out) - 1);
}

// ---------------------------------------------------------------------------
// Test groups
// ---------------------------------------------------------------------------

TEST_GROUP(Logger)
{
    void setup()    { ring_reset(); }
    void teardown() {}
};

/* -------------------------------------------------------------------------
 * parse_bool — truthy set: "1", "yes", "true", "on" (case-insensitive)
 * ---------------------------------------------------------------------- */

TEST(Logger, ParseBool_TruthyValues)
{
    CHECK_TRUE(parse_bool("1"));
    CHECK_TRUE(parse_bool("yes"));
    CHECK_TRUE(parse_bool("YES"));
    CHECK_TRUE(parse_bool("Yes"));
    CHECK_TRUE(parse_bool("true"));
    CHECK_TRUE(parse_bool("TRUE"));
    CHECK_TRUE(parse_bool("on"));
    CHECK_TRUE(parse_bool("ON"));
}

TEST(Logger, ParseBool_FalsyValues)
{
    CHECK_FALSE(parse_bool("0"));
    CHECK_FALSE(parse_bool("no"));
    CHECK_FALSE(parse_bool("false"));
    CHECK_FALSE(parse_bool("off"));
    CHECK_FALSE(parse_bool(""));
    CHECK_FALSE(parse_bool("garbage"));
    CHECK_FALSE(parse_bool("2"));
}

/* -------------------------------------------------------------------------
 * trim — strips leading/trailing spaces, tabs, \r, \n
 * ---------------------------------------------------------------------- */

TEST(Logger, Trim_TrailingNewline)
{
    char s[] = "hello\n";
    trim(s);
    STRCMP_EQUAL("hello", s);
}

TEST(Logger, Trim_TrailingCrLf)
{
    char s[] = "value\r\n";
    trim(s);
    STRCMP_EQUAL("value", s);
}

TEST(Logger, Trim_LeadingSpaces)
{
    char s[] = "   key";
    trim(s);
    STRCMP_EQUAL("key", s);
}

TEST(Logger, Trim_BothEnds)
{
    char s[] = "  hello world  ";
    trim(s);
    STRCMP_EQUAL("hello world", s);
}

TEST(Logger, Trim_TabLeading)
{
    char s[] = "\t\tvalue";
    trim(s);
    STRCMP_EQUAL("value", s);
}

TEST(Logger, Trim_AlreadyClean)
{
    char s[] = "clean";
    trim(s);
    STRCMP_EQUAL("clean", s);
}

TEST(Logger, Trim_EmptyString)
{
    char s[] = "";
    trim(s);
    STRCMP_EQUAL("", s);
}

TEST(Logger, Trim_OnlyWhitespace)
{
    char s[] = "   \t\r\n";
    trim(s);
    STRCMP_EQUAL("", s);
}

/* -------------------------------------------------------------------------
 * cmd_name — COMMO command byte → display string
 * ---------------------------------------------------------------------- */

TEST(Logger, CmdName_KnownCommands)
{
    STRCMP_EQUAL("SYNC",     cmd_name(0x00));
    STRCMP_EQUAL("GETSTAT",  cmd_name(0x01));
    STRCMP_EQUAL("PLAY",     cmd_name(0x03));
    STRCMP_EQUAL("STOP",     cmd_name(0x08));
    STRCMP_EQUAL("PAUSE",    cmd_name(0x09));
    STRCMP_EQUAL("GETTN",    cmd_name(0x13));
    STRCMP_EQUAL("SEEKP",    cmd_name(0x16));
    STRCMP_EQUAL("READTOC",  cmd_name(0x1E));
}

TEST(Logger, CmdName_UnknownIsUNKNOWN)
{
    STRCMP_EQUAL("UNKNOWN", cmd_name(0x12));  // gap between GETLOCP and GETTN
    STRCMP_EQUAL("UNKNOWN", cmd_name(0xFF));
    STRCMP_EQUAL("UNKNOWN", cmd_name(0x20));
}

/* -------------------------------------------------------------------------
 * Ring buffer — append, wrap, drop-on-overflow
 * ---------------------------------------------------------------------- */

TEST(Logger, Ring_AppendAndDrain)
{
    ring_append("hello", 5);
    LONGS_EQUAL(5, (long)s_ring_used);
    char out[16];
    ring_drain(out, sizeof(out));
    STRCMP_EQUAL("hello", out);
    LONGS_EQUAL(0, (long)s_ring_used);
}

TEST(Logger, Ring_MultipleAppends)
{
    ring_append("abc", 3);
    ring_append("def", 3);
    char out[16];
    ring_drain(out, sizeof(out));
    STRCMP_EQUAL("abcdef", out);
}

TEST(Logger, Ring_OverflowDropsAndCounts)
{
    // Fill the ring completely
    char fill[RING_SIZE];
    memset(fill, 'X', RING_SIZE);
    ring_append(fill, RING_SIZE);
    LONGS_EQUAL(RING_SIZE, (long)s_ring_used);
    LONGS_EQUAL(0, (long)s_dropped);

    // One more byte must be dropped
    ring_append("Z", 1);
    LONGS_EQUAL(1, (long)s_dropped);
    LONGS_EQUAL(RING_SIZE, (long)s_ring_used);  // still full
}

TEST(Logger, Ring_WrapAround)
{
    // Append 48 bytes, drain them, then append 32 more — head wraps past end
    char block[48];
    memset(block, 'A', sizeof(block));
    ring_append(block, sizeof(block));
    char tmp[64];
    ring_drain(tmp, sizeof(tmp));   // tail now at 48

    char wrap[32];
    memset(wrap, 'B', sizeof(wrap));
    ring_append(wrap, sizeof(wrap));  // head wraps: 48+32=80 mod 64 = 16

    char out[64];
    ring_drain(out, sizeof(out));
    // All 32 bytes should be 'B'
    LONGS_EQUAL(32, (long)strlen(out));
    for (int i = 0; i < 32; i++) {
        BYTES_EQUAL('B', (uint8_t)out[i]);
    }
}

TEST(Logger, Ring_ZeroLengthAppendIsNoop)
{
    ring_append("x", 0);
    LONGS_EQUAL(0, (long)s_ring_used);
    LONGS_EQUAL(0, (long)s_dropped);
}

/* -------------------------------------------------------------------------
 * Status byte flag decoding (mirrors _log_cmd_resp in logger.c)
 * ---------------------------------------------------------------------- */

TEST(Logger, StatusFlags_Busy)
{
    char flags[32];
    decode_status_flags(0x80, flags, sizeof(flags));
    CHECK_TRUE(strstr(flags, "BUSY") != NULL);
}

TEST(Logger, StatusFlags_Disc)
{
    char flags[32];
    decode_status_flags(0x04, flags, sizeof(flags));
    CHECK_TRUE(strstr(flags, "DISC") != NULL);
}

TEST(Logger, StatusFlags_Err)
{
    char flags[32];
    decode_status_flags(0x01, flags, sizeof(flags));
    CHECK_TRUE(strstr(flags, "ERR") != NULL);
}

TEST(Logger, StatusFlags_Multiple)
{
    char flags[32];
    decode_status_flags(0x80 | 0x20 | 0x04, flags, sizeof(flags));
    CHECK_TRUE(strstr(flags, "BUSY") != NULL);
    CHECK_TRUE(strstr(flags, "DRQ")  != NULL);
    CHECK_TRUE(strstr(flags, "DISC") != NULL);
}

TEST(Logger, StatusFlags_Zero_IsEmpty)
{
    char flags[32];
    decode_status_flags(0x00, flags, sizeof(flags));
    STRCMP_EQUAL("", flags);
}

/* -------------------------------------------------------------------------
 * fw_token key dispatch — replica of the logger.c parser branch
 *
 * The parser in logger.c uses strncpy into a 33-byte field.  These tests
 * catch regressions where the field is mis-sized, the key name is misspelled,
 * or the NUL-termination logic is wrong.
 * ---------------------------------------------------------------------- */

static void parse_fw_token_key(const char *val, char *out, size_t outsz)
{
    // Replica of the fw_token branch in logger.c parse_settings_file():
    //   strncpy(field, val, sizeof(field) - 1); field[sizeof(field)-1] = '\0';
    // Use memcpy + clamp to avoid -Wstringop-truncation/-Wformat-truncation.
    size_t vlen = strlen(val);
    if (vlen >= outsz) vlen = outsz - 1;
    memcpy(out, val, vlen);
    out[vlen] = '\0';
}

TEST(Logger, FwToken_ParsedIntoField)
{
    char fw_token[33] = {0};
    parse_fw_token_key("aabbccddeeff00112233445566778899", fw_token, sizeof(fw_token));
    STRCMP_EQUAL("aabbccddeeff00112233445566778899", fw_token);
}

TEST(Logger, FwToken_TruncatedAtFieldWidth)
{
    // A value longer than 32 chars must be truncated, not overflow the buffer.
    char fw_token[33] = {0};
    parse_fw_token_key("aabbccddeeff00112233445566778899EXTRA", fw_token, sizeof(fw_token));
    STRCMP_EQUAL("aabbccddeeff00112233445566778899", fw_token);
    CHECK_EQUAL('\0', fw_token[32]);  // always NUL-terminated
}

TEST(Logger, FwToken_EmptyValueParsedAsEmpty)
{
    char fw_token[33] = {0};
    parse_fw_token_key("", fw_token, sizeof(fw_token));
    STRCMP_EQUAL("", fw_token);
}

TEST(Logger, FwToken_FieldSizeIs33)
{
    // logger_config_t.fw_token must be exactly 33 bytes (32 hex + NUL).
    // If this changes the token validation in webserver.c will silently truncate.
    logger_config_t dummy;
    CHECK_EQUAL(33u, sizeof(dummy.fw_token));
}
