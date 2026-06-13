#!/usr/bin/env python3
"""
la_commo_direction.py — COMMO bus IF_DIR toggle and GPIO44 liveness check

Captures IF_CLK / IF_DATA / IF_DIR on three analyzer channels while the
Nebula32 is connected to a CD32 and responds to at least one command.

Gaps filled:
  - IF_DIR (GPIO 46) actually toggles on TX (OpenOCD can only set a watchpoint;
    this captures the wire state directly)
  - IF_CLK at GPIO 44 is active — proves that RP2350B pins above GPIO 31
    are accessible from PIO1 (Test 65 in openocd.md)
  - 0x27 frame-header byte visible on IF_DATA during a TX packet

WIRING (analyzer probe → Nebula32 GPIO):
  Analyzer Ch0  → GPIO 44  (IF_CLK)
  Analyzer Ch1  → GPIO 45  (IF_DATA)
  Analyzer Ch2  → GPIO 46  (IF_DIR)
  GND           → GND

Prerequisites: Nebula32 connected to CD32; power on CD32 so it sends the
initial SPINDLE_MOTOR_OFF command (0x15 0x00 0xEA).

Usage:
  python3 la_commo_direction.py /dev/ttyACM1
  python3 la_commo_direction.py --replay existing_capture.csv
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from la_utils import (build_settings, run_capture, load_csv,
                      find_edges, half_periods_ns,
                      mean, stddev, TestReport)

CH_CLK  = 0   # IF_CLK  (GPIO 44)
CH_DATA = 1   # IF_DATA (GPIO 45)
CH_DIR  = 2   # IF_DIR  (GPIO 46)

# COMMO clock rate is ~50–100 kHz; at 100 MHz that's 1000–2000 samples/cycle.
# 200k samples covers ≥ 2 full 15-byte status packets.
CAPTURE_SAMPLES = 200_000


def analyse(rows, rep):
    if not rows:
        rep.check(False, "CSV has rows")
        return

    clk_edges  = find_edges(rows, CH_CLK)
    data_edges = find_edges(rows, CH_DATA)
    dir_edges  = find_edges(rows, CH_DIR)

    # ---- GPIO 44 liveness: IF_CLK must toggle (proves pin >31 works) ----
    rep.check(len(clk_edges) >= 8,
              "IF_CLK (GPIO 44) has ≥ 8 edges — pin above GPIO 31 is active",
              f"got {len(clk_edges)}")

    # ---- IF_DATA liveness ----
    rep.check(len(data_edges) >= 2,
              "IF_DATA (GPIO 45) transitions observed",
              f"got {len(data_edges)}")

    # ---- IF_DIR actually toggles ----
    rep.check(len(dir_edges) >= 2,
              "IF_DIR (GPIO 46) transitions ≥ 2 (both RX and TX phases seen)",
              f"got {len(dir_edges)}")

    # ---- IF_DIR high during TX bursts: CLK edges while DIR=1 ----
    tx_clk = [e for e in clk_edges
               if _channel_at(rows, CH_DIR, e[0]) == 1]
    rx_clk = [e for e in clk_edges
               if _channel_at(rows, CH_DIR, e[0]) == 0]
    rep.check(len(tx_clk) >= 2,
              "CLK edges with IF_DIR=1 (TX phase edges present)",
              f"got {len(tx_clk)}")
    rep.check(len(rx_clk) >= 2,
              "CLK edges with IF_DIR=0 (RX phase edges present)",
              f"got {len(rx_clk)}")

    # ---- IF_DIR returns low after TX (not permanently stuck high) ----
    if dir_edges:
        last_dir = dir_edges[-1]
        final_dir = _channel_at(rows, CH_DIR, rows[-1][0])
        rep.check(final_dir == 0,
                  "IF_DIR returns to 0 (RX mode) after TX burst",
                  f"final IF_DIR = {final_dir}")

    # ---- Clock frequency plausible (20 kHz – 500 kHz) ----
    hp = half_periods_ns(clk_edges)
    if hp:
        m = mean(hp)
        freq_khz = 1_000_000 / (2 * m) if m > 0 else 0
        rep.check(20 <= freq_khz <= 500,
                  "IF_CLK frequency 20–500 kHz",
                  f"{freq_khz:.1f} kHz")

    # ---- 0x27 frame header byte check ----
    # The first TX packet should start with 0x27 (bit pattern 0b00100111).
    # Decode the first TX burst by finding CLK edges while DIR=1 and
    # sampling DATA on each falling CLK edge (data valid on falling).
    tx_burst_start = next((e[0] for e in dir_edges if e[1] == +1), None)
    if tx_burst_start is not None:
        bits = _sample_bits_after(rows, CH_DATA, CH_CLK, CH_DIR,
                                   tx_burst_start, max_bits=8)
        if len(bits) == 8:
            byte_val = 0
            for b in bits:
                byte_val = (byte_val << 1) | b
            rep.check(byte_val == 0x27,
                      "First TX byte = 0x27 (COMMO frame header)",
                      f"got 0x{byte_val:02X}")
        else:
            rep.check(False, "Could not decode 8 TX bits for 0x27 check",
                      f"decoded {len(bits)} bits")
    else:
        print("  [skip] No DIR rising edge found — 0x27 check skipped")


def _channel_at(rows, ch_idx, ts_ns):
    """Return the channel value at the sample closest to ts_ns."""
    best = rows[0][1][ch_idx]
    for ts, ch in rows:
        if ts <= ts_ns:
            best = ch[ch_idx]
        else:
            break
    return best


def _sample_bits_after(rows, data_ch, clk_ch, dir_ch, start_ts, max_bits=8):
    """Sample DATA on each falling CLK edge while DIR=1, from start_ts."""
    bits = []
    prev_clk = None
    for ts, ch in rows:
        if ts < start_ts:
            continue
        if ch[dir_ch] != 1:
            if bits:
                break
            continue
        clk = ch[clk_ch]
        if prev_clk == 1 and clk == 0:   # falling edge
            bits.append(ch[data_ch])
            if len(bits) >= max_bits:
                break
        prev_clk = clk
    return bits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("serial", nargs="?",
                    help="Logic analyzer serial port (e.g. /dev/ttyACM1)")
    ap.add_argument("--replay", metavar="CSV",
                    help="Skip capture; analyse an existing CSV file")
    args = ap.parse_args()

    if not args.replay and not args.serial:
        ap.error("Provide a serial port or --replay <csv>")

    rep = TestReport("la_commo_direction")
    print("=" * 60)
    print("COMMO bus direction and GPIO 44 liveness — Logic Analyzer")
    print("=" * 60)

    if args.replay:
        csv_path = args.replay
        print(f"[replay] {csv_path}")
    else:
        settings = build_settings(
            channels=["IF_CLK", "IF_DATA", "IF_DIR"],
            total_samples=CAPTURE_SAMPLES,
            pre_trigger=500,
            trigger_ch=CH_CLK,
            trigger_falling=True,   # falling CLK edge = start of packet
        )
        fd, csv_path = tempfile.mkstemp(suffix=".csv", prefix="la_commo_")
        os.close(fd)
        print(f"[capture] {args.serial} → {csv_path}")
        run_capture(args.serial, settings, csv_path)

    rows = load_csv(csv_path)
    print(f"[info] {len(rows)} rows loaded")
    if not rows:
        print("[FAIL] Empty CSV — check wiring; ensure CD32 is powered on")
        sys.exit(1)

    analyse(rows, rep)
    ok = rep.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
