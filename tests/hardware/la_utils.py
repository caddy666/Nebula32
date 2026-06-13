"""
Shared utilities for Logic Analyzer hardware tests.

CSV format from TerminalCapture (LogicAnalyzer project):
  Time,Channel1,Channel2,...
  0.000000000,0,0,0
  1e-08,1,0,0          (10 ns step at 100 MHz)

or the datetime-stamped format used in existing captures:
  01/01/2024 00:00:00.000000010,0,0,0
"""

import json
import math
import os
import subprocess
import sys
import tempfile

LA_BIN = "/home/caddy/Desktop/pi-logic/TerminalCapture"
SAMPLE_RATE_HZ = 100_000_000
NS_PER_SAMPLE = 1_000_000_000 // SAMPLE_RATE_HZ  # 10 ns


# ---------------------------------------------------------------------------
# Capture settings builder
# ---------------------------------------------------------------------------

def build_settings(channels, total_samples=500_000, pre_trigger=1000,
                   trigger_ch=0, trigger_falling=False):
    """Return a TerminalCapture settings dict ready for json.dump()."""
    ch_list = []
    for i, name in enumerate(channels):
        ch_list.append({
            "TextualChannelNumber": f"Channel {i + 1}",
            "ChannelNumber": i,
            "ChannelName": name,
            "ChannelColor": None,
            "Hidden": False,
            "Samples": None,
        })
    return {
        "Frequency": SAMPLE_RATE_HZ,
        "PreTriggerSamples": pre_trigger,
        "PostTriggerSamples": total_samples - pre_trigger,
        "TotalSamples": total_samples,
        "LoopCount": 0,
        "MeasureBursts": False,
        "CaptureChannels": ch_list,
        "Bursts": None,
        "TriggerType": 0,        # 0 = edge
        "TriggerChannel": trigger_ch,
        "TriggerInverted": trigger_falling,
        "TriggerBitCount": 0,
        "TriggerPattern": 0,
    }


def run_capture(serial_port, settings, out_csv):
    """Write settings file, invoke TerminalCapture, return path to CSV."""
    tcs = out_csv.replace(".csv", ".tcs")
    with open(tcs, "w") as f:
        json.dump(settings, f)
    result = subprocess.run(
        [LA_BIN, "capture", serial_port, tcs, out_csv],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"[FAIL] TerminalCapture error:\n{result.stderr}")
        sys.exit(1)
    return out_csv


# ---------------------------------------------------------------------------
# CSV parser
# ---------------------------------------------------------------------------

def _parse_ts_ns(ts_str):
    """Parse timestamp to nanoseconds.  Handles float-seconds and
    'DD/MM/YYYY HH:MM:SS.nnnnnnnnn' formats."""
    ts_str = ts_str.strip()
    if ts_str[2:3] == "/" or ts_str[2:3] == "-":
        # datetime format: DD/MM/YYYY HH:MM:SS.nnnnnnnnn
        t = ts_str[11:]  # "HH:MM:SS.nnnnnnnnn"
        hh = int(t[0:2])
        mm = int(t[3:5])
        ss = int(t[6:8])
        ns_str = t[9:18].ljust(9, "0")
        ns = int(ns_str)
        return (hh * 3600 + mm * 60 + ss) * 1_000_000_000 + ns
    else:
        # float or scientific notation in seconds
        return int(float(ts_str) * 1_000_000_000)


def load_csv(path):
    """Return list of (ts_ns, [ch0, ch1, ...]) from a capture CSV.
    Timestamps are relative (first sample = 0)."""
    rows = []
    t0 = None
    with open(path, newline="") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("T") or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) < 2:
                continue
            try:
                ts = _parse_ts_ns(parts[0])
            except (ValueError, IndexError):
                continue
            if t0 is None:
                t0 = ts
            ch = [int(x.strip()) for x in parts[1:]]
            rows.append((ts - t0, ch))
    return rows


# ---------------------------------------------------------------------------
# Edge detection
# ---------------------------------------------------------------------------

def find_edges(rows, ch_idx):
    """Return list of (ts_ns, direction) where direction=+1 (rising) or -1 (falling)."""
    edges = []
    prev = None
    for ts, ch in rows:
        v = ch[ch_idx] if ch_idx < len(ch) else 0
        if prev is not None and v != prev:
            edges.append((ts, +1 if v == 1 else -1))
        prev = v
    return edges


def half_periods_ns(edges):
    """Return list of half-period durations (ns) between consecutive edges."""
    times = [e[0] for e in edges]
    return [times[i + 1] - times[i] for i in range(len(times) - 1)]


def high_low_times(edges):
    """Return (high_times_ns, low_times_ns) from alternating edges."""
    highs, lows = [], []
    for i in range(len(edges) - 1):
        ts0, d0 = edges[i]
        ts1, _  = edges[i + 1]
        dur = ts1 - ts0
        if d0 == -1:    # falling → was high
            highs.append(dur)
        else:           # rising → was low
            lows.append(dur)
    return highs, lows


# ---------------------------------------------------------------------------
# Statistics helpers
# ---------------------------------------------------------------------------

def mean(vals):
    return sum(vals) / len(vals) if vals else 0.0


def stddev(vals):
    if len(vals) < 2:
        return 0.0
    m = mean(vals)
    return math.sqrt(sum((v - m) ** 2 for v in vals) / (len(vals) - 1))


# ---------------------------------------------------------------------------
# PASS/FAIL reporter
# ---------------------------------------------------------------------------

class TestReport:
    def __init__(self, name):
        self.name = name
        self._pass = 0
        self._fail = 0

    def check(self, condition, label, detail=""):
        tag = "[PASS]" if condition else "[FAIL]"
        print(f"  {tag} {label}" + (f"  ({detail})" if detail else ""))
        if condition:
            self._pass += 1
        else:
            self._fail += 1

    def summary(self):
        total = self._pass + self._fail
        print(f"\n{self.name}: {self._pass}/{total} passed")
        return self._fail == 0
