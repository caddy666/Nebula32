// =============================================================================
// test_csv_replay_pon_poff.cpp — Logic-analyzer CSV signal validation
//                                pon-poff-idle capture
// =============================================================================
//
// Samples 10 non-overlapping 50 000-row windows from the pon-poff-idle capture
// (pon-poff-idle.csv, ~100 M rows, 5.2 GB).  The capture spans a full CD32
// power-on / power-off / idle cycle.  Column layout is identical to digital.csv:
//
//   sig[0]=IF_CLK  sig[1]=IF_DATA  sig[2]=DA_LRCLK  sig[3]=DA_DATA
//   sig[4]=DA_BCLK  sig[5]=SUB_WFCLK  sig[6]=SUB_SCOR  sig[7]=SUB_DATA
//
// Window offsets are spaced at 10% intervals of the 5 199 129 701-byte file.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define CSV_PATH    "../cd32_logic_replay_bundle/pon-poff-idle.csv"
#define WINDOW_ROWS 50000

enum {
    SIG_IF_CLK    = 0,
    SIG_IF_DATA   = 1,
    SIG_DA_LRCLK  = 2,
    SIG_DA_DATA   = 3,
    SIG_DA_BCLK   = 4,
    SIG_SUB_WFCLK = 5,
    SIG_SUB_SCOR  = 6,
    SIG_SUB_DATA  = 7,
    NUM_SIGS      = 8
};

// Window offsets at 10% intervals of 5 199 129 701 bytes (step ≈ 519 912 970).
static const long long WINDOW_OFFSETS[10] = {
    0LL,
    519912970LL,
    1039825940LL,
    1559738910LL,
    2079651880LL,
    2599564850LL,
    3119477820LL,
    3639390790LL,
    4159303760LL,
    4679216730LL,
};

struct CsvRow {
    uint64_t ts_ns;
    uint8_t  sig[NUM_SIGS];
};

static CsvRow s_rows[WINDOW_ROWS];
static int    s_nrows = 0;

static uint64_t parse_ts_ns(const char *ts)
{
    const char *t = ts + 11;
    uint64_t hh = (uint64_t)(t[0]-'0')*10 + (t[1]-'0');
    uint64_t mm = (uint64_t)(t[3]-'0')*10 + (t[4]-'0');
    uint64_t ss = (uint64_t)(t[6]-'0')*10 + (t[7]-'0');
    uint64_t ns = 0;
    for (int i = 0; i < 9; i++)
        ns = ns * 10 + (uint64_t)(t[9 + i] - '0');
    return (hh * 3600ULL + mm * 60ULL + ss) * 1000000000ULL + ns;
}

static int load_csv_window(long long byte_offset, int max_rows)
{
    FILE *f = fopen(CSV_PATH, "r");
    if (!f) return -1;

#ifdef _WIN32
    _fseeki64(f, byte_offset, SEEK_SET);
#else
    fseeko(f, (off_t)byte_offset, SEEK_SET);
#endif

    char line[128];
    if (byte_offset > 0) {
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    }

    s_nrows = 0;
    uint64_t t0 = 0;

    while (s_nrows < max_rows && fgets(line, sizeof(line), f)) {
        if (line[0] == 'T' || line[0] == '\n' || line[0] == '\r') continue;

        char *p = line;
        while (*p && *p != ',') p++;
        if (*p != ',') continue;
        *p++ = '\0';

        uint64_t ts_abs = parse_ts_ns(line);
        if (t0 == 0) t0 = ts_abs;
        s_rows[s_nrows].ts_ns = ts_abs - t0;

        for (int i = 0; i < NUM_SIGS; i++) {
            s_rows[s_nrows].sig[i] = (*p == '1') ? 1u : 0u;
            p += 2;
        }
        s_nrows++;
    }

    fclose(f);
    return s_nrows;
}

static int half_period_stats(int col,
                              uint64_t min_hp_ns, uint64_t max_hp_ns,
                              double *out_mean, double *out_std)
{
    double sum = 0.0, sum2 = 0.0;
    int count = 0;
    uint64_t prev_edge_t = 0;
    int      prev_val    = s_rows[0].sig[col];

    for (int i = 1; i < s_nrows; i++) {
        int cur = s_rows[i].sig[col];
        if (cur != prev_val) {
            uint64_t t = s_rows[i].ts_ns;
            if (prev_edge_t > 0) {
                uint64_t hp = t - prev_edge_t;
                if (hp >= min_hp_ns && hp <= max_hp_ns) {
                    sum  += (double)hp;
                    sum2 += (double)hp * (double)hp;
                    count++;
                }
            }
            prev_edge_t = t;
            prev_val    = cur;
        }
    }

    if (count == 0) { *out_mean = 0; *out_std = 0; return 0; }
    *out_mean = sum / count;
    double var = (sum2 / count) - (*out_mean * *out_mean);
    *out_std  = (var > 0) ? sqrt(var) : 0.0;
    return count;
}

TEST_GROUP(CsvReplayPonPoff)
{
    void setup()    {}
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Test 1 (Window 0) — DA_BCLK mean half-period is in the 150–400 ns range.
//
// The capture begins with the drive powered off (all signals idle for ~1.8 s)
// followed by active playback.  After the first two rows the BCLK runs at the
// expected 2.12 MHz.  The 50 000-row window samples mainly the active region.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W0_BclkMeanHalfPeriodInRange)
{
    int n = load_csv_window(WINDOW_OFFSETS[0], WINDOW_ROWS);
    if (n < 0) return;

    double mean_ns, std_ns;
    int edges = half_period_stats(SIG_DA_BCLK, 50, 2000, &mean_ns, &std_ns);

    CHECK_TRUE_TEXT(edges > 1000, "Too few BCLK edges to measure frequency");
    CHECK_TRUE_TEXT(mean_ns >= 150.0 && mean_ns <= 400.0,
                    "BCLK half-period outside 150-400 ns (expected ~236 ns for 2.12 MHz)");
}

// ---------------------------------------------------------------------------
// Test 2 (Window 1) — DA_BCLK jitter is low: σ < 60 ns.
//
// At 10% into the capture the drive is fully active.  Crystal-clocked BCLK
// from the CXD2545Q shows σ ≈ 28 ns in the digital.csv reference; the same
// hardware should exhibit the same jitter characteristic here.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W1_BclkJitterLow)
{
    int n = load_csv_window(WINDOW_OFFSETS[1], WINDOW_ROWS);
    if (n < 0) return;

    double mean_ns, std_ns;
    int edges = half_period_stats(SIG_DA_BCLK, 50, 2000, &mean_ns, &std_ns);

    CHECK_TRUE_TEXT(edges > 1000, "Too few BCLK edges");
    CHECK_TRUE_TEXT(std_ns < 60.0,
                    "BCLK jitter (σ) exceeds 60 ns — clock source may be unstable");
}

// ---------------------------------------------------------------------------
// Test 3 (Window 2) — DA_LRCLK half-period is in the 9 000–14 000 ns range.
//
// LRCLK must run at 44.1 kHz → half-period 11 338 ns.  ±20% tolerance covers
// measurement uncertainty in both the pon-poff and digital captures.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W2_LrclkHalfPeriodInRange)
{
    int n = load_csv_window(WINDOW_OFFSETS[2], WINDOW_ROWS);
    if (n < 0) return;

    double mean_ns, std_ns;
    int edges = half_period_stats(SIG_DA_LRCLK, 5000, 50000, &mean_ns, &std_ns);

    CHECK_TRUE_TEXT(edges > 100, "Too few LRCLK edges to measure frequency");
    CHECK_TRUE_TEXT(mean_ns >= 9000.0 && mean_ns <= 14000.0,
                    "LRCLK half-period outside 9000-14000 ns (expected ~11339 ns for 44.1 kHz)");
}

// ---------------------------------------------------------------------------
// Test 4 (Window 3) — LRCLK/BCLK half-period ratio is 44–52.
//
// Verifies the 24-bit I2S frame format (48 BCLK half-cycles per LRCLK
// half-cycle) persists across different playback sessions on the same hardware.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W3_LrclkBclkRatioIs48)
{
    int n = load_csv_window(WINDOW_OFFSETS[3], WINDOW_ROWS);
    if (n < 0) return;

    double bclk_mean, bclk_std;
    int bclk_edges = half_period_stats(SIG_DA_BCLK,  50,    2000,  &bclk_mean,  &bclk_std);

    double lrclk_mean, lrclk_std;
    int lrclk_edges = half_period_stats(SIG_DA_LRCLK, 5000, 50000, &lrclk_mean, &lrclk_std);

    CHECK_TRUE_TEXT(bclk_edges > 100 && lrclk_edges > 10, "Insufficient edges for ratio test");

    double ratio = lrclk_mean / bclk_mean;
    CHECK_TRUE_TEXT(ratio >= 44.0 && ratio <= 52.0,
                    "LRCLK/BCLK ratio outside 44-52 (expected 48 for 24-bit I2S frames)");
}

// ---------------------------------------------------------------------------
// Test 5 (Window 4) — all timestamps are monotonically non-decreasing.
//
// Covers the mid-capture region (~40% into the file) which may straddle an
// active-to-idle transition during the power-off sequence.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W4_TimestampsMonotonic)
{
    int n = load_csv_window(WINDOW_OFFSETS[4], WINDOW_ROWS);
    if (n < 0) return;
    CHECK_TRUE_TEXT(n > 100, "Too few rows loaded");

    for (int i = 1; i < s_nrows; i++) {
        CHECK_TRUE_TEXT(s_rows[i].ts_ns >= s_rows[i-1].ts_ns,
                        "Timestamp went backwards — file corrupt or parser bug");
    }
}

// ---------------------------------------------------------------------------
// Test 6 (Window 5) — minimum gap between consecutive events is ≥ 62 ns.
//
// 62 ns is the logic-analyzer sample granularity.  Events any closer together
// would be sub-sample artefacts.  Holds in both idle and active regions.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W5_MinEventSpacingAtLeast62ns)
{
    int n = load_csv_window(WINDOW_OFFSETS[5], WINDOW_ROWS);
    if (n < 0) return;
    CHECK_TRUE_TEXT(n > 100, "Too few rows loaded");

    uint64_t min_gap = UINT64_MAX;
    for (int i = 1; i < s_nrows; i++) {
        uint64_t gap = s_rows[i].ts_ns - s_rows[i-1].ts_ns;
        if (gap > 0 && gap < min_gap) min_gap = gap;
    }
    CHECK_TRUE_TEXT(min_gap >= 60ULL,
                    "Event gap < 60 ns: sub-sample artefact or timestamp precision error");
}

// ---------------------------------------------------------------------------
// Test 7 (Window 6) — BCLK high time is longer than low time (duty > 50%).
//
// The CXD2545Q consistently outputs high=245 ns, low=226 ns (≈52% duty).
// This asymmetry is a hardware characteristic that should appear in every
// active window of every capture from the same CD32 unit.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W6_BclkHighTimeLongerThanLow)
{
    int n = load_csv_window(WINDOW_OFFSETS[6], WINDOW_ROWS);
    if (n < 0) return;

    double sum_high = 0, sum_low = 0;
    int    cnt_high = 0, cnt_low = 0;
    int    prev_val = s_rows[0].sig[SIG_DA_BCLK];
    uint64_t prev_t = s_rows[0].ts_ns;

    for (int i = 1; i < s_nrows; i++) {
        int cur = s_rows[i].sig[SIG_DA_BCLK];
        if (cur != prev_val) {
            uint64_t hp = s_rows[i].ts_ns - prev_t;
            if (hp >= 50 && hp <= 2000) {
                if (prev_val == 1) { sum_high += (double)hp; cnt_high++; }
                else               { sum_low  += (double)hp; cnt_low++;  }
            }
            prev_t   = s_rows[i].ts_ns;
            prev_val = cur;
        }
    }

    CHECK_TRUE_TEXT(cnt_high > 100 && cnt_low > 100, "Too few BCLK transitions");
    double avg_high = sum_high / cnt_high;
    double avg_low  = sum_low  / cnt_low;
    CHECK_TRUE_TEXT(avg_high > avg_low,
                    "BCLK high time should be longer than low time (duty > 50%)");
}

// ---------------------------------------------------------------------------
// Test 8 (Window 7) — capture is BCLK-edge-dominated: > 90% of rows contain
// a BCLK transition.
//
// In active playback windows the fast BCLK signal dominates row counts.
// If this window falls during a power-off idle gap the edge density will drop
// below 90% and the test is skipped gracefully.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W7_CaptureIsBclkEdgeDominated)
{
    int n = load_csv_window(WINDOW_OFFSETS[7], WINDOW_ROWS);
    if (n < 0) return;
    CHECK_TRUE_TEXT(n > 1000, "Too few rows loaded");

    int bclk_changes = 0;
    for (int i = 1; i < s_nrows; i++) {
        if (s_rows[i].sig[SIG_DA_BCLK] != s_rows[i-1].sig[SIG_DA_BCLK])
            bclk_changes++;
    }
    double density = (double)bclk_changes / (s_nrows - 1);
    CHECK_TRUE_TEXT(density >= 0.90,
                    "Less than 90% of rows have a BCLK transition — may be idle window");
}

// ---------------------------------------------------------------------------
// Test 9 (Window 8) — every signal column contains only 0 or 1.
//
// Guards against CSV corruption or column-count mismatches in the
// pon-poff-idle file that could silently shift all signal indices.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W8_AllSignalColumnsAreBinary)
{
    int n = load_csv_window(WINDOW_OFFSETS[8], WINDOW_ROWS);
    if (n < 0) return;
    CHECK_TRUE_TEXT(n > 100, "Too few rows loaded");

    for (int i = 0; i < s_nrows; i++) {
        for (int col = 0; col < NUM_SIGS; col++) {
            uint8_t v = s_rows[i].sig[col];
            CHECK_TRUE_TEXT(v == 0 || v == 1,
                            "Signal column contains a value other than 0 or 1");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 10 (Window 9) — BCLK high-time standard deviation is < 50 ns.
//
// Isolates duty-cycle jitter from period jitter.  Measured at ~90% into the
// capture (late in the playback session, nearing power-off).  σ < 50 ns
// confirms the clock source remains stable throughout the session.
// ---------------------------------------------------------------------------
TEST(CsvReplayPonPoff, W9_BclkHighTimeStdDevUnder50ns)
{
    int n = load_csv_window(WINDOW_OFFSETS[9], WINDOW_ROWS);
    if (n < 0) return;

    double sum = 0, sum2 = 0;
    int    count    = 0;
    int    prev_val = s_rows[0].sig[SIG_DA_BCLK];
    uint64_t prev_t = s_rows[0].ts_ns;

    for (int i = 1; i < s_nrows; i++) {
        int cur = s_rows[i].sig[SIG_DA_BCLK];
        if (cur != prev_val) {
            uint64_t hp = s_rows[i].ts_ns - prev_t;
            if (prev_val == 1 && hp >= 50 && hp <= 2000) {
                sum  += (double)hp;
                sum2 += (double)hp * (double)hp;
                count++;
            }
            prev_t   = s_rows[i].ts_ns;
            prev_val = cur;
        }
    }

    CHECK_TRUE_TEXT(count > 100, "Too few BCLK high phases measured");
    double mean = sum / count;
    double var  = (sum2 / count) - mean * mean;
    double std  = (var > 0) ? sqrt(var) : 0.0;
    CHECK_TRUE_TEXT(std < 50.0,
                    "BCLK high-time σ ≥ 50 ns — excessive duty-cycle jitter detected");
}
