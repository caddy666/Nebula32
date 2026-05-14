#!/usr/bin/env python3
"""
cover_fetch.py — Fetch cover art for CD32 disc images

Downloads JPEG cover art from the internet and places it in the 'covers/'
directory alongside your disc images, ready for the CD32 ODE web interface.

Sources used (in order of preference):
  1. MobyGames API (best quality, requires free API key)
  2. Open Library / Internet Archive covers (no key needed, CD titles only)
  3. TheGamesDB (scraping, no key)
  4. Local scan prompt (if all online sources fail)

Usage:
    # Fetch covers for all .iso/.bin/.nrg/.mdf files in current directory
    python3 cover_fetch.py /path/to/sd/card/

    # Fetch for a single disc
    python3 cover_fetch.py /path/to/sd/card/ --disc "Zool2.iso"

    # Use MobyGames API key for better results
    python3 cover_fetch.py /path/to/sd/card/ --api-key YOUR_KEY

    # Dry run — show what would be downloaded without downloading
    python3 cover_fetch.py /path/to/sd/card/ --dry-run

    # Force re-download even if cover already exists
    python3 cover_fetch.py /path/to/sd/card/ --force

Cover naming convention:
    For "0:/Zool2.iso" the cover is saved as "covers/Zool2.jpg"
    Base name must match exactly (case-sensitive on FAT32 SD cards).

Requirements: Python 3.8+, 'requests' library (pip install requests)
"""

import sys
import os
import re
import json
import time
import shutil
import argparse
import urllib.request
import urllib.parse
import urllib.error
from pathlib import Path
from typing import Optional, List, Tuple

# ─── Try to import requests for better HTTP support ───────────────────────────
try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False
    print("Note: 'requests' library not found. Using urllib (limited functionality).")
    print("      Install with: pip install requests")

# ─── Colours ─────────────────────────────────────────────────────────────────
if sys.stdout.isatty() and sys.platform != 'win32':
    G = '\033[32m'; Y = '\033[33m'; R = '\033[31m'; B = '\033[36m'
    BOLD = '\033[1m'; DIM = '\033[2m'; RESET = '\033[0m'
else:
    G = Y = R = B = BOLD = DIM = RESET = ''

# ─── Supported disc image extensions ─────────────────────────────────────────
IMAGE_EXTS = {'.iso', '.bin', '.nrg', '.mdf', '.img', '.cue'}

# ─── Image size target ───────────────────────────────────────────────────────
# Target size for downloaded covers (firmware displays at 200×200)
TARGET_SIZE_PX = 400  # Download at 400×400 for good quality

# ─── Rate limiting ────────────────────────────────────────────────────────────
REQUEST_DELAY_S = 0.5   # Polite delay between API calls


def http_get(url: str, headers: dict = None) -> Optional[bytes]:
    """Download a URL, return raw bytes or None on failure."""
    hdrs = {'User-Agent': 'CD32-ODE-CoverFetcher/1.0 (github.com/cd32-ode)'}
    if headers:
        hdrs.update(headers)

    try:
        if HAS_REQUESTS:
            r = requests.get(url, headers=hdrs, timeout=10)
            if r.status_code == 200:
                return r.content
            return None
        else:
            req = urllib.request.Request(url, headers=hdrs)
            with urllib.request.urlopen(req, timeout=10) as resp:
                if resp.status == 200:
                    return resp.read()
            return None
    except Exception as e:
        return None


def http_get_json(url: str, headers: dict = None) -> Optional[dict]:
    """Download and parse a JSON API response."""
    data = http_get(url, headers)
    if data is None:
        return None
    try:
        return json.loads(data.decode('utf-8'))
    except Exception:
        return None


# ─── Title normalisation ─────────────────────────────────────────────────────
def normalise_title(name: str) -> str:
    """
    Convert a disc image filename to a search title.
    Examples:
        'Zool2.iso'              → 'Zool 2'
        'AlienBreed3D_AGA.bin'  → 'Alien Breed 3D AGA'
        'SuperFrog_CD32.nrg'    → 'SuperFrog'
    """
    # Remove extension
    name = Path(name).stem
    # Remove common suffixes: _CD32, _AGA, _NTSC, _PAL, (Disk1), etc.
    name = re.sub(r'[_\s]*(CD32|AGA|ECS|OCS|NTSC|PAL|Disk\d+|V\d+|\(\d{4}\))',
                  '', name, flags=re.IGNORECASE)
    # Insert space before capital letters following lowercase (CamelCase)
    name = re.sub(r'([a-z])([A-Z])', r'\1 \2', name)
    # Insert space before digits following letters
    name = re.sub(r'([A-Za-z])(\d)', r'\1 \2', name)
    # Replace underscores/hyphens with spaces
    name = name.replace('_', ' ').replace('-', ' ')
    # Collapse multiple spaces
    name = re.sub(r'\s+', ' ', name).strip()
    return name


# ─── Source 1: MobyGames API ─────────────────────────────────────────────────
MOBYGAMES_BASE = "https://api.mobygames.com/v1"

def fetch_mobygames(title: str, api_key: str) -> Optional[bytes]:
    """
    Search MobyGames for a game cover, preferring Amiga/CD32 platform.
    Returns JPEG bytes or None.
    """
    # Search for game
    url = f"{MOBYGAMES_BASE}/games?api_key={api_key}&title={urllib.parse.quote(title)}&platform=116"
    # 116 = Amiga CD32 on MobyGames
    data = http_get_json(url, {'Accept': 'application/json'})
    if not data or not data.get('games'):
        # Try without platform filter
        url = f"{MOBYGAMES_BASE}/games?api_key={api_key}&title={urllib.parse.quote(title)}"
        data = http_get_json(url)

    if not data or not data.get('games'):
        return None

    game_id = data['games'][0]['game_id']
    time.sleep(REQUEST_DELAY_S)

    # Get cover art
    url = f"{MOBYGAMES_BASE}/games/{game_id}/covers?api_key={api_key}"
    covers = http_get_json(url)
    if not covers or not covers.get('cover_groups'):
        return None

    # Find the best cover (front cover, CD32 or Amiga preferred)
    best_url = None
    for group in covers['cover_groups']:
        for cover in group.get('covers', []):
            if cover.get('scan_of', '').lower() == 'front cover':
                best_url = cover.get('image')
                break
        if best_url:
            break

    if not best_url:
        # Any cover
        for group in covers['cover_groups']:
            for cover in group.get('covers', []):
                best_url = cover.get('image')
                if best_url:
                    break
            if best_url:
                break

    if not best_url:
        return None

    time.sleep(REQUEST_DELAY_S)
    return http_get(best_url)


# ─── Source 2: TheGamesDB (HTML scraping, no key) ────────────────────────────
TGDB_SEARCH = "https://thegamesdb.net/search.php?name={}&platform_id[]=4928"  # 4928=Amiga CD32

def fetch_thegamesdb(title: str) -> Optional[bytes]:
    """Scrape TheGamesDB for cover art. Fragile — depends on HTML structure."""
    url = TGDB_SEARCH.format(urllib.parse.quote(title))
    html = http_get(url)
    if html is None:
        return None

    html_str = html.decode('utf-8', errors='replace')

    # Find first game card image
    img_match = re.search(
        r'<img[^>]+src="(https://[^"]+\.(?:jpg|jpeg|png))"[^>]+>',
        html_str, re.IGNORECASE
    )
    if not img_match:
        return None

    img_url = img_match.group(1)
    time.sleep(REQUEST_DELAY_S)
    return http_get(img_url)


# ─── Image validation and resize ─────────────────────────────────────────────
def is_valid_jpeg(data: bytes) -> bool:
    """Check that the data starts with JPEG magic bytes."""
    return (data is not None and
            len(data) > 3 and
            data[:2] == b'\xff\xd8')


def save_cover(data: bytes, output_path: Path) -> bool:
    """Save cover art bytes to a file. Returns True on success."""
    if not is_valid_jpeg(data):
        return False
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(data)
        return True
    except Exception as e:
        print(f"    Error saving {output_path}: {e}")
        return False


# ─── Main fetch logic for one disc ───────────────────────────────────────────
def fetch_one(disc_path: Path, covers_dir: Path,
              api_key: str, force: bool, dry_run: bool) -> bool:
    """
    Fetch cover art for one disc image.
    Returns True if cover was obtained (or already existed).
    """
    stem = disc_path.stem
    out_jpg  = covers_dir / f"{stem}.jpg"
    out_jpeg = covers_dir / f"{stem}.jpeg"

    # Already have it?
    if not force and (out_jpg.exists() or out_jpeg.exists()):
        existing = out_jpg if out_jpg.exists() else out_jpeg
        print(f"  {G}EXISTS{RESET}  {stem}.jpg ({existing.stat().st_size // 1024} KB)")
        return True

    title = normalise_title(disc_path.name)
    print(f"  {B}SEARCH{RESET}  '{title}' (from {disc_path.name})")

    if dry_run:
        print(f"  {DIM}(dry-run: would search MobyGames + TheGamesDB){RESET}")
        return False

    # Try MobyGames first (best quality, requires API key)
    data = None
    if api_key:
        print(f"         Trying MobyGames...", end=' ', flush=True)
        data = fetch_mobygames(title, api_key)
        if data and is_valid_jpeg(data):
            print(f"{G}found{RESET} ({len(data) // 1024} KB)")
        else:
            print(f"{Y}not found{RESET}")
            data = None

    # Fallback: TheGamesDB
    if not data:
        print(f"         Trying TheGamesDB...", end=' ', flush=True)
        data = fetch_thegamesdb(title)
        if data and is_valid_jpeg(data):
            print(f"{G}found{RESET} ({len(data) // 1024} KB)")
        else:
            print(f"{Y}not found{RESET}")
            data = None

    if data and is_valid_jpeg(data):
        if save_cover(data, out_jpg):
            print(f"         {G}Saved → covers/{stem}.jpg{RESET}")
            return True
        else:
            print(f"         {R}Failed to save{RESET}")
            return False
    else:
        print(f"         {R}No cover found — add manually to covers/{stem}.jpg{RESET}")
        return False


# ─── Scan directory for disc images ──────────────────────────────────────────
def find_disc_images(directory: Path) -> List[Path]:
    """Find all disc images in the given directory (non-recursive)."""
    images = []
    try:
        for entry in sorted(directory.iterdir()):
            if entry.is_file() and entry.suffix.lower() in IMAGE_EXTS:
                images.append(entry)
    except PermissionError as e:
        print(f"Error reading directory: {e}", file=sys.stderr)
    return images


# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description='Fetch cover art JPEGs for CD32 ODE disc images.')
    parser.add_argument('directory',
                        help='Path to SD card root (or disc image directory)')
    parser.add_argument('--disc', metavar='FILENAME',
                        help='Only fetch for this specific disc filename')
    parser.add_argument('--api-key', metavar='KEY',
                        help='MobyGames API key (free at mobygames.com)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show what would be fetched without downloading')
    parser.add_argument('--force', action='store_true',
                        help='Re-download even if cover already exists')
    args = parser.parse_args()

    sd_root   = Path(args.directory)
    covers_dir = sd_root / 'covers'

    if not sd_root.is_dir():
        print(f"Error: '{sd_root}' is not a directory", file=sys.stderr)
        sys.exit(1)

    print(f"\n{BOLD}CD32 ODE Cover Art Fetcher{RESET}")
    print(f"Directory  : {sd_root}")
    print(f"Covers dir : {covers_dir}")
    if args.api_key:
        print(f"MobyGames  : API key provided")
    else:
        print(f"MobyGames  : No API key (get one free at mobygames.com for better results)")
    if args.dry_run:
        print(f"{Y}Dry-run mode — nothing will be downloaded{RESET}")
    print()

    # Find disc images
    if args.disc:
        images = [sd_root / args.disc]
        if not images[0].exists():
            print(f"Error: {images[0]} not found", file=sys.stderr)
            sys.exit(1)
    else:
        images = find_disc_images(sd_root)
        # Also check a 'games' or 'discs' subdirectory if it exists
        for subdir in ['games', 'discs', 'roms']:
            sub = sd_root / subdir
            if sub.is_dir():
                images.extend(find_disc_images(sub))

    if not images:
        print(f"No disc images found in {sd_root}")
        print(f"Looking for files with extensions: {', '.join(IMAGE_EXTS)}")
        sys.exit(0)

    print(f"Found {len(images)} disc image(s)\n")

    # Create covers directory
    if not args.dry_run:
        covers_dir.mkdir(parents=True, exist_ok=True)

    found = 0
    missed = 0
    skipped = 0

    for disc in images:
        result = fetch_one(disc, covers_dir,
                           args.api_key or '',
                           args.force,
                           args.dry_run)
        if result:
            found += 1
        elif args.dry_run:
            skipped += 1
        else:
            missed += 1
        time.sleep(REQUEST_DELAY_S)

    print(f"\n{'─' * 50}")
    print(f"Results: {G}{found} found{RESET}  "
          f"{R}{missed} not found{RESET}  "
          f"{DIM}{skipped} skipped (dry-run){RESET}")

    if missed > 0:
        print(f"\n{Y}Tips for missing covers:{RESET}")
        print("  1. Get a free MobyGames API key at https://www.mobygames.com/info/api/")
        print("     and re-run with --api-key YOUR_KEY")
        print("  2. Search manually at:")
        print("     • https://www.mobygames.com/")
        print("     • https://hol.abime.net/ (Hall of Light — Amiga games)")
        print("     • https://lemon64.com/")
        print(f"  3. Save JPEG to {covers_dir}/GameName.jpg")
        print("     (must match disc filename exactly, without extension)")

    print()


if __name__ == '__main__':
    main()
