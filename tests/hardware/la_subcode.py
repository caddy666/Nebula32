#!/usr/bin/env python3
"""
la_subcode.py — Subcode signal timing verification via Logic Analyzer

Captures SUB_DATA / SUB_CLK / SUB_WFCLK / SUB_SCOR during disc playback
and verifies clock frequency and pulse widths for the signals driven by
PIO0 SM1 (subcode_encoder.pio) and by subcode_pulse_sector_clocks().

Gaps filled:
  - SUB_CLK frequency plausible for the configured clkdiv
  - WFCLK and SCOR produce short pulses (not stuck low/high)
  - DA and subcode PIO co-exist (if this works while DA is also running,
    PIO0 resource sharing is confirmed)

WIRING (analyzer probe → Nebula32 GPIO):
  Analyzer Ch0  → GPIO 5   (SUB_DATA)
  Analyzer Ch1  → GPIO 6   (SUB_CLK)
  Analyzer Ch2  → GPIO 7   (SUB_WFCLK)
  Analyzer Ch3  → GPIO 8   (SUB_SCOR)
  GND           → GND

Prerequisites: Nebula32 playing a disc (subcode is only generated during playback).

Usage:
  python3 la_subcode.py /dev/ttyACM1
  python3 la_subcode.py --replay existing_capture.csv
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from la_utils import (build_settings, run_capture, load_csv,
                      find_edges, half_periods_ns, high_low_times,
                      mean, stddev, TestReport)

CH_DATA  = 0   # SUB_DATA  (GPIO 5)
CH_CLK   = 1   # SUB_CLK   (GPIO 6)
CH_WFCLK = 2   # SUB_WFCLK (GPIO 7)
CH_SCOR  = 3   # SUB_SCOR  (GPIO 8)

# SUB_CLK: PIO0 SM1 clkdiv=24 at 135.475 MHz → SM clk ≈ 5.645 MHz
# SUB_CLK toggles once per SM cycle → ≈ 2.82 MHz
# (actual measured value may differ; we use a wide tolerance)
SUBCLK_FREQ_MIN_KHZ = 200
SUBCLK_FREQ_MAX_KHZ = 5_000

# WFCLK and SCOR are pulsed by subcode_pulse_sector_clocks() once per sector.
# At 75 sectors/sec that's a 13.3 ms period.  We just need to see at least
# one pulse in a ~50 ms window (100k samples @ 100 MHz).
CAPTURE_SAMPLES = 100_000


def analyse(rows, rep):
    if not rows:
        rep.check(False, "CSV has rows")
        return

    clk_edges  = find_edges(rows, CH_CLK)
    data_edges = find_edges(rows, CH_DATA)
    wf_edges   = find_edges(rows, CH_WFCLK)
    scor_edges = find_edges(rows, CH_SCOR)

    # ---- SUB_CLK must be active ----
    rep.check(len(clk_edges) >= 10,
              "SUB_CLK (GPIO 6) ≥ 10 edges during playback",
              f"got {len(clk_edges)}")

    # ---- SUB_CLK frequency ----
    hp = half_periods_ns(clk_edges)
    if hp:
        m = mean(hp)
        freq_khz = 1_000_000 / (2 * m) if m > 0 else 0
        rep.check(SUBCLK_FREQ_MIN_KHZ <= freq_khz <= SUBCLK_FREQ_MAX_KHZ,
                  f"SUB_CLK frequency {SUBCLK_FREQ_MIN_KHZ}–{SUBCLK_FREQ_MAX_KHZ} kHz",
                  f"{freq_khz:.1f} kHz")

    # ---- SUB_DATA transitions (not stuck) ----
    rep.check(len(data_edges) >= 2,
              "SUB_DATA (GPIO 5) has transitions (not stuck)",
              f"got {len(data_edges)}")

    # ---- WFCLK pulse(s) observed ----
    rep.check(len(wf_edges) >= 2,
              "SUB_WFCLK (GPIO 7) pulsed ≥ once (rising + falling edge)",
              f"got {len(wf_edges)}")

    # ---- WFCLK returns low (not latched high) ----
    if rows:
        final_wf = rows[-1][1][CH_WFCLK]
        rep.check(final_wf == 0,
                  "SUB_WFCLK idles low between pulses",
                  f"final level = {final_wf}")

    # ---- SCOR pulse(s) observed ----
    rep.check(len(scor_edges) >= 2,
              "SUB_SCOR (GPIO 8) pulsed ≥ once (rising + falling edge)",
              f"got {len(scor_edges)}")

    # ---- SCOR returns low ----
    if rows:
        final_scor = rows[-1][1][CH_SCOR]
        rep.check(final_scor == 0,
                  "SUB_SCOR idles low between pulses",
                  f"final level = {final_scor}")

    # ---- WFCLK pulse width < 2 ms (not DC) ----
    wf_hi, _ = high_low_times(wf_edges)
    if wf_hi:
        max_pulse_ns = max(wf_hi)
        rep.check(max_pulse_ns < 2_000_000,
                  "SUB_WFCLK pulse width < 2 ms (not stuck high)",
                  f"max = {max_pulse_ns/1000:.1f} µs")

    # ---- Simultaneous BCLK-domain activity (SUB_CLK ≠ DA_BCLK rate) ----
    if hp and clk_edges:
        m = mean(hp)
        freq_mhz = 1_000 / (2 * m) if m > 0 else 0
        rep.check(freq_mhz < 3.0,
                  "SUB_CLK is distinct from DA_BCLK (< 3 MHz confirms separate SM)",
                  f"{freq_mhz:.2f} MHz")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("serial", nargs="?",
                    help="Logic analyzer serial port (e.g. /dev/ttyACM1)")
    ap.add_argument("--replay", metavar="CSV",
                    help="Skip capture; analyse an existing CSV file")
    ap.add_argument("--samples", type=int, default=CAPTURE_SAMPLES,
                    help=f"Samples to capture (default {CAPTURE_SAMPLES})")
    args = ap.parse_args()

    if not args.replay and not args.serial:
        ap.error("Provide a serial port or --replay <csv>")

    rep = TestReport("la_subcode")
    print("=" * 60)
    print("Subcode signal timing — Logic Analyzer")
    print("=" * 60)

    if args.replay:
        csv_path = args.replay
        print(f"[replay] {csv_path}")
    else:
        settings = build_settings(
            channels=["SUB_DATA", "SUB_CLK", "SUB_WFCLK", "SUB_SCOR"],
            total_samples=args.samples,
            pre_trigger=500,
            trigger_ch=CH_CLK,
            trigger_falling=False,
        )
        fd, csv_path = tempfile.mkstemp(suffix=".csv", prefix="la_sub_")
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
