// =============================================================================
// test_da_speed.cpp — DA output playback state machine tests
//
// Functions under test (replicated from src/da_output.c):
//   da_output_init()      — initialise state; sets double_speed + s_initialised
//   da_set_double_speed() — idempotency guard; writes clkdiv on change only
//   da_is_double_speed()  — read speed flag
//   da_start_play()       — set s_playing = true, s_paused = false
//   da_stop()             — set s_playing = false, s_paused = false
//   da_pause()            — set s_paused = true (s_playing unchanged)
//   da_resume()           — set s_paused = false
//   da_is_playing()       — s_playing && !s_paused
//
// DaExpand already covers the clkdiv arithmetic (32.0 = 1×, 16.0 = 2×).
// This group covers state transitions and the idempotency guard in
// da_set_double_speed() — the original test_da_speed.c had wrong clkdiv
// values (24/48) because it pre-dated the HIGH-4 fix; those are corrected here.
//
// Relevant production code: src/da_output.c
// =============================================================================

#include <CppUTest/TestHarness.h>

// ---------------------------------------------------------------------------
// Stub — captures pio_sm_set_clkdiv() calls (not linked in host build)
// ---------------------------------------------------------------------------

static float g_stub_last_clkdiv = 0.0f;

// ---------------------------------------------------------------------------
// State machine replicated from src/da_output.c
// Changes here must be reflected in the production source and vice versa.
// ---------------------------------------------------------------------------

static bool s_playing      = false;
static bool s_paused       = false;
static bool s_double_speed = false;
static bool s_initialised  = false;

static float _clkdiv(bool dbl) { return dbl ? 16.0f : 32.0f; }

static void da_output_init(bool double_speed)
{
    s_double_speed = double_speed;
    s_playing      = false;
    s_paused       = false;
    s_initialised  = true;
}

static void da_set_double_speed(bool double_speed)
{
    if (!s_initialised)                  return;
    if (s_double_speed == double_speed)  return;  // idempotency guard
    s_double_speed     = double_speed;
    g_stub_last_clkdiv = _clkdiv(double_speed);   // would call pio_sm_set_clkdiv
}

static bool da_is_double_speed(void) { return s_double_speed; }

static void da_start_play(void)
{
    if (!s_initialised) return;
    s_playing = true;
    s_paused  = false;
}

static void da_stop(void)
{
    if (!s_initialised) return;
    s_playing = false;
    s_paused  = false;
}

static void da_pause(void)
{
    if (!s_playing || s_paused) return;
    s_paused = true;
}

static void da_resume(void)
{
    if (!s_playing || !s_paused) return;
    s_paused = false;
}

static bool da_is_playing(void) { return s_playing && !s_paused; }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_GROUP(DaSpeed)
{
    void setup() override
    {
        s_playing      = false;
        s_paused       = false;
        s_double_speed = false;
        s_initialised  = false;
        g_stub_last_clkdiv = 0.0f;
    }
};

/* -------------------------------------------------------------------------
 * Initial state — before da_output_init()
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, NotPlaying_BeforeInit)
{
    CHECK_FALSE(da_is_playing());
}

TEST(DaSpeed, NotDoubleSpeed_BeforeInit)
{
    CHECK_FALSE(da_is_double_speed());
}

/* -------------------------------------------------------------------------
 * da_output_init
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, Init1x_NotDoubleSpeed)
{
    da_output_init(false);
    CHECK_FALSE(da_is_double_speed());
    CHECK_FALSE(da_is_playing());
}

TEST(DaSpeed, Init2x_IsDoubleSpeed)
{
    da_output_init(true);
    CHECK_TRUE(da_is_double_speed());
    CHECK_FALSE(da_is_playing());
}

/* -------------------------------------------------------------------------
 * da_set_double_speed — clkdiv capture (values verified in DaExpand)
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, SetDouble_True_WritesClkdiv16)
{
    da_output_init(false);
    da_set_double_speed(true);
    CHECK_TRUE(da_is_double_speed());
    DOUBLES_EQUAL(16.0, g_stub_last_clkdiv, 0.001);
}

TEST(DaSpeed, SetDouble_False_WritesClkdiv32)
{
    da_output_init(true);
    da_set_double_speed(false);
    CHECK_FALSE(da_is_double_speed());
    DOUBLES_EQUAL(32.0, g_stub_last_clkdiv, 0.001);
}

/* -------------------------------------------------------------------------
 * da_set_double_speed — idempotency (no pio_sm_set_clkdiv on same-value call)
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, Idempotent_1xTo1x_NoClkdivWrite)
{
    da_output_init(false);   // starts at 1×
    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(false);
    DOUBLES_EQUAL(0.0, g_stub_last_clkdiv, 0.001);
}

TEST(DaSpeed, Idempotent_2xTo2x_NoClkdivWrite)
{
    da_output_init(false);
    da_set_double_speed(true);    // move to 2×
    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(true);    // same value again
    DOUBLES_EQUAL(0.0, g_stub_last_clkdiv, 0.001);
}

/* -------------------------------------------------------------------------
 * Playback state machine
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, StartPlay_IsPlaying)
{
    da_output_init(false);
    da_start_play();
    CHECK_TRUE(da_is_playing());
}

TEST(DaSpeed, Pause_NotPlaying)
{
    da_output_init(false);
    da_start_play();
    da_pause();
    CHECK_FALSE(da_is_playing());
}

TEST(DaSpeed, Resume_IsPlayingAgain)
{
    da_output_init(false);
    da_start_play();
    da_pause();
    da_resume();
    CHECK_TRUE(da_is_playing());
}

TEST(DaSpeed, Stop_NotPlaying)
{
    da_output_init(false);
    da_start_play();
    da_stop();
    CHECK_FALSE(da_is_playing());
}

TEST(DaSpeed, Pause_WhenNotPlaying_IsNoOp)
{
    da_output_init(false);   // s_playing = false
    da_pause();              // guard: !s_playing → return
    CHECK_FALSE(da_is_playing());
}

TEST(DaSpeed, Resume_WhenNotPaused_IsNoOp)
{
    da_output_init(false);
    da_start_play();
    da_resume();             // guard: !s_paused → return; still playing
    CHECK_TRUE(da_is_playing());
}

/* -------------------------------------------------------------------------
 * Speed change while playing — playing state preserved, clkdiv updated
 * ---------------------------------------------------------------------- */

TEST(DaSpeed, SpeedChange_WhilePlaying_StillPlaying)
{
    da_output_init(false);
    da_start_play();
    da_set_double_speed(true);
    CHECK_TRUE(da_is_playing());
    da_set_double_speed(false);
    CHECK_TRUE(da_is_playing());
}

TEST(DaSpeed, SpeedChange_WhilePlaying_ClkdivUpdated)
{
    da_output_init(false);
    da_start_play();

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(true);
    DOUBLES_EQUAL(16.0, g_stub_last_clkdiv, 0.001);

    g_stub_last_clkdiv = 0.0f;
    da_set_double_speed(false);
    DOUBLES_EQUAL(32.0, g_stub_last_clkdiv, 0.001);
}
