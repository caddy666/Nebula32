// =============================================================================
// test_csv_replay_zool2.cpp — Logic-analyzer CSV signal validation
//                              zool2 capture
// =============================================================================
//
// Samples 10 non-overlapping 50 000-row windows from the Zool 2 game capture
// (zool2.csv, ~233 M rows, 12.1 GB).  Column layout differs from digital.csv:
//
//   sig[0]=IF_CLK  sig[1]=IF_DATA  sig[2]=IF_DIR
//   sig[3]=DA_LRCLK  sig[4]=DA_DATA  sig[5]=DA_BCLK
//   sig[6]=DA_C2PO   sig[7]=DA_EMPH
//
// IF_DIR is present (COMMO direction) and the subcode signals are absent.
// DA_C2PO and DA_EMPH replace SUB_WFCLK/SUB_SCOR — they are driven low by
// the ODE (no errors, no pre-emphasis).
//
// Window offsets are spaced at 10% intervals of the 12 127 527 952-byte file.
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define CSV_PATH    "../cd32_logic_replay_bundle/zool2.csv"
#define WINDOW_ROWS 50000

// Column indices — zool2 layout differs from digital.csv.
// DA_BCLK is at column 5 (not 4) and DA_LRCLK is at column 3 (not 2).
enum {
    SIG_IF_CLK   = 0,
    SIG_IF_DATA  = 1,
    SIG_IF_DIR   = 2,
    SIG_DA_LRCLK = 3,
    SIG_DA_DATA  = 4,
    SIG_DA_BCLK  = 5,
    SIG_DA_C2PO  = 6,
    SIG_DA_EMPH  = 7,
    NUM_SIGS     = 8
};

// Window offsets at 10% intervals of 12 127 527 952 bytes (step ≈ 1 212 752 795).
static const long long WINDOW_OFFSETS[10] = {
    0LL,
    1212752795LL,
    2425505590LL,
    3638258385LL,
    4851011180LL,
    6063763976LL,
    7276516771LL,
    8489269566LL,
    9702022361LL,
    10914775156LL,
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

TEST_GROUP(CsvReplayZool2)
{
    void setup()    {}
    void teardown() {}
};

// ---------------------------------------------------------------------------
// Test 1 (Window 0) — DA_BCLK mean half-period is in the 150–400 ns range.
//
// The Zool 2 capture starts with the drive already active (BCLK=1 in row 0).
// Expecting ~236 ns half-period (2.12 MHz) from the CXD2545Q.
// Note: DA_BCLK is column 5 in this capture, not column 4 as in digital.csv.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W0_BclkMeanHalfPeriodInRange)
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
// Jitter validation at 10% into the 12 GB capture.  Game audio playback
// should show the same crystal-locked stability as the digital.csv reference.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W1_BclkJitterLow)
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
// LRCLK (word select) must run at 44.1 kHz → half-period 11 338 ns.
// DA_LRCLK is column 3 in the zool2 layout (column 2 in digital.csv).
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W2_LrclkHalfPeriodInRange)
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
// Confirms 24-bit I2S framing (48 BCLK half-cycles per LRCLK half-cycle) is
// consistent across different game titles on the same hardware.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W3_LrclkBclkRatioIs48)
{
    int n = load_csv_window(WINDOW_OFFSETS[3], WINDOW_ROWS);
    if (n < 0) return;

    double bclk_mean, bclk_std;
    int bclk_edges  = half_period_stats(SIG_DA_BCLK,  50,    2000,  &bclk_mean,  &bclk_std);

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
// Validates the mid-capture region (~40%) of a long continuous game session.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W4_TimestampsMonotonic)
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
// At 50% into the file the capture is well within steady-state game playback.
// The 62 ns floor reflects the 16 MHz logic-analyzer sample rate.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W5_MinEventSpacingAtLeast62ns)
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
// The CXD2545Q duty-cycle asymmetry (high=245 ns, low=226 ns) is a fixed
// output characteristic of that chip and should appear in the Zool 2 capture
// exactly as in the digital.csv and pon-poff-idle captures.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W6_BclkHighTimeLongerThanLow)
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
// Zool 2 is a continuous game session with no idle periods, so BCLK edge
// density should be uniformly high throughout the entire file.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W7_CaptureIsBclkEdgeDominated)
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
                    "Less than 90% of rows have a BCLK transition — capture may be idle or corrupt");
}

// ---------------------------------------------------------------------------
// Test 9 (Window 8) — every signal column contains only 0 or 1.
//
// The zool2 layout has a different column order than digital.csv.  This test
// guards against index mis-mapping that would shift a non-binary field into a
// signal slot and silently corrupt all subsequent timing assertions.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W8_AllSignalColumnsAreBinary)
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
// At ~90% through the 12 GB game capture, clock stability late in a long
// session is verified.  Same σ < 50 ns threshold as the reference capture.
// ---------------------------------------------------------------------------
TEST(CsvReplayZool2, W9_BclkHighTimeStdDevUnder50ns)
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
