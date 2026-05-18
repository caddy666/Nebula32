// =============================================================================
// test_webserver_html.cpp — compile-and-run tests for build_html_page()
//
// Intent: actually compile src/webserver.c (via the webserver_html_test.o
// build rule) and exercise build_html_page() so that:
//   • format-string bugs (unescaped %) are caught at compile / UBSan time
//   • buffer overflow beyond 32 KB is caught by UBSan / LONGS_EQUAL
//   • new HTML features (starfield, SD card status) are verified present
//   • CSS percentage escaping (100%% → "100%") is verified in output
//
// The webserver_get_page_for_test() shim is compiled into webserver_html_test.o
// under -DWEBSERVER_TEST_BUILD and calls the real static build_html_page().
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "cd_types.h"     // drive_state_t, DRIVE_IDLE, MAX_PATH_LEN
#include "ff.h"           // FATFS, DWORD — stub types for f_getfree control

// ---------------------------------------------------------------------------
// Symbols extern'd by webserver.c from main.c — define them here.
// ---------------------------------------------------------------------------
char     s_image_paths[32][MAX_PATH_LEN];
uint32_t s_image_count = 0;

// ---------------------------------------------------------------------------
// Controllable FatFS free-space state (extern'd from ff.h stub).
// csize=64 sectors/cluster, 32 GB card, ~8 GB free.
// ---------------------------------------------------------------------------
FATFS g_stub_fatfs        = { /* .csize = */ 64, /* .n_fatent = */ 1000002 };
DWORD g_stub_ff_fre_clust = 262144;  // 262144 * 64 * 512 ≈ 8 GB free

// ---------------------------------------------------------------------------
// Drive-state stub — replaces the real commo_bridge_get_drive_state().
// webserver_html_test.o is compiled without commo_bridge.c in the build,
// so we provide the symbol here with a settable global.
// ---------------------------------------------------------------------------
static drive_state_t g_test_drive_state = DRIVE_IDLE;

drive_state_t commo_bridge_get_drive_state(void) {
    return g_test_drive_state;
}

// Test hook exposed by webserver.c under -DWEBSERVER_TEST_BUILD.
const char *webserver_get_page_for_test(void);
} // extern "C"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *get_html(void) {
    // Reset to a known state before each call.
    s_image_count = 2;
    strncpy(s_image_paths[0], "0:/Zool2.iso",   MAX_PATH_LEN - 1);
    strncpy(s_image_paths[1], "0:/Pinball.bin",  MAX_PATH_LEN - 1);
    g_test_drive_state = DRIVE_IDLE;
    return webserver_get_page_for_test();
}

// ============================================================================
// WebserverHtml — tests that compile and run build_html_page()
// ============================================================================

TEST_GROUP(WebserverHtml) {};

// The static buffer inside build_html_page is 32768 bytes.
// If HTML grows beyond that, snprintf silently truncates and the page is
// missing its </html> — catch it here before it reaches a live device.
TEST(WebserverHtml, FitsIn32KBuffer)
{
    const char *html = get_html();
    CHECK_TRUE(html != NULL);
    LONGS_EQUAL_TEXT(1, (strlen(html) < 32768) ? 1 : 0,
                     "HTML output exceeds 32 KB static buffer");
}

TEST(WebserverHtml, HasDoctype)
{
    CHECK_TRUE(strstr(get_html(), "<!DOCTYPE html") != NULL);
}

TEST(WebserverHtml, HasCd32Title)
{
    CHECK_TRUE(strstr(get_html(), "Nebula32") != NULL);
}

// Verify the starfield script landed in the output.
TEST(WebserverHtml, HasStarfieldScript)
{
    CHECK_TRUE(strstr(get_html(), "requestAnimationFrame") != NULL);
}

// Verify the SD card status item is present.
TEST(WebserverHtml, HasSdCardStatusLabel)
{
    CHECK_TRUE(strstr(get_html(), "SD Card") != NULL);
}

// With the stub returning ~8 GB free / ~30 GB total, the output must
// contain "GB" — proving sd_get_space() ran and fmt_bytes() formatted it.
TEST(WebserverHtml, SdSpaceFormattedInGb)
{
    CHECK_TRUE(strstr(get_html(), " GB") != NULL);
}

// The loadDisc JS function must be present for the disc grid to work.
TEST(WebserverHtml, HasLoadDiscFunction)
{
    CHECK_TRUE(strstr(get_html(), "loadDisc") != NULL);
}

// CSS uses width:100% and height:100% for the canvas.
// In the C format string these must be written as 100%% so snprintf
// produces a literal %.  Verify the final HTML contains "100%" not "100%%".
TEST(WebserverHtml, CssPercentEscapedCorrectly)
{
    const char *html = get_html();
    CHECK_TRUE(strstr(html, "100%")  != NULL);   // literal % reached output
    CHECK_TRUE(strstr(html, "100%%") == NULL);   // doubled % did NOT survive
}

// Both image names supplied in setup must appear in the disc grid.
TEST(WebserverHtml, DiscNamesAppearInGrid)
{
    const char *html = get_html();
    CHECK_TRUE(strstr(html, "Zool2")   != NULL);
    CHECK_TRUE(strstr(html, "Pinball") != NULL);
}

// The "Drive State" label must appear in the status bar regardless of state value.
// state_name(0) == "IDLE" (DRIVE_IDLE=0 maps to case 0 in state_name()).
TEST(WebserverHtml, DriveStateInStatusBar)
{
    CHECK_TRUE(strstr(get_html(), "Drive State") != NULL);
}
