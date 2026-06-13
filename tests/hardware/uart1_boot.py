#!/usr/bin/env python3
"""
uart1_boot.py — Boot sequence verification over UART1 (GPIO 20 TX)

Connects to Nebula32 UART1 (GPIO 20) and checks that the firmware reaches
IDLE state, WiFi initialises successfully, and (when connected to a CD32)
the first COMMO opcode from the host is received and logged.

Gaps filled:
  - Boot reaches IDLE drive state (no unit test covers main.c boot sequence)
  - WiFi CYW43439 init return code logged (GPIO 23/24/25/29 RM2 module)
  - First COMMO opcode byte from a real CD32 (requires physical CD32)

WIRING:
  Pi GPIO 15 (RXD1)  → Nebula32 GPIO 20 (UART1_TX)
  Pi GPIO 14 (TXD1)  → Nebula32 GPIO 21 (UART1_RX)  [optional]
  GND                → GND

  Or use a USB-UART adapter:
  FTDI RX → GPIO 20 (UART1_TX)

Usage:
  python3 uart1_boot.py /dev/ttyS0             (Pi UART)
  python3 uart1_boot.py /dev/ttyUSB0           (FTDI)
  python3 uart1_boot.py /dev/ttyUSB0 --baud 115200
  python3 uart1_boot.py --log existing.log     (replay saved log)
"""

import argparse
import re
import sys
import time

BAUD = 115200
BOOT_TIMEOUT_S = 15.0   # time allowed for boot to reach IDLE


def check(cond, label, detail=""):
    tag = "[PASS]" if cond else "[FAIL]"
    print(f"  {tag} {label}" + (f"  ({detail})" if detail else ""))
    return cond


def analyse_lines(lines):
    passed = failed = 0

    found_idle    = False
    wifi_ok       = None   # True/False/None=not seen
    first_opcode  = None
    sd_count      = None
    psram_init    = None

    for line in lines:
        line = line.strip()

        # Boot state reaching IDLE
        if re.search(r'\[MAIN\].*state.*IDLE|drive.*IDLE|IDLE.*drive', line, re.I):
            found_idle = True

        # WiFi init (cyw43_arch_init return code)
        m = re.search(r'\[WIFI\].*(?:init|cyw43).*?(\d+)|cyw43_arch_init.*?(\d+)', line, re.I)
        if m:
            rc = int(m.group(1) or m.group(2))
            wifi_ok = (rc == 0)

        # Alternative WiFi success message
        if re.search(r'wifi.*connect|ip.*address|dhcp|ssid', line, re.I):
            if wifi_ok is None:
                wifi_ok = True

        # First COMMO opcode from CD32
        m = re.search(r'\[COMMO\].*opcode.*0x([0-9A-Fa-f]{2})|rx.*cmd.*0x([0-9A-Fa-f]{2})', line, re.I)
        if m and first_opcode is None:
            first_opcode = int(m.group(1) or m.group(2), 16)

        # SD image count
        m = re.search(r'(?:found|count|images?).*?(\d+)\s*(?:image|disc|file)', line, re.I)
        if m:
            sd_count = int(m.group(1))

        # PSRAM init
        if re.search(r'psram.*ok|psram.*init|qspi.*psram', line, re.I):
            psram_init = True
        if re.search(r'psram.*fail|psram.*error|no psram', line, re.I):
            psram_init = False

    print("\n--- Boot verification results ---")

    r = check(found_idle, "Boot reaches IDLE drive state")
    passed += r; failed += not r

    if wifi_ok is not None:
        r = check(wifi_ok, "WiFi CYW43439 init succeeded (return code 0)")
        passed += r; failed += not r
    else:
        print("  [skip] No WiFi init log line seen (check --baud or WiFi not built)")

    if psram_init is not None:
        r = check(psram_init, "PSRAM initialised successfully")
        passed += r; failed += not r
    else:
        print("  [skip] No PSRAM log line seen")

    if sd_count is not None:
        r = check(sd_count >= 0, f"SD image scan completed ({sd_count} images found)")
        passed += r; failed += not r
    else:
        print("  [skip] No SD image count log line seen")

    if first_opcode is not None:
        known = first_opcode in (0x15, 0x12, 0x1A, 0x27)  # common power-on opcodes
        r = check(True, f"First COMMO opcode received from CD32: 0x{first_opcode:02X}")
        passed += r; failed += not r
        if not known:
            print(f"    [warn] 0x{first_opcode:02X} is not a common power-on opcode — verify")
    else:
        print("  [skip] No COMMO opcode seen (CD32 may not be connected)")

    total = passed + failed
    print(f"\nuart1_boot: {passed}/{total} passed")
    return failed == 0


def read_from_port(port, baud, timeout_s):
    import serial
    lines = []
    deadline = time.monotonic() + timeout_s
    print(f"[capture] {port} @ {baud} baud  (waiting {timeout_s:.0f} s for boot)")
    try:
        with serial.Serial(port, baud, timeout=1.0) as ser:
            while time.monotonic() < deadline:
                line = ser.readline()
                if line:
                    decoded = line.decode(errors="replace")
                    print(f"  >> {decoded}", end="")
                    lines.append(decoded)
                    if re.search(r'IDLE|boot.*complete|ready', decoded, re.I):
                        break  # boot done; keep reading for WiFi
            # Extra 3 s for WiFi and COMMO
            extra = min(3.0, deadline - time.monotonic())
            if extra > 0:
                end2 = time.monotonic() + extra
                while time.monotonic() < end2:
                    line = ser.readline()
                    if line:
                        decoded = line.decode(errors="replace")
                        print(f"  >> {decoded}", end="")
                        lines.append(decoded)
    except ImportError:
        print("[FAIL] pyserial not installed: pip install pyserial")
        sys.exit(1)
    return lines


def read_from_log(path):
    with open(path) as f:
        return f.readlines()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="Serial port (e.g. /dev/ttyS0)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--timeout", type=float, default=BOOT_TIMEOUT_S,
                    help="Seconds to wait for boot completion")
    ap.add_argument("--log", metavar="FILE",
                    help="Replay a saved UART log instead of connecting live")
    args = ap.parse_args()

    if not args.log and not args.port:
        ap.error("Provide a serial port or --log <file>")

    print("=" * 60)
    print("Boot sequence verification — UART1 monitor")
    print("=" * 60)

    if args.log:
        print(f"[replay] {args.log}")
        lines = read_from_log(args.log)
    else:
        lines = read_from_port(args.port, args.baud, args.timeout)

    ok = analyse_lines(lines)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
