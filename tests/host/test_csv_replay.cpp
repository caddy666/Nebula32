// =============================================================================
// test_csv_replay.cpp — Logic-analyzer CSV signal validation
// =============================================================================
//
// Samples 10 non-overlapping 50 000-row windows from the full logic-analyzer
// capture (digital.csv, ~100 M rows, 4.9 GB).  Each window is ~1/40 of
// sample_digital.csv's 2 M-row span — large enough to accumulate thousands of
// BCLK and LRCLK half-periods for statistically robust assertions.
//
// CSV column order (after timestamp):
//   sig[0]=IF_CLK  sig[1]=IF_DATA  sig[2]=DA_LRCLK  sig[3]=DA_DATA
//   sig[4]=DA_BCLK  sig[5]=SUB_WFCLK  sig[6]=SUB_SCOR  sig[7]=SUB_DATA
//
// Observed values from real CD32 hardware (windows 0-9 all consistent):
//   DA_BCLK half-period : mean=236 ns, σ=28 ns, high=245 ns, low=226 ns
//   DA_LRCLK half-period: mean=11 339 ns  → 44.1 kHz word clock
//   LRCLK/BCLK ratio   : 48 BCLK half-cycles per LRCLK half-cycle
//   Min event gap       : 62 ns (logic-analyzer sample granularity)
//   Capture density     : >99% of rows are BCLK transitions
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Path to the large capture, relative to the tests/host/ working directory.
#define CSV_PATH "../cd32_logic_replay_bundle/digital.csv"

// Number of rows to load per window.  50 000 rows ≈ 2.75 MB of I/O each.
#define WINDOW_ROWS 50000

// Signal indices matching CSV column order.
enum {
    SIG_IF_CLK   = 0,
    SIG_IF_DATA  = 1,
    SIG_DA_LRCLK = 2,
    SIG_DA_DATA  = 3,
    SIG_DA_BCLK  = 4,
    SIG_SUB_WFCLK = 5,
    SIG_SUB_SCOR  = 6,
    SIG_SUB_DATA  = 7,
    NUM_SIGS      = 8
};

// Byte offsets into digital.csv for each window (~10% increments of the file).
// Computed as: window_i starts at i * 10 000 000 rows × 52 bytes/row.
static const long long WINDOW_OFFSETS[10] = {
    0LL,           // Window 0 — first 50 k rows
    520000002LL,   // Window 1 — rows ~10 M
    1040000005LL,  // Window 2 — rows ~20 M
    1560000007LL,  // Window 3 — rows ~30 M
    2080000010LL,  // Window 4 — rows ~40 M
    2600000012LL,  // Window 5 — rows ~50 M
    3120000015LL,  // Window 6 — rows ~60 M
    3640000017LL,  // Window 7 — rows ~70 M
    4160000020LL,  // Window 8 — rows ~80 M
    4680000022LL,  // Window 9 — rows ~90 M
};

// ---------------------------------------------------------------------------
// Parsed row storage (static so it doesn't live on the test-runner stack).
// ---------------------------------------------------------------------------
struct CsvRow {
    uint64_t ts_ns;      // nanoseconds since first row in window
    uint8_t  sig[NUM_SIGS];
};

static CsvRow s_rows[WINDOW_ROWS];
static int    s_nrows = 0;

// ---------------------------------------------------------------------------
// parse_ts_ns — convert ISO8601 timestamp to nanoseconds (absolute)
//
// Input format: "2023-04-24T03:48:54.801044500+00:00"
//                          ^10 = 'T'
//                           ^11 hh  ^14 mm  ^17 ss  ^19 '.'  ^20..28 9-digit ns
// ---------------------------------------------------------------------------
static uint64_t parse_ts_ns(const char *ts)
{
    // 'T' is always at position 10 in the ISO8601 date+time string.
    const char *t = ts + 11;                    // points to first digit of HH
    uint64_t hh = (uint64_t)(t[0]-'0')*10 + (t[1]-'0');
    uint64_t mm = (uint64_t)(t[3]-'0')*10 + (t[4]-'0');
    uint64_t ss = (uint64_t)(t[6]-'0')*10 + (t[7]-'0');
    // t[8] == '.'
    uint64_t ns = 0;
    for (int i = 0; i < 9; i++)
        ns = ns * 10 + (uint64_t)(t[9 + i] - '0');
    return (hh * 3600ULL + mm * 60ULL + ss) * 1000000000ULL + ns;
}

// ---------------------------------------------------------------------------
// load_csv_window — seek to byte_offset, skip the partial row if any,
// then read up to max_rows complete data rows into s_rows[].
// Returns the number of rows read, or -1 if the file cannot be opened.
// ---------------------------------------------------------------------------
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
    // Skip the partial line that begins at a mid-file byte offset.
    if (byte_offset > 0) {
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    }

    s_nrows = 0;
    uint64_t t0 = 0;

    while (s_nrows < max_rows && fgets(line, sizeof(line), f)) {
        // Skip header or blank lines.
        if (line[0] == 'T' || line[0] == '\n' || line[0] == '\r') continue;

        // Find the comma that ends the timestamp field.
        char *p = line;
        while (*p && *p != ',') p++;
        if (*p != ',') continue;
        *p++ = '\0';

        uint64_t ts_abs = parse_ts_ns(line);
        if (t0 == 0) t0 = ts_abs;
        s_rows[s_nrows].ts_ns = ts_abs - t0;

        // Parse 8 signal columns (each is '0' or '1' followed by ',' or '\n').
        for (int i = 0; i < NUM_SIGS; i++) {
            s_rows[s_nrows].sig[i] = (*p == '1') ? 1u : 0u;
            p += 2;   // digit + separator
        }
        s_nrows++;
    }

    fclose(f);
    return s_nrows;
}

// ---------------------------------------------------------------------------
// half_period_stats — scan s_rows for edges on signal column 'col',
// accumulate half-period durations in [min_hp_ns, max_hp_ns], and write
// mean / stddev into *out_mean / *out_std.  Returns edge count.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// TEST GROUP
// ---------------------------------------------------------------------------

TEST_GROUP(CsvReplay)
{
    void setup()   {}
    void teardown(){}
};

// ---------------------------------------------------------------------------
// Test 1 (Window 0) — DA_BCLK mean half-period is in the 150–400 ns range.
//
// At 1× CD speed the real CXD2545Q outputs BCLK at ~2.12 MHz.  The logic
// analyzer measured 236 ns half-periods.  We allow ±40% to tolerate minor
// speed variations but exclude impossible values (>400 ns would be < 1.25 MHz,
// clearly wrong; <150 ns would be > 3.3 MHz, above 2× speed).
// ---------------------------------------------------------------------------
TEST(CsvReplay, W0_BclkMeanHalfPeriodInRange)
{
    int n = load_csv_window(WINDOW_OFFSETS[0], WINDOW_ROWS);
    if (n < 0) return;  // CSV not present — skip gracefully

    double mean_ns, std_ns;
    int edges = half_period_stats(SIG_DA_BCLK, 50, 2000, &mean_ns, &std_ns);

    CHECK_TRUE_TEXT(edges > 1000, "Too few BCLK edges to measure frequency");
    CHECK_TRUE_TEXT(mean_ns >= 150.0 && mean_ns <= 400.0,
                    "BCLK half-period outside 150-400 ns (expected ~236 ns for 2.12 MHz)");
}

// ---------------------------------------------------------------------------
// Test 2 (Window 1) — DA_BCLK jitter is low: σ < 60 ns (< 25% of mean).
//
// A well-clocked PIO/crystal system should have very low jitter.  The capture
// shows σ ≈ 28 ns which is partly logic-analyzer quantisation noise (62 ns
// granularity); true jitter is lower.  We use a relaxed 60 ns threshold.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W1_BclkJitterLow)
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
// LRCLK (word select) must run at exactly 44.1 kHz → period 22.68 µs →
// half-period 11 338 ns.  We allow ±20% for measurement uncertainty.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W2_LrclkHalfPeriodInRange)
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
// The ratio measures how many BCLK half-cycles span one LRCLK half-cycle.
// On real hardware: 11 339 ns / 236 ns ≈ 48.  This confirms the CXD2545Q
// uses 24-bit I2S frames (24 BCLK cycles per channel × 2 channels = 48 per
// LRCLK half-period), not the 16-bit frames assumed in the original firmware.
// The ODE's da_output.pio must match this or Akiko will lose framing sync.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W3_LrclkBclkRatioIs48)
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
// A violated monotonicity guarantee would indicate file corruption or a
// parser bug — either would silently corrupt every timing assertion.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W4_TimestampsMonotonic)
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
// The logic analyzer samples every 62.5 ns (16 MHz sample rate inferred from
// the observed minimum gap).  Any two events closer than 62 ns would be a
// sub-sample artefact, not a real signal edge.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W5_MinEventSpacingAtLeast62ns)
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
// The captured hardware consistently shows high=245 ns, low=226 ns (duty
// ≈ 52%).  This asymmetry is characteristic of the CXD2545Q's output stage
// and must be reproduced by the ODE's PIO to avoid phase drift in Akiko's
// bit-clock recovery.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W6_BclkHighTimeLongerThanLow)
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
// Because BCLK is the fastest signal (one edge every 236 ns) and the
// analyzer records every edge, BCLK transitions should account for almost
// every row in the CSV.  Measured: ~99.5% in each window.  This ensures the
// raw CSV actually contains a live signal, not a mostly-idle bus.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W7_CaptureIsBclkEdgeDominated)
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
// Guards against CSV corruption, encoding errors (e.g., UTF-16 BOM, CRLF
// anomalies) or an incorrect column count that would shift fields and
// introduce invalid values.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W8_AllSignalColumnsAreBinary)
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
// The high-time σ isolates one half of the duty cycle from the other.
// Measuring σ < 50 ns separately on the high phase (rather than the
// combined half-period) is more sensitive to drive-side timing anomalies
// such as a jittered pull-up vs. an accurate pull-down.  Observed: σ ≈ 28 ns.
// ---------------------------------------------------------------------------
TEST(CsvReplay, W9_BclkHighTimeStdDevUnder50ns)
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
