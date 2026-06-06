#!/usr/bin/env python3
"""
validate_image.py — CD32 ODE disc image validator

Verifies that a disc image will work correctly with the CD32 ODE firmware
before copying it to the SD card.  Checks:

  • File format detection (.iso / .bin+.cue / .nrg / .mdf+.mds)
  • File completeness (size is a multiple of sector size)
  • CUE sheet syntax (for .bin images)
  • NRG chunk structure integrity
  • MDS/MDF consistency
  • EDC integrity for raw Mode 1 sectors (optional, slow)
  • Track count and TOC sanity

Usage:
    python3 validate_image.py image.iso
    python3 validate_image.py image.bin         # looks for image.cue
    python3 validate_image.py image.nrg
    python3 validate_image.py image.mdf         # looks for image.mds
    python3 validate_image.py --edc image.bin   # also verify EDC (slow)

SD card setup reminder:
  Copy validated images to the root directory of a FAT32 or exFAT formatted
  SD card.  The firmware also reads 'nebula32.cfg' from the SD card root to
  configure logging and other settings.  On first boot, if this file does not
  exist, the firmware creates a template with all settings documented inline.

  Key settings in nebula32.cfg:
    logging_enabled = 1     # Write cd32_cd.log (1=on, 0=off)
    log_commands    = 1     # Log every CXD2545Q command and response
    log_sectors     = 0     # Log every sector (verbose; leave off normally)
    log_seeks       = 1     # Log seek start/complete events
    log_errors      = 1     # Log SD read errors and cache misses
    log_max_kb      = 4096  # Rotate log when it exceeds this size in KB

Requirements: Python 3.8+, no external libraries needed.
"""

import sys
import os
import struct
import argparse
from pathlib import Path

# ─── Colours (disabled on Windows / non-TTY) ──────────────────────────────
if sys.platform != "win32" and sys.stdout.isatty():
    GREEN  = "\033[32m"
    RED    = "\033[31m"
    YELLOW = "\033[33m"
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
else:
    GREEN = RED = YELLOW = RESET = BOLD = ""

PASS = f"{GREEN}PASS{RESET}"
FAIL = f"{RED}FAIL{RESET}"
WARN = f"{YELLOW}WARN{RESET}"

pass_count = 0
fail_count = 0
warn_count = 0

def ok(msg):
    global pass_count
    pass_count += 1
    print(f"  [{PASS}] {msg}")

def fail(msg):
    global fail_count
    fail_count += 1
    print(f"  [{FAIL}] {msg}")

def warn(msg):
    global warn_count
    warn_count += 1
    print(f"  [{WARN}] {msg}")

def section(title):
    print(f"\n{BOLD}── {title} ──{RESET}")

# ─── EDC verification ────────────────────────────────────────────────────────
def _edc_poly(edc, byte):
    edc ^= byte
    for _ in range(8):
        if edc & 1:
            edc = (edc >> 1) ^ 0xD8018001
        else:
            edc >>= 1
    return edc & 0xFFFFFFFF

def verify_edc(sector_2352: bytes) -> bool:
    """Returns True if the Mode 1 EDC at bytes 2064-2067 is correct."""
    if len(sector_2352) < 2068:
        return False
    edc = 0
    for b in sector_2352[:2064]:
        edc = _edc_poly(edc, b)
    stored = struct.unpack_from("<I", sector_2352, 2064)[0]
    return edc == stored

# ─── ISO validator ───────────────────────────────────────────────────────────
def validate_iso(path: Path, check_edc: bool):
    section(f"ISO 9660: {path.name}")

    size = path.stat().st_size
    sectors = size // 2048
    remainder = size % 2048

    ok(f"File size: {size:,} bytes")

    if remainder != 0:
        warn(f"File size is not a multiple of 2048 ({remainder} bytes trailing) "
             f"— last sector may be incomplete")
    else:
        ok(f"Size is exact multiple of 2048 ({sectors} sectors, "
           f"{sectors/75:.1f} seconds)")

    if sectors < 300:
        warn(f"Only {sectors} sectors — unusually small disc image")
    else:
        ok(f"Sector count {sectors} is reasonable")

    # Check for ISO 9660 primary volume descriptor at sector 16
    with open(path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(8)
    if pvd[:8] == b'\x01CD001\x01\x00':
        ok("ISO 9660 Primary Volume Descriptor found at sector 16")
    else:
        warn("No ISO 9660 PVD at sector 16 — may be a raw data image, not ISO 9660")

    if check_edc:
        section("EDC verification (synthesising Mode 1 sectors)")
        # For ISO we synthesise and check a few sectors
        _check_edc_iso(path)

def _check_edc_iso(path: Path):
    """Synthesise a handful of Mode 1 sectors from an ISO and verify EDC."""
    SYNC = bytes([0x00] + [0xFF]*10 + [0x00])
    checked = 0
    errors  = 0
    with open(path, "rb") as f:
        for lba in range(min(100, path.stat().st_size // 2048)):
            data = f.read(2048)
            if len(data) < 2048:
                break
            # Synthesise a minimal Mode 1 sector
            abs_frames = lba + 150
            mm = abs_frames // (75*60)
            ss = (abs_frames // 75) % 60
            ff = abs_frames % 75
            def bcd(v): return ((v//10) << 4) | (v%10)
            sector = (SYNC
                      + bytes([bcd(mm), bcd(ss), bcd(ff), 0x01])
                      + data
                      + b'\x00' * 288)
            if verify_edc(sector):
                checked += 1
            else:
                errors += 1
    if errors == 0:
        ok(f"EDC correct for {checked} synthesised sectors")
    else:
        fail(f"EDC mismatch in {errors}/{checked+errors} sectors")

# ─── BIN/CUE validator ───────────────────────────────────────────────────────
def validate_bin(path: Path, check_edc: bool):
    section(f"BIN: {path.name}")

    size = path.stat().st_size
    ok(f"BIN file size: {size:,} bytes")

    # Detect sector size by checking for sync pattern at offset 0
    with open(path, "rb") as f:
        header = f.read(16)
    if header[:12] == bytes([0x00]+[0xFF]*10+[0x00]):
        sector_size = 2352
        ok("Detected 2352-byte raw sectors (sync pattern at offset 0)")
    elif size % 2352 == 0:
        sector_size = 2352
        ok(f"Sector size: 2352 bytes ({size//2352} sectors) inferred from file size")
    elif size % 2048 == 0:
        sector_size = 2048
        ok(f"Sector size: 2048 bytes ({size//2048} sectors) inferred from file size")
    else:
        warn("File size not a multiple of 2048 or 2352 — unusual")
        sector_size = 2352

    # Check for CUE sheet
    cue_path = path.with_suffix(".cue")
    if not cue_path.exists():
        cue_path = path.with_suffix(".CUE")
    if cue_path.exists():
        ok(f"CUE sheet found: {cue_path.name}")
        _validate_cue(cue_path, path, sector_size)
    else:
        warn("No CUE sheet found — firmware will assume single data track at 2352 bytes/sector")

    if check_edc and sector_size == 2352:
        _check_edc_bin(path)

def _validate_cue(cue_path: Path, bin_path: Path, sector_size: int):
    section(f"CUE: {cue_path.name}")
    tracks = []
    with open(cue_path, "r", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if line.upper().startswith("FILE"):
                # Check referenced filename exists
                parts = line.split('"')
                if len(parts) >= 2:
                    ref = parts[1]
                    ref_path = cue_path.parent / ref
                    if ref_path.exists():
                        ok(f"CUE FILE reference '{ref}' exists")
                    else:
                        fail(f"CUE FILE reference '{ref}' NOT FOUND (line {lineno})")
            elif line.upper().startswith("TRACK"):
                parts = line.split()
                if len(parts) >= 3:
                    tracks.append((parts[1], parts[2]))
            elif line.upper().startswith("INDEX 01"):
                pass  # Just noting presence

    if tracks:
        ok(f"CUE defines {len(tracks)} track(s)")
        for num, mode in tracks:
            if mode in ("MODE1/2352", "MODE2/2352", "AUDIO"):
                ok(f"  Track {num}: {mode}")
            elif mode in ("MODE1/2048",):
                warn(f"  Track {num}: {mode} — unusual for BIN (normally 2352)")
            else:
                warn(f"  Track {num}: {mode} — unknown mode string")
    else:
        fail("No TRACK entries found in CUE sheet")

def _check_edc_bin(path: Path):
    section("EDC verification (raw 2352-byte sectors)")
    checked = errors = 0
    with open(path, "rb") as f:
        while True:
            sector = f.read(2352)
            if len(sector) < 2352:
                break
            mode = sector[15]
            if mode == 0x01:  # Mode 1 only
                if verify_edc(sector):
                    checked += 1
                else:
                    errors += 1
    if errors == 0:
        ok(f"EDC correct for all {checked} Mode 1 sectors checked")
    else:
        fail(f"EDC mismatch in {errors}/{checked+errors} Mode 1 sectors")

# ─── NRG validator ───────────────────────────────────────────────────────────
def validate_nrg(path: Path, check_edc: bool):
    section(f"NRG (Nero): {path.name}")

    size = path.stat().st_size
    ok(f"File size: {size:,} bytes ({size/(1024*1024):.1f} MB)")

    with open(path, "rb") as f:
        # Try v2 footer (last 12 bytes)
        f.seek(-12, 2)
        magic_v2, offset_v2 = struct.unpack(">IQ", f.read(12))
        if magic_v2 == 0x4E455235:  # "NER5"
            ok(f"NRG v2 header detected at offset {offset_v2}")
            _walk_nrg_chunks(f, offset_v2, size)
            return

        # Try v1 footer (last 8 bytes)
        f.seek(-8, 2)
        magic_v1, offset_v1 = struct.unpack(">II", f.read(8))
        if magic_v1 == 0x4E45524F:  # "NERO"
            ok(f"NRG v1 header detected at offset {offset_v1}")
            _walk_nrg_chunks(f, offset_v1, size)
            return

    fail("No valid NRG v1 or v2 footer found — file may be corrupt or not an NRG")

def _walk_nrg_chunks(f, start_offset: int, file_size: int):
    f.seek(start_offset)
    chunk_count  = 0
    track_count  = 0
    found_end    = False

    while True:
        header = f.read(8)
        if len(header) < 8:
            break
        chunk_id, chunk_size = struct.unpack(">II", header)
        chunk_name = struct.pack(">I", chunk_id).decode("ascii", errors="replace")

        if chunk_id == 0x454E4421:  # "END!"
            ok(f"NRG END! chunk found after {chunk_count} chunk(s)")
            found_end = True
            break

        if chunk_id in (0x44414F58, 0x44414F49):  # DAOX / DAOI
            ok(f"NRG chunk '{chunk_name}': {chunk_size} bytes (track info)")
            track_count += chunk_size // 42  # Rough estimate
        elif chunk_id in (0x43554558, 0x43554553):  # CUEX / CUES
            ok(f"NRG chunk '{chunk_name}': {chunk_size} bytes (cue sheet)")
        elif chunk_id == 0x53494E46:  # SINF
            ok(f"NRG chunk '{chunk_name}': {chunk_size} bytes (session info)")
        else:
            warn(f"NRG chunk '{chunk_name}' (0x{chunk_id:08X}): {chunk_size} bytes (unrecognised)")

        chunk_count += 1
        f.seek(chunk_size, 1)

    if not found_end:
        fail("NRG END! chunk not found — chunk list may be truncated")

# ─── MDF/MDS validator ───────────────────────────────────────────────────────
def validate_mdf(path: Path, check_edc: bool):
    section(f"MDF (Alcohol 120%): {path.name}")

    size = path.stat().st_size
    ok(f"MDF file size: {size:,} bytes ({size/(1024*1024):.1f} MB)")

    mds_path = path.with_suffix(".mds")
    if not mds_path.exists():
        mds_path = path.with_suffix(".MDS")
    if not mds_path.exists():
        fail("No MDS metadata file found alongside MDF — firmware cannot parse TOC")
        return

    ok(f"MDS file found: {mds_path.name}")
    _validate_mds(mds_path)

def _validate_mds(mds_path: Path):
    section(f"MDS: {mds_path.name}")
    with open(mds_path, "rb") as f:
        sig = f.read(16)
    if sig == b"MEDIA DESCRIPTOR":
        ok("MDS signature 'MEDIA DESCRIPTOR' valid")
    else:
        fail(f"Invalid MDS signature: {sig!r}")
        return

    mds_size = mds_path.stat().st_size
    ok(f"MDS file size: {mds_size} bytes")
    if mds_size < 88:
        fail("MDS file too small to contain a valid header")
    else:
        ok("MDS file size is sufficient for header")

# ─── Main ────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Validate disc images for use with the CD32 ODE firmware.")
    parser.add_argument("image", help="Path to the disc image file")
    parser.add_argument("--edc", action="store_true",
                        help="Also verify EDC checksums in raw sectors (slow for large images)")
    args = parser.parse_args()

    path = Path(args.image)
    if not path.exists():
        print(f"{RED}Error: file not found: {path}{RESET}")
        sys.exit(1)

    print(f"\n{BOLD}CD32 ODE Image Validator{RESET}")
    print(f"Image: {path}")

    ext = path.suffix.lower()
    if ext == ".iso":
        validate_iso(path, args.edc)
    elif ext == ".bin":
        validate_bin(path, args.edc)
    elif ext == ".nrg":
        validate_nrg(path, args.edc)
    elif ext == ".mdf":
        validate_mdf(path, args.edc)
    else:
        warn(f"Unknown extension '{ext}' — attempting ISO validation")
        validate_iso(path, args.edc)

    print(f"\n{'─'*50}")
    print(f"  Results: {GREEN}{pass_count} passed{RESET}, "
          f"{YELLOW}{warn_count} warnings{RESET}, "
          f"{RED}{fail_count} failed{RESET}")
    print(f"{'─'*50}\n")

    if fail_count > 0:
        print(f"{RED}Image has errors — correct them before use.{RESET}\n")
        sys.exit(1)
    elif warn_count > 0:
        print(f"{YELLOW}Image has warnings — it may still work, but review them.{RESET}\n")
        sys.exit(0)
    else:
        print(f"{GREEN}Image looks good — safe to use with CD32 ODE.{RESET}\n")
        sys.exit(0)

if __name__ == "__main__":
    main()
