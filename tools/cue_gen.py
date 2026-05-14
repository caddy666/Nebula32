#!/usr/bin/env python3
"""
cue_gen.py — Generate a .cue sheet for a raw BIN disc image

When you have a .bin file but no accompanying .cue sheet, the CD32 ODE
firmware assumes a single MODE1/2352 data track.  That works for most
single-track disc images, but multi-track images (data + audio) need a
proper .cue to be parsed correctly.

This script generates a minimal .cue sheet by inspecting the BIN file:
  • Detects 2048-byte vs 2352-byte sector size
  • Identifies Mode 1, Mode 2, and audio sectors from sync patterns
  • Places track boundaries where the sector type changes
  • Outputs a .cue file in the same directory as the .bin

Usage:
    python3 cue_gen.py image.bin           # auto-detect, write image.cue
    python3 cue_gen.py image.bin -o my.cue # specify output filename
    python3 cue_gen.py image.bin --dry-run # print CUE without writing

Requirements: Python 3.8+, no external libraries.
"""

import sys
import os
import argparse
import struct
from pathlib import Path
from typing import List, Tuple, Optional

# CD sync pattern (12 bytes) identifying raw Mode 1 / Mode 2 sectors
CD_SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])

# Sector mode byte offset within a 2352-byte sector
MODE_OFFSET = 15

# ─── Sector type detection ────────────────────────────────────────────────────

def detect_sector_size(path: Path) -> int:
    """
    Determine whether the BIN uses 2048 or 2352 byte sectors.
    Checks for the CD sync pattern at offset 0; if absent tries 2048.
    """
    with open(path, "rb") as f:
        header = f.read(16)

    if len(header) >= 12 and header[:12] == CD_SYNC:
        return 2352

    # No sync at offset 0 — check if 2352 divides the file size exactly
    size = path.stat().st_size
    if size % 2352 == 0:
        # Could still be 2352; check a sector-aligned sync pattern
        with open(path, "rb") as f:
            f.seek(2352)
            sector2 = f.read(12)
        if sector2 == CD_SYNC:
            return 2352

    if size % 2048 == 0:
        return 2048

    # Default: 2352 with possible partial last sector
    return 2352


def read_sector_type(f, offset: int, sector_size: int) -> str:
    """
    Read one sector and return its type string: 'MODE1', 'MODE2', or 'AUDIO'.
    'offset' is the byte offset in the file.
    """
    f.seek(offset)
    sector = f.read(min(sector_size, 32))  # Only need the header

    if len(sector) < 12:
        return "AUDIO"

    if sector_size == 2048:
        return "MODE1"  # ISO-style has no header; assume Mode 1

    if sector[:12] != CD_SYNC:
        return "AUDIO"

    if len(sector) < 16:
        return "AUDIO"

    mode = sector[MODE_OFFSET]
    if mode == 0x01:
        return "MODE1"
    elif mode == 0x02:
        return "MODE2"
    else:
        return "AUDIO"  # Mode 0x00 = audio, undefined = treat as audio


# ─── Track boundary detection ─────────────────────────────────────────────────

class Track:
    def __init__(self, number: int, mode: str, start_lba: int, sector_size: int):
        self.number      = number
        self.mode        = mode        # 'MODE1', 'MODE2', 'AUDIO'
        self.start_lba   = start_lba  # In sectors (no pregap offset here)
        self.sector_size = sector_size
        self.end_lba     = 0           # Set after detection

    def cue_mode_string(self) -> str:
        """Return the CUE mode string for this track."""
        if self.mode == "AUDIO":
            return "AUDIO"
        elif self.mode == "MODE1":
            if self.sector_size == 2048:
                return "MODE1/2048"
            return "MODE1/2352"
        else:  # MODE2
            return "MODE2/2352"

    def lba_to_msf(self, lba: int) -> str:
        """Convert a sector LBA to MM:SS:FF string for a CUE INDEX line."""
        # CUE INDEX 01 uses the LBA relative to the start of the file, not disc
        # (i.e., LBA 0 = 00:00:00 in a CUE file, unlike real disc addressing)
        frames = lba % 75
        seconds = (lba // 75) % 60
        minutes = lba // (75 * 60)
        return f"{minutes:02d}:{seconds:02d}:{frames:02d}"


def detect_tracks(path: Path, sector_size: int,
                  sample_interval: int = 150) -> List[Track]:
    """
    Scan the BIN file and detect track type changes.
    Samples every 'sample_interval' sectors for speed.
    Returns a list of Track objects.
    """
    total_sectors = path.stat().st_size // sector_size
    if total_sectors == 0:
        return []

    tracks: List[Track] = []
    current_type: Optional[str] = None
    track_start: int = 0
    track_num: int = 1

    print(f"  Scanning {total_sectors} sectors (sample every {sample_interval})...")

    with open(path, "rb") as f:
        for lba in range(0, total_sectors, sample_interval):
            sector_type = read_sector_type(f, lba * sector_size, sector_size)

            if current_type is None:
                # First sector
                current_type = sector_type
                track_start  = lba
            elif sector_type != current_type:
                # Track type changed — close current track, start new one
                trk = Track(track_num, current_type, track_start, sector_size)
                trk.end_lba = lba
                tracks.append(trk)
                print(f"  Track {track_num}: {current_type} "
                      f"LBA {track_start}–{lba-1} "
                      f"({lba - track_start} sectors)")
                track_num    += 1
                current_type  = sector_type
                track_start   = lba

    # Close the last track
    if current_type is not None:
        trk = Track(track_num, current_type, track_start, sector_size)
        trk.end_lba = total_sectors
        tracks.append(trk)
        print(f"  Track {track_num}: {current_type} "
              f"LBA {track_start}–{total_sectors-1} "
              f"({total_sectors - track_start} sectors)")

    return tracks


# ─── CUE generation ───────────────────────────────────────────────────────────

def generate_cue(bin_path: Path, tracks: List[Track]) -> str:
    """Generate the text content of the .cue file."""
    lines = []
    bin_filename = bin_path.name
    lines.append(f'FILE "{bin_filename}" BINARY')

    for trk in tracks:
        mode_str = trk.cue_mode_string()
        trk_num  = f"{trk.number:02d}"
        msf      = trk.lba_to_msf(trk.start_lba)

        lines.append(f"  TRACK {trk_num} {mode_str}")

        # Add a 2-second (150-frame) pregap for track 1
        if trk.number == 1:
            lines.append(f"    PREGAP 00:02:00")
            lines.append(f"    INDEX 01 {msf}")
        else:
            # For subsequent tracks, INDEX 00 (optional pregap) not emitted
            lines.append(f"    INDEX 01 {msf}")

    return "\n".join(lines) + "\n"


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate a .cue sheet for a raw BIN disc image.")
    parser.add_argument("bin_file", help="Path to the .bin image file")
    parser.add_argument("-o", "--output",
                        help="Output .cue path (default: same name as BIN)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the generated CUE without writing to disk")
    parser.add_argument("--sector-size", type=int, choices=[2048, 2352],
                        help="Override sector size detection")
    parser.add_argument("--sample", type=int, default=150,
                        help="Sector sampling interval for track detection (default 150)")
    args = parser.parse_args()

    bin_path = Path(args.bin_file)
    if not bin_path.exists():
        print(f"Error: {bin_path} not found", file=sys.stderr)
        sys.exit(1)

    out_path = Path(args.output) if args.output else bin_path.with_suffix(".cue")

    print(f"\ncue_gen.py — CUE sheet generator for CD32 ODE")
    print(f"Input : {bin_path}  ({bin_path.stat().st_size:,} bytes)")

    # Detect sector size
    if args.sector_size:
        sector_size = args.sector_size
        print(f"Sector size : {sector_size} (forced)")
    else:
        sector_size = detect_sector_size(bin_path)
        print(f"Sector size : {sector_size} (detected)")

    total_sectors = bin_path.stat().st_size // sector_size
    print(f"Sectors     : {total_sectors}  ({total_sectors/75:.1f} seconds)")

    # Detect tracks
    print(f"\nDetecting tracks...")
    tracks = detect_tracks(bin_path, sector_size, args.sample)

    if not tracks:
        print("Error: no sectors found in BIN file", file=sys.stderr)
        sys.exit(1)

    print(f"\nDetected {len(tracks)} track(s)")

    # Generate CUE text
    cue_text = generate_cue(bin_path, tracks)

    # Output
    print(f"\n{'─'*50}")
    print(cue_text)
    print(f"{'─'*50}")

    if args.dry_run:
        print("(dry-run: not written to disk)")
    else:
        if out_path.exists():
            answer = input(f"Overwrite existing {out_path}? [y/N] ").strip().lower()
            if answer != "y":
                print("Aborted.")
                sys.exit(0)

        with open(out_path, "w") as f:
            f.write(cue_text)
        print(f"Written: {out_path}")

    print("\nDone.")


if __name__ == "__main__":
    main()
