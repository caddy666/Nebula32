#!/usr/bin/env python3
"""
la_da_timing.py — Live DA I²S signal verification via Logic Analyzer

Captures DA_BCLK / DA_LRCLK / DA_DATA on three analyzer channels and asserts
the same timing thresholds as the CsvReplay host tests (W0-W9 in
tests/host/test_csv_replay_*.cpp).  Run while Nebula32 is playing a disc.

WIRING (analyzer probe → Nebula32 GPIO):
  Analyzer Ch0  → GPIO 1  (DA_BCLK)
  Analyzer Ch1  → GPIO 2  (DA_LRCLK)
  Analyzer Ch2  → GPIO 0  (DA_DATA)
  GND           → GND

Usage:
  python3 la_da_timing.py /dev/ttyACM1
  python3 la_da_timing.py --replay existing_capture.csv   (offline analysis)
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from la_utils import (build_settings, run_capture, load_csv,
                      find_edges, half_periods_ns, high_low_times,
                      mean, stddev, TestReport)

# Channel indices in the capture
CH_BCLK  = 0
CH_LRCLK = 1
CH_DATA  = 2

# Thresholds — match CsvReplay W0-W9 assertions
BCLK_HP_MIN_NS   = 150      # W0: BCLK half-period lower bound
BCLK_HP_MAX_NS   = 400      # W0: BCLK half-period upper bound
BCLK_JITTER_NS   = 60       # W1: max σ of half-period
LRCLK_HP_MIN_NS  = 9_000    # W2
LRCLK_HP_MAX_NS  = 14_000   # W2
RATIO_MIN        = 44       # W3: BCLK edges per LRCLK half-period
RATIO_MAX        = 52       # W3
MIN_GAP_NS       = 62       # W5: minimum event spacing (16 MHz analyser floor)
BCLK_DOMINATED   = 0.90     # W7: >90% of edges must be BCLK


def analyse(rows, rep):
    if not rows:
        rep.check(False, "CSV has rows")
        return

    total_edges = 0

    # ---- BCLK ----
    bclk_edges = find_edges(rows, CH_BCLK)
    hp_b = half_periods_ns(bclk_edges)
    hi_b, lo_b = high_low_times(bclk_edges)

    rep.check(len(bclk_edges) >= 200, "BCLK edge count ≥ 200",
              f"got {len(bclk_edges)}")

    if hp_b:
        m = mean(hp_b)
        s = stddev(hp_b)
        rep.check(BCLK_HP_MIN_NS <= m <= BCLK_HP_MAX_NS,
                  f"BCLK half-period mean {BCLK_HP_MIN_NS}–{BCLK_HP_MAX_NS} ns",
                  f"{m:.1f} ns")
        rep.check(s < BCLK_JITTER_NS,
                  f"BCLK half-period σ < {BCLK_JITTER_NS} ns", f"{s:.1f} ns")
    if hi_b and lo_b:
        rep.check(mean(hi_b) > mean(lo_b),
                  "BCLK high time > low time (duty cycle > 50%)",
                  f"high={mean(hi_b):.1f} ns  low={mean(lo_b):.1f} ns")
        rep.check(stddev(hi_b) < 50,
                  "BCLK high-time σ < 50 ns", f"{stddev(hi_b):.1f} ns")

    total_edges += len(bclk_edges)

    # ---- LRCLK ----
    lrclk_edges = find_edges(rows, CH_LRCLK)
    hp_l = half_periods_ns(lrclk_edges)

    rep.check(len(lrclk_edges) >= 10, "LRCLK edge count ≥ 10",
              f"got {len(lrclk_edges)}")

    if hp_l:
        m = mean(hp_l)
        rep.check(LRCLK_HP_MIN_NS <= m <= LRCLK_HP_MAX_NS,
                  f"LRCLK half-period mean {LRCLK_HP_MIN_NS}–{LRCLK_HP_MAX_NS} ns",
                  f"{m:.1f} ns")

    total_edges += len(lrclk_edges)

    # ---- BCLK/LRCLK ratio (48-cycle I²S frames) ----
    if lrclk_edges and bclk_edges:
        lrclk_times = [e[0] for e in lrclk_edges]
        # Count BCLK edges in each LRCLK half-period
        ratios = []
        for i in range(len(lrclk_times) - 1):
            t0, t1 = lrclk_times[i], lrclk_times[i + 1]
            count = sum(1 for e in bclk_edges if t0 <= e[0] < t1)
            if count > 0:
                ratios.append(count)
        if ratios:
            r = mean(ratios)
            rep.check(RATIO_MIN <= r <= RATIO_MAX,
                      f"BCLK/LRCLK half-period ratio {RATIO_MIN}–{RATIO_MAX} (48-cycle I²S)",
                      f"{r:.1f}")

    # ---- Minimum event gap ----
    all_edges = sorted(bclk_edges + lrclk_edges + find_edges(rows, CH_DATA),
                       key=lambda e: e[0])
    if len(all_edges) >= 2:
        gaps = [all_edges[i+1][0] - all_edges[i][0]
                for i in range(len(all_edges) - 1) if all_edges[i+1][0] > all_edges[i][0]]
        if gaps:
            rep.check(min(gaps) >= MIN_GAP_NS,
                      f"Min event gap ≥ {MIN_GAP_NS} ns (analyser floor)",
                      f"{min(gaps)} ns")

    # ---- Signal column binary check ----
    bad = sum(1 for _, ch in rows
              for v in ch if v not in (0, 1))
    rep.check(bad == 0, "All channel values are binary {0,1}", f"{bad} non-binary")

    # ---- BCLK dominates transitions ----
    data_edges = find_edges(rows, CH_DATA)
    total_edges += len(data_edges)
    if total_edges > 0:
        frac = len(bclk_edges) / total_edges
        rep.check(frac >= BCLK_DOMINATED,
                  f"BCLK transitions > {int(BCLK_DOMINATED*100)}% of all edges",
                  f"{frac*100:.1f}%")

    # ---- Timestamp monotonicity ----
    ts = [r[0] for r in rows]
    non_mono = sum(1 for i in range(len(ts)-1) if ts[i+1] < ts[i])
    rep.check(non_mono == 0, "Timestamps monotonically non-decreasing",
              f"{non_mono} violations")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("serial", nargs="?",
                    help="Logic analyzer serial port (e.g. /dev/ttyACM1)")
    ap.add_argument("--replay", metavar="CSV",
                    help="Skip capture; analyse an existing CSV file")
    ap.add_argument("--samples", type=int, default=500_000,
                    help="Number of samples to capture (default 500000 = 5 ms)")
    args = ap.parse_args()

    if not args.replay and not args.serial:
        ap.error("Provide a serial port or --replay <csv>")

    rep = TestReport("la_da_timing")
    print("=" * 60)
    print("DA I²S timing verification — Logic Analyzer")
    print("=" * 60)

    if args.replay:
        csv_path = args.replay
        print(f"[replay] {csv_path}")
    else:
        settings = build_settings(
            channels=["DA_BCLK", "DA_LRCLK", "DA_DATA"],
            total_samples=args.samples,
            pre_trigger=1000,
            trigger_ch=CH_BCLK,
            trigger_falling=False,   # rising BCLK edge
        )
        fd, csv_path = tempfile.mkstemp(suffix=".csv", prefix="la_da_")
        os.close(fd)
        print(f"[capture] {args.serial} → {csv_path}")
        run_capture(args.serial, settings, csv_path)

    rows = load_csv(csv_path)
    print(f"[info] {len(rows)} rows loaded")
    if not rows:
        print("[FAIL] Empty CSV — check wiring and ensure disc is playing")
        sys.exit(1)

    analyse(rows, rep)
    ok = rep.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
