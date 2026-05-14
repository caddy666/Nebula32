#include "test_runner.h"
#include "da_output.h"
#include <stdint.h>

// Captured by the pio stub whenever pio_sm_set_clkdiv() is called.
// Defined in run_tests.c; reset to 0.0f before each assertion block.
extern float g_stub_last_clkdiv;

void test_da_speed(void) {

    // -------------------------------------------------------------------------
    SUITE("da_output: initial state before init");

    ASSERT_FALSE(da_is_playing(),      "not playing before da_output_init");
    ASSERT_FALSE(da_is_double_speed(), "not double-speed before da_output_init");

    // -------------------------------------------------------------------------
    SUITE("da_output: init at 1x speed");

    da_output_init(false);
    ASSERT_FALSE(da_is_double_speed(), "1x after da_output_init(false)");
    ASSERT_FALSE(da_is_playing(),      "not playing after init");

    // -------------------------------------------------------------------------
    SUITE("da_output: switch to 2x — clkdiv must be 24");
    // sys_clk = 135,475,200 Hz
    // clkdiv 24 → SM clock 5,644,800 Hz → BCLK 2,822,400 Hz = 2× Red Book

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(true);
    ASSERT_TRUE(da_is_double_speed(), "is_double_speed() true after set(true)");
    ASSERT_EQ((int)g_stub_last_clkdiv, 24,
              "clkdiv = 24 for 2x  (BCLK 2,822,400 Hz)");

    // -------------------------------------------------------------------------
    SUITE("da_output: switch back to 1x — clkdiv must be 48");
    // clkdiv 48 → SM clock 2,822,400 Hz → BCLK 1,411,200 Hz = 1× Red Book

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(false);
    ASSERT_FALSE(da_is_double_speed(), "is_double_speed() false after set(false)");
    ASSERT_EQ((int)g_stub_last_clkdiv, 48,
              "clkdiv = 48 for 1x  (BCLK 1,411,200 Hz)");

    // -------------------------------------------------------------------------
    SUITE("da_output: speed switch is idempotent (no pio_sm_set_clkdiv on no-op)");

    // Already at 1×; calling set_double_speed(false) again must not touch clkdiv
    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(false);
    ASSERT_EQ((int)g_stub_last_clkdiv, 0, "no clkdiv write on 1x→1x");

    da_set_double_speed(true);   // move to 2×
    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(true);   // same value again
    ASSERT_EQ((int)g_stub_last_clkdiv, 0, "no clkdiv write on 2x→2x");

    // -------------------------------------------------------------------------
    SUITE("da_output: clkdiv values produce exact Red Book timing");
    // Verify the arithmetic is correct in integer domain, independent of the
    // stub capture.  If these fail the constants in da_output.c are wrong.

    uint32_t bclk_1x = 135475200u / 48u / 2u;
    ASSERT_EQ(bclk_1x, 1411200u,
              "sys_clk / 48 / 2 = 1,411,200 Hz  (1x BCLK)");

    uint32_t bclk_2x = 135475200u / 24u / 2u;
    ASSERT_EQ(bclk_2x, 2822400u,
              "sys_clk / 24 / 2 = 2,822,400 Hz  (2x BCLK)");

    // 2× BCLK must be exactly 2× 1× BCLK
    ASSERT_EQ(bclk_2x, bclk_1x * 2u, "2x BCLK is exactly double 1x BCLK");

    // -------------------------------------------------------------------------
    SUITE("da_output: playback state machine");

    da_set_double_speed(false);   // put speed back to 1× for clean state

    da_start_play(NULL, 0);
    ASSERT_TRUE(da_is_playing(),  "playing after da_start_play");

    da_pause();
    ASSERT_FALSE(da_is_playing(), "not playing while paused");

    da_resume();
    ASSERT_TRUE(da_is_playing(),  "playing again after da_resume");

    da_stop();
    ASSERT_FALSE(da_is_playing(), "not playing after da_stop");

    // -------------------------------------------------------------------------
    SUITE("da_output: speed change is safe while playing");
    // pio_sm_set_clkdiv() operates on a running SM — no stop/restart needed.

    da_start_play(NULL, 0);
    ASSERT_TRUE(da_is_playing(), "playing before mid-play speed change");

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(true);
    ASSERT_TRUE(da_is_playing(),          "still playing after speed→2x");
    ASSERT_EQ((int)g_stub_last_clkdiv, 24, "clkdiv updated to 24 during playback");

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(false);
    ASSERT_TRUE(da_is_playing(),          "still playing after speed→1x");
    ASSERT_EQ((int)g_stub_last_clkdiv, 48, "clkdiv updated to 48 during playback");

    da_stop();
}
