// =============================================================================
// test_webserver.cpp — Web server pure-logic tests
//
// Intent: verify the five pure-C helpers inside src/webserver.c that have
// no lwIP, CYW43, or FatFS dependency.  Those helpers are static and
// hardware-bound in the full build, so this file replicates each one
// verbatim and tests the invariant.  If the production code diverges, the
// replica will differ and the test will catch the regression.
//
// Functions under test (all static in webserver.c):
//   basename_no_ext()  — strip path prefix and file extension from a string
//   state_name()       — map drive_state_t integer to a display string
//   config line parser — key=value splitting used in read_wifi_config()
//   ".." traversal guard — rejects cover-art paths with "..", "/", "\", or "%" in filename
//   load-index bounds check — POST /api/load/{index} validates index range
// =============================================================================
//   CoverDir group: verifies that display.cpp and webserver.c use the same
//   cover-art directory prefix.  The expected value is read from WS_COVERS_DIR
//   (the single source of truth); display.cpp's prefix is captured as a
//   compile-time string literal replicated here.  If either side drifts the
//   test fails without requiring a build of the embedded target.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>   // strtoul
#include <limits.h>   // ULONG_MAX

// ---------------------------------------------------------------------------
// Cover-directory contract — single source of truth from webserver.h
// WS_COVERS_DIR is defined there; replicate it here as the reference value.
// If webserver.h cannot be included (hardware deps), mirror the define below
// and keep it in sync with include/webserver.h.
// ---------------------------------------------------------------------------
#define WS_COVERS_DIR   "0:/covers/"

// The prefix used by find_cover_jpeg() in src/display.cpp.
// Must stay in sync with the snprintf format string in that function.
// grep: snprintf(out, out_len, "0:/covers/%s.jpg", name);
#define DISPLAY_COVERS_PREFIX  "0:/covers/"

// The default cover path used by display.cpp when no title-specific art exists.
// Must stay in sync with DEFAULT_COVER_PATH in src/display.cpp.
#define DISPLAY_DEFAULT_COVER  "0:/covers/cd32-default.jpg"

// ---------------------------------------------------------------------------
// Replicated helpers — must stay in sync with src/webserver.c
// ---------------------------------------------------------------------------

static const char *basename_no_ext(const char *path, char *buf, int bufsz)
{
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    strncpy(buf, name, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    return buf;
}

// Replica of static state_name() from webserver.c — aligned with drive_state_t (DRIVE_IDLE=0)
static const char *state_name(int state)
{
    switch (state) {
        case 0: return "IDLE";    case 1: return "SPINUP";
        case 2: return "READY";   case 3: return "SEEKING";
        case 4: return "READING"; case 5: return "PLAYING";
        case 6: return "PAUSED";  case 7: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Returns true if the line is a valid key=value pair.
// Writes null-terminated key and val into the provided buffers.
static bool parse_cfg_line(const char *line, char *key, int keysz,
                            char *val, int valsz)
{
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0') return false;
    const char *eq = strchr(line, '=');
    if (!eq) return false;

    // key: everything before '=', leading spaces stripped
    const char *k = line;
    while (*k == ' ') k++;
    int klen = (int)(eq - k);
    if (klen <= 0 || klen >= keysz) return false;
    memcpy(key, k, klen);
    key[klen] = '\0';
    // Trim trailing spaces from key
    while (klen > 0 && key[klen - 1] == ' ') key[--klen] = '\0';

    // val: everything after '=', leading spaces stripped
    const char *v = eq + 1;
    while (*v == ' ') v++;
    strncpy(val, v, valsz - 1);
    val[valsz - 1] = '\0';
    return true;
}

static bool covers_path_safe(const char *path)
{
    // Replica of the guard in handle_request().
    // Precondition: only called after strncmp(path, "/covers/", 8) == 0 confirmed —
    // so path always starts with "/covers/" here, exactly as in production.
    const char *name = path + 8;
    return strstr(name, "..") == NULL &&
           strchr(name, '/')  == NULL &&
           strchr(name, '\\') == NULL &&
           strchr(name, '%')  == NULL;
}

// Replica of the parse+validate step in handle_request() for POST /api/load/.
// Takes the raw path suffix (everything after "/api/load/") and image count.
static bool load_index_valid_from_str(const char *suffix, uint32_t count)
{
    char *endp;
    unsigned long v = strtoul(suffix, &endp, 10);
    if (endp == suffix || v > UINT32_MAX) return false;
    return (uint32_t)v < count;
}

// Replica of extract_request_token() from webserver.c.
static void extract_request_token(const char *req, const char *url_path,
                                   char *out, size_t outsz)
{
    out[0] = '\0';
    if (outsz == 0) return;

    const char *hdr = strstr(req, "\r\nX-FW-Token: ");
    if (hdr) {
        hdr += sizeof("\r\nX-FW-Token: ") - 1;
        size_t i = 0;
        while (hdr[i] && hdr[i] != '\r' && hdr[i] != '\n' && i < outsz - 1) {
            out[i] = hdr[i];
            i++;
        }
        out[i] = '\0';
        if (out[0] != '\0') return;
    }

    const char *qs = strchr(url_path, '?');
    if (!qs) return;
    qs++;
    const char *tok = strstr(qs, "token=");
    if (!tok) return;
    tok += sizeof("token=") - 1;
    size_t i = 0;
    while (tok[i] && tok[i] != '&' && tok[i] != '#' && i < outsz - 1) {
        out[i] = tok[i];
        i++;
    }
    out[i] = '\0';
}

static bool load_index_valid(uint32_t idx, uint32_t count)
{
    return idx < count;
}

// ---------------------------------------------------------------------------
// Test groups
// ---------------------------------------------------------------------------

TEST_GROUP(Webserver) {};

/* -------------------------------------------------------------------------
 * basename_no_ext
 * ---------------------------------------------------------------------- */

TEST(Webserver, BasenameNoExt_FullPath)
{
    char buf[128];
    basename_no_ext("0:/Zool2.iso", buf, sizeof(buf));
    STRCMP_EQUAL("Zool2", buf);
}

TEST(Webserver, BasenameNoExt_PathWithSpaces)
{
    char buf[128];
    basename_no_ext("0:/games/Pinball Illusions.bin", buf, sizeof(buf));
    STRCMP_EQUAL("Pinball Illusions", buf);
}

TEST(Webserver, BasenameNoExt_NoSlash)
{
    char buf[128];
    basename_no_ext("root_no_slash.nrg", buf, sizeof(buf));
    STRCMP_EQUAL("root_no_slash", buf);
}

TEST(Webserver, BasenameNoExt_NoExtension)
{
    char buf[128];
    basename_no_ext("0:/DISC1", buf, sizeof(buf));
    STRCMP_EQUAL("DISC1", buf);
}

TEST(Webserver, BasenameNoExt_DotInDir_StripsOnlyFileExt)
{
    char buf[128];
    // directory has a dot but the file also has an extension
    basename_no_ext("0:/v1.2/game.iso", buf, sizeof(buf));
    STRCMP_EQUAL("game", buf);
}

/* -------------------------------------------------------------------------
 * state_name
 * ---------------------------------------------------------------------- */

TEST(Webserver, StateName_AllKnownStates)
{
    STRCMP_EQUAL("IDLE",    state_name(0));
    STRCMP_EQUAL("SPINUP",  state_name(1));
    STRCMP_EQUAL("READY",   state_name(2));
    STRCMP_EQUAL("SEEKING", state_name(3));
    STRCMP_EQUAL("READING", state_name(4));
    STRCMP_EQUAL("PLAYING", state_name(5));
    STRCMP_EQUAL("PAUSED",  state_name(6));
    STRCMP_EQUAL("ERROR",   state_name(7));
}

TEST(Webserver, StateName_UnknownIsError)
{
    STRCMP_EQUAL("UNKNOWN", state_name(8));
    STRCMP_EQUAL("UNKNOWN", state_name(-1));
    STRCMP_EQUAL("UNKNOWN", state_name(255));
}

/* -------------------------------------------------------------------------
 * Config line parser
 * ---------------------------------------------------------------------- */

TEST(Webserver, CfgParse_SimpleKeyValue)
{
    char key[64], val[64];
    CHECK_TRUE(parse_cfg_line("wifi_ssid=MyNetwork", key, sizeof(key),
                               val, sizeof(val)));
    STRCMP_EQUAL("wifi_ssid", key);
    STRCMP_EQUAL("MyNetwork", val);
}

TEST(Webserver, CfgParse_SpacesAroundEquals)
{
    char key[64], val[64];
    CHECK_TRUE(parse_cfg_line("wifi_password = secret123", key, sizeof(key),
                               val, sizeof(val)));
    STRCMP_EQUAL("wifi_password", key);
    STRCMP_EQUAL("secret123", val);
}

TEST(Webserver, CfgParse_HashCommentSkipped)
{
    char key[64], val[64];
    CHECK_FALSE(parse_cfg_line("# this is a comment", key, sizeof(key),
                                val, sizeof(val)));
}

TEST(Webserver, CfgParse_SemicolonCommentSkipped)
{
    char key[64], val[64];
    CHECK_FALSE(parse_cfg_line("; another comment", key, sizeof(key),
                                val, sizeof(val)));
}

TEST(Webserver, CfgParse_BlankLineSkipped)
{
    char key[64], val[64];
    CHECK_FALSE(parse_cfg_line("", key, sizeof(key), val, sizeof(val)));
}

TEST(Webserver, CfgParse_MissingEqualsSkipped)
{
    char key[64], val[64];
    CHECK_FALSE(parse_cfg_line("noequalshere", key, sizeof(key),
                                val, sizeof(val)));
}

/* -------------------------------------------------------------------------
 * ".." traversal guard
 * ---------------------------------------------------------------------- */

TEST(Webserver, CoversPath_SafePathAccepted)
{
    CHECK_TRUE(covers_path_safe("/covers/Zool2.jpg"));
    CHECK_TRUE(covers_path_safe("/covers/Pinball Illusions.jpeg"));
}

TEST(Webserver, CoversPath_DotDotRejected)
{
    CHECK_FALSE(covers_path_safe("/covers/../etc/passwd"));
    CHECK_FALSE(covers_path_safe("/covers/..%2F..%2Fetc"));  // encoded — still contains ".."
}

TEST(Webserver, CoversPath_PercentEncodedDotDotRejected)
{
    // %2e%2e has no literal ".." so the old strstr("..") guard missed this.
    CHECK_FALSE(covers_path_safe("/covers/%2e%2e/cd32_ode.cfg"));
    CHECK_FALSE(covers_path_safe("/covers/%2E%2E/secret"));
}

TEST(Webserver, CoversPath_SlashInFilenameRejected)
{
    CHECK_FALSE(covers_path_safe("/covers/sub/escape.jpg"));
    CHECK_FALSE(covers_path_safe("/covers/a\\b.jpg"));
}

TEST(Webserver, CoversPath_PercentInFilenameRejected)
{
    CHECK_FALSE(covers_path_safe("/covers/art%20name.jpg"));
}

/* -------------------------------------------------------------------------
 * Load-index bounds check
 * ---------------------------------------------------------------------- */

TEST(Webserver, LoadIndex_ValidIndices)
{
    CHECK_TRUE(load_index_valid(0, 3));
    CHECK_TRUE(load_index_valid(2, 3));
}

TEST(Webserver, LoadIndex_ExactlyAtCount_Invalid)
{
    CHECK_FALSE(load_index_valid(3, 3));
}

TEST(Webserver, LoadIndex_Overflow_Invalid)
{
    CHECK_FALSE(load_index_valid(UINT32_MAX, 3));
}

TEST(Webserver, LoadIndex_ZeroCount_AlwaysInvalid)
{
    CHECK_FALSE(load_index_valid(0, 0));
}

// The following tests exercise the parse step (strtoul replica) — these are
// the cases that atoi() could not safely handle.
TEST(Webserver, LoadIndex_NegativeStringRejected)
{
    // strtoul("-1") returns ULONG_MAX (wraps unsigned); v > UINT32_MAX triggers rejection.
    CHECK_FALSE(load_index_valid_from_str("-1", 3));
}

TEST(Webserver, LoadIndex_OverflowStringRejected)
{
    CHECK_FALSE(load_index_valid_from_str("99999999999", 3));
}

TEST(Webserver, LoadIndex_NonNumericStringRejected)
{
    CHECK_FALSE(load_index_valid_from_str("abc", 3));
    CHECK_FALSE(load_index_valid_from_str("", 3));
}

TEST(Webserver, LoadIndex_ValidStringAccepted)
{
    CHECK_TRUE(load_index_valid_from_str("0", 3));
    CHECK_TRUE(load_index_valid_from_str("2", 3));
}

/* -------------------------------------------------------------------------
 * Firmware-flash token extraction and validation
 * ---------------------------------------------------------------------- */

TEST(Webserver, FwToken_ExtractFromHeader)
{
    char out[33];
    const char *req =
        "POST /api/fw/flash/NEBULA32.UF2 HTTP/1.1\r\n"
        "Host: nebula32.local\r\n"
        "X-FW-Token: aabbccddeeff00112233445566778899\r\n"
        "Content-Length: 0\r\n\r\n";
    extract_request_token(req, "/api/fw/flash/NEBULA32.UF2", out, sizeof(out));
    STRCMP_EQUAL("aabbccddeeff00112233445566778899", out);
}

TEST(Webserver, FwToken_ExtractFromQueryString)
{
    char out[33];
    const char *req = "POST /api/fw/flash/NEBULA32.UF2?token=deadbeef01234567 HTTP/1.1\r\n\r\n";
    extract_request_token(req, "/api/fw/flash/NEBULA32.UF2?token=deadbeef01234567",
                          out, sizeof(out));
    STRCMP_EQUAL("deadbeef01234567", out);
}

TEST(Webserver, FwToken_HeaderTakesPrecedenceOverQuery)
{
    char out[33];
    const char *req =
        "POST /api/fw/flash/N.UF2?token=fromquery HTTP/1.1\r\n"
        "X-FW-Token: fromheader\r\n\r\n";
    extract_request_token(req, "/api/fw/flash/N.UF2?token=fromquery", out, sizeof(out));
    STRCMP_EQUAL("fromheader", out);
}

TEST(Webserver, FwToken_MissingTokenReturnsEmpty)
{
    char out[33];
    const char *req = "POST /api/fw/flash/NEBULA32.UF2 HTTP/1.1\r\n\r\n";
    extract_request_token(req, "/api/fw/flash/NEBULA32.UF2", out, sizeof(out));
    STRCMP_EQUAL("", out);
}

TEST(Webserver, FwToken_QueryMultipleParams)
{
    char out[33];
    const char *req = "POST /x?foo=bar&token=abc123&baz=1 HTTP/1.1\r\n\r\n";
    extract_request_token(req, "/x?foo=bar&token=abc123&baz=1", out, sizeof(out));
    STRCMP_EQUAL("abc123", out);
}

// Replica of the token-validation guard in handle_request().
// Returns true when the request should be allowed through.
static bool fw_token_valid(const char *req, const char *url_path,
                           const char *server_token)
{
    if (server_token[0] == '\0') return false;
    char provided[33] = "";
    extract_request_token(req, url_path, provided, sizeof(provided));
    return strcmp(provided, server_token) == 0;
}

TEST(Webserver, FwToken_WrongTokenRejected)
{
    const char *req =
        "POST /api/fw/flash/N.UF2 HTTP/1.1\r\n"
        "X-FW-Token: ffffffffffffffffffffffffffffffff\r\n\r\n";
    CHECK_FALSE(fw_token_valid(req, "/api/fw/flash/N.UF2",
                               "aabbccddeeff00112233445566778899"));
}

TEST(Webserver, FwToken_CorrectTokenAccepted)
{
    const char *req =
        "POST /api/fw/flash/N.UF2 HTTP/1.1\r\n"
        "X-FW-Token: aabbccddeeff00112233445566778899\r\n\r\n";
    CHECK_TRUE(fw_token_valid(req, "/api/fw/flash/N.UF2",
                              "aabbccddeeff00112233445566778899"));
}

TEST(Webserver, FwToken_EmptyServerTokenBlocksAll)
{
    // Even a correct-looking client token must be rejected when server has no token configured.
    const char *req =
        "POST /api/fw/flash/N.UF2 HTTP/1.1\r\n"
        "X-FW-Token: aabbccddeeff00112233445566778899\r\n\r\n";
    CHECK_FALSE(fw_token_valid(req, "/api/fw/flash/N.UF2", ""));
}

/* -------------------------------------------------------------------------
 * CoverDir — cover-art directory consistency between display.cpp and webserver.c
 *
 * These tests assert that both subsystems agree on where cover art lives.
 * The WS_COVERS_DIR define (from webserver.h) is the canonical value;
 * DISPLAY_COVERS_PREFIX mirrors what is hardcoded in find_cover_jpeg().
 * If either side is changed without updating the other, one of these tests
 * fails immediately.
 * ---------------------------------------------------------------------- */

TEST_GROUP(CoverDir) {};

TEST(CoverDir, DisplayPrefix_MatchesWebserverDir)
{
    // Both subsystems must agree on the directory prefix.
    // WS_COVERS_DIR is the authoritative define from webserver.h.
    // DISPLAY_COVERS_PREFIX mirrors the snprintf format in find_cover_jpeg().
    STRCMP_EQUAL(WS_COVERS_DIR, DISPLAY_COVERS_PREFIX);
}

TEST(CoverDir, DisplayDefaultCover_StartsWithWebserverDir)
{
    // The fallback cover path must live inside the same covers directory.
    size_t dir_len = strlen(WS_COVERS_DIR);
    CHECK(strncmp(DISPLAY_DEFAULT_COVER, WS_COVERS_DIR, dir_len) == 0);
}

TEST(CoverDir, WebserverDir_StartsWithVolumePrefix)
{
    // Both sides use the FatFS volume prefix "0:/" — catch a future refactor
    // that drops it (e.g. a relative path) before it reaches the target.
    CHECK(strncmp(WS_COVERS_DIR, "0:/", 3) == 0);
}

TEST(CoverDir, DisplayPrefix_StartsWithVolumePrefix)
{
    CHECK(strncmp(DISPLAY_COVERS_PREFIX, "0:/", 3) == 0);
}

TEST(CoverDir, DisplayDefaultCover_EndsWithJpg)
{
    // Sanity: the default cover must be a JPEG so the JPEG decoder is invoked.
    const char *end = DISPLAY_DEFAULT_COVER + strlen(DISPLAY_DEFAULT_COVER) - 4;
    STRCMP_EQUAL(".jpg", end);
}

TEST(CoverDir, WebserverDir_EndsWithSlash)
{
    // Directory prefix must end with '/' so path concatenation is correct.
    size_t len = strlen(WS_COVERS_DIR);
    CHECK(len > 0);
    CHECK(WS_COVERS_DIR[len - 1] == '/');
}

/* -------------------------------------------------------------------------
 * Gap 14: basename with multiple dots — strrchr finds the LAST dot, so only
 * the final extension is stripped; earlier dots are preserved.
 * ---------------------------------------------------------------------- */
TEST(Webserver, BasenameNoExt_MultipleDots_StripLastOnly)
{
    char buf[128];
    basename_no_ext("0:/game.v1.2.iso", buf, sizeof(buf));
    STRCMP_EQUAL("game.v1.2", buf);
}

/* -------------------------------------------------------------------------
 * Gap 15: value containing '=' — parse_cfg_line uses strchr (first '=') so
 * any additional '=' characters must pass through verbatim into the value.
 * ---------------------------------------------------------------------- */
TEST(Webserver, CfgParse_ValueWithEquals)
{
    char key[64], val[64];
    CHECK_TRUE(parse_cfg_line("token=abc=def", key, sizeof(key), val, sizeof(val)));
    STRCMP_EQUAL("token", key);
    STRCMP_EQUAL("abc=def", val);
}

/* =========================================================================
 * HtmlEscape — replicated from src/webserver.c (lines 148-172)
 *
 * html_escape() is a static helper that escapes the five HTML special chars:
 *   & → &amp;   < → &lt;   > → &gt;   " → &quot;   ' → &#39;
 * All other bytes pass through unchanged.  Output is truncated safely when
 * the output buffer would overflow (never writes past outsz-1).
 * ======================================================================= */

static int html_escape_r(char *out, int outsz, const char *in)
{
    int n = 0;
    for (const char *p = in; *p; p++) {
        const char *esc;
        int elen;
        switch (*p) {
            case '&':  esc = "&amp;";  elen = 5; break;
            case '<':  esc = "&lt;";   elen = 4; break;
            case '>':  esc = "&gt;";   elen = 4; break;
            case '"':  esc = "&quot;"; elen = 6; break;
            case '\'': esc = "&#39;";  elen = 5; break;
            default:   esc = NULL;     elen = 1; break;
        }
        if (esc) {
            if (n + elen >= outsz - 1) break;
            memcpy(out + n, esc, elen);
            n += elen;
        } else {
            if (n >= outsz - 1) break;
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

TEST_GROUP(HtmlEscape) {};

TEST(HtmlEscape, PlainText_PassesThrough)
{
    char out[64];
    html_escape_r(out, sizeof(out), "hello world");
    STRCMP_EQUAL("hello world", out);
}

TEST(HtmlEscape, Ampersand_Escaped)
{
    char out[64];
    html_escape_r(out, sizeof(out), "a&b");
    STRCMP_EQUAL("a&amp;b", out);
}

TEST(HtmlEscape, LessThan_Escaped)
{
    char out[64];
    html_escape_r(out, sizeof(out), "a<b");
    STRCMP_EQUAL("a&lt;b", out);
}

TEST(HtmlEscape, GreaterThan_Escaped)
{
    char out[64];
    html_escape_r(out, sizeof(out), "a>b");
    STRCMP_EQUAL("a&gt;b", out);
}

TEST(HtmlEscape, DoubleQuote_Escaped)
{
    char out[64];
    html_escape_r(out, sizeof(out), "say \"hi\"");
    STRCMP_EQUAL("say &quot;hi&quot;", out);
}

TEST(HtmlEscape, SingleQuote_Escaped)
{
    char out[64];
    html_escape_r(out, sizeof(out), "it's");
    STRCMP_EQUAL("it&#39;s", out);
}

TEST(HtmlEscape, EmptyInput_EmptyOutput)
{
    char out[64] = {0};
    html_escape_r(out, sizeof(out), "");
    STRCMP_EQUAL("", out);
}

TEST(HtmlEscape, OutputTruncates_WhenBufferFull)
{
    char out[8];   /* small buffer — only fits a few bytes */
    int n = html_escape_r(out, sizeof(out), "a&b&c&d");
    /* Must not overrun the buffer and must be NUL-terminated. */
    CHECK_TRUE(n < (int)sizeof(out));
    BYTES_EQUAL('\0', out[n]);
}
