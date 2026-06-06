#!/usr/bin/env python3
"""
log_analyse.py — CD32 ODE activity log analyser

Parses cd32_cd.log and produces human-readable reports:

  summary      General statistics: command counts, seek times, error rate
  commands     Command frequency table with parameter/response breakdown
  seeks        Seek timeline with distances and estimated vs actual times
  errors       All errors and warnings, grouped by type
  timeline     Chronological event stream (filtered by category)
  sessions     Per-boot-session statistics
  sectors      Sector delivery rate and LBA range analysis (if log_sectors=1)
  audio        Audio underrun and start/stop timeline
  replay       Simulate reading the log as it was written (live tail)

Usage:
    python3 log_analyse.py cd32_cd.log summary
    python3 log_analyse.py cd32_cd.log commands
    python3 log_analyse.py cd32_cd.log seeks
    python3 log_analyse.py cd32_cd.log errors
    python3 log_analyse.py cd32_cd.log timeline --filter CMD SEEK DRV
    python3 log_analyse.py cd32_cd.log sessions
    python3 log_analyse.py cd32_cd.log replay --speed 10

Requirements: Python 3.8+, no external libraries needed.
"""

import sys
import re
import time
import argparse
from pathlib import Path
from collections import defaultdict, Counter
from typing import List, Optional, Tuple


# ─── Log line parser ─────────────────────────────────────────────────────────

class LogLine:
    """Represents one parsed line from cd32_cd.log."""
    __slots__ = ('ts_ms', 'level', 'tag', 'message', 'raw')

    LINE_RE = re.compile(
        r'^\[(\s*\d+)\]\s+(DEBG|INFO|WARN|ERR )\s+(\S{1,4})\s{1,3}(.*)$'
    )

    def __init__(self, ts_ms: int, level: str, tag: str, message: str, raw: str):
        self.ts_ms   = ts_ms
        self.level   = level.strip()
        self.tag     = tag.strip()
        self.message = message.strip()
        self.raw     = raw

    @classmethod
    def parse(cls, line: str) -> Optional['LogLine']:
        m = cls.LINE_RE.match(line.rstrip())
        if not m:
            return None
        return cls(
            ts_ms   = int(m.group(1).strip()),
            level   = m.group(2),
            tag     = m.group(3),
            message = m.group(4),
            raw     = line.rstrip()
        )

    def is_session_start(self) -> bool:
        return 'SESSION START' in self.message

    def is_session_end(self) -> bool:
        return 'SESSION END' in self.message


def load_log(path: Path) -> List[LogLine]:
    """Parse all lines from the log file, skipping separator lines."""
    lines = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            if raw.startswith('===') or raw.startswith('---') or raw.strip() == '':
                continue
            parsed = LogLine.parse(raw)
            if parsed:
                lines.append(parsed)
    return lines


def split_into_sessions(lines: List[LogLine]) -> List[List[LogLine]]:
    """Split log lines into per-boot sessions."""
    sessions = []
    current  = []
    for line in lines:
        if line.is_session_start() and current:
            sessions.append(current)
            current = []
        current.append(line)
    if current:
        sessions.append(current)
    return sessions


# ─── Colour helpers ──────────────────────────────────────────────────────────

USE_COLOUR = sys.stdout.isatty() and sys.platform != 'win32'
RESET  = '\033[0m'  if USE_COLOUR else ''
BOLD   = '\033[1m'  if USE_COLOUR else ''
RED    = '\033[31m' if USE_COLOUR else ''
YELLOW = '\033[33m' if USE_COLOUR else ''
GREEN  = '\033[32m' if USE_COLOUR else ''
CYAN   = '\033[36m' if USE_COLOUR else ''
DIM    = '\033[2m'  if USE_COLOUR else ''

def col_level(level: str) -> str:
    if level == 'ERR':  return RED
    if level == 'WARN': return YELLOW
    if level == 'INFO': return ''
    return DIM

def fmt_ms(ms: int) -> str:
    """Format milliseconds as mm:ss.mmm"""
    secs  = ms // 1000
    mins  = secs // 60
    secs  = secs % 60
    millis = ms % 1000
    if mins > 0:
        return f'{mins:02d}:{secs:02d}.{millis:03d}'
    return f'{secs:2d}.{millis:03d}s'

def hbar(n: int, max_n: int, width: int = 40) -> str:
    """ASCII horizontal bar chart."""
    filled = int(round(n / max_n * width)) if max_n > 0 else 0
    return '█' * filled + '░' * (width - filled)


# ─── Command name lookup ─────────────────────────────────────────────────────

CMD_NAMES = {
    '0x00': 'SYNC',     '0x01': 'GETSTAT', '0x02': 'SETLOC',
    '0x03': 'PLAY',     '0x04': 'FORWARD', '0x05': 'BACKWARD',
    '0x06': 'READN',    '0x07': 'MOTORON', '0x08': 'STOP',
    '0x09': 'PAUSE',    '0x0A': 'RESET',   '0x0B': 'MUTE',
    '0x0C': 'UNMUTE',   '0x0D': 'SETFILTER','0x0E': 'SETMODE',
    '0x0F': 'GETPARAM', '0x10': 'GETLOCL', '0x11': 'GETLOCP',
    '0x13': 'GETTN',    '0x14': 'GETTD',   '0x15': 'SEEKL',
    '0x16': 'SEEKP',    '0x19': 'TEST',    '0x1A': 'ID',
    '0x1B': 'READS',    '0x1E': 'READTOC',
}


# ─── Report: summary ─────────────────────────────────────────────────────────

def report_summary(lines: List[LogLine]):
    sessions = split_into_sessions(lines)
    total_duration = lines[-1].ts_ms if lines else 0

    cmd_lines   = [l for l in lines if l.tag == 'CMD']
    seek_lines  = [l for l in lines if l.tag == 'SEEK' and 'start' in l.message]
    err_lines   = [l for l in lines if l.level in ('ERR', 'WARN')]
    sector_lines= [l for l in lines if l.tag == 'SECT']
    audio_lines = [l for l in lines if l.tag == 'AUDI']
    state_lines = [l for l in lines if l.tag == 'DRV']

    print(f'\n{BOLD}CD32 ODE Log Summary{RESET}')
    print('─' * 60)
    print(f'  File lines      : {len(lines):,}')
    print(f'  Boot sessions   : {len(sessions)}')
    print(f'  Total duration  : {fmt_ms(total_duration)}')
    print()
    print(f'  Commands issued : {len(cmd_lines):,}')
    print(f'  Seeks performed : {len(seek_lines):,}')
    print(f'  Sectors logged  : {len(sector_lines):,}')
    print(f'  Audio events    : {len(audio_lines):,}')
    print(f'  State changes   : {len(state_lines):,}')
    print(f'  Errors/warnings : {len(err_lines):,}')
    print()

    # Session table
    print(f'{BOLD}Sessions:{RESET}')
    for i, sess in enumerate(sessions, 1):
        duration = sess[-1].ts_ms - sess[0].ts_ms if len(sess) > 1 else 0
        cmds  = sum(1 for l in sess if l.tag == 'CMD')
        errs  = sum(1 for l in sess if l.level in ('ERR', 'WARN'))
        start = sess[0].ts_ms
        print(f'  Session {i:2d}: {fmt_ms(duration):>12s}  '
              f'{cmds:4d} cmds  {errs:3d} errors  '
              f'(boot at {start}ms)')


# ─── Report: commands ────────────────────────────────────────────────────────

def report_commands(lines: List[LogLine]):
    # Only count lines that are the command dispatch (contain opcode without →)
    dispatch_re = re.compile(r'(0x[0-9A-Fa-f]{2})\s+(\w+)(?:\s+params=\[([^\]]*)\])?$')
    response_re = re.compile(r'(0x[0-9A-Fa-f]{2})\s+(\w+)\s+→\s+\[([^\]]*)\]')

    cmd_counts   = Counter()
    cmd_errors   = Counter()
    cmd_with_err = set()

    for line in lines:
        if line.tag != 'CMD':
            continue
        m = response_re.search(line.message)
        if m:
            opcode = m.group(1).upper()
            resp   = m.group(3)
            # Check if response status byte has ERR bit set (bit 0)
            try:
                stat = int(resp.split(':')[0], 16)
                if stat & 0x01:
                    cmd_errors[opcode] += 1
            except ValueError:
                pass
            continue
        m = dispatch_re.search(line.message)
        if m:
            opcode = m.group(1).upper()
            cmd_counts[opcode] += 1

    if not cmd_counts:
        print('No command data found in log.')
        return

    max_count = max(cmd_counts.values())
    total     = sum(cmd_counts.values())

    print(f'\n{BOLD}Command Frequency ({total:,} total){RESET}')
    print('─' * 70)
    print(f'  {"Opcode":<8} {"Name":<12} {"Count":>7}  {"Errors":>6}  Bar')
    print('  ' + '─' * 66)

    for opcode, count in sorted(cmd_counts.items(), key=lambda x: -x[1]):
        name  = CMD_NAMES.get(opcode.lower(), '?')
        errs  = cmd_errors.get(opcode, 0)
        bar   = hbar(count, max_count, 28)
        ecol  = RED if errs else ''
        print(f'  {opcode:<8} {name:<12} {count:>7,}  '
              f'{ecol}{errs:>6}{RESET}  {CYAN}{bar}{RESET}')


# ─── Report: seeks ───────────────────────────────────────────────────────────

def report_seeks(lines: List[LogLine]):
    seek_start_re = re.compile(
        r'start from=\s*(\d+)\s+to=\s*(\d+)\s+est=(\d+)ms'
    )
    seek_done_re  = re.compile(r'complete LBA=(\d+)')

    pending: dict = {}   # lba → (ts_ms, from_lba, to_lba, est_ms)
    seeks = []

    for line in lines:
        if line.tag != 'SEEK':
            continue
        m = seek_start_re.search(line.message)
        if m:
            from_lba = int(m.group(1))
            to_lba   = int(m.group(2))
            est_ms   = int(m.group(3))
            pending[to_lba] = (line.ts_ms, from_lba, to_lba, est_ms)
            continue
        m = seek_done_re.search(line.message)
        if m:
            lba = int(m.group(1))
            if lba in pending:
                start_ts, from_lba, to_lba, est_ms = pending.pop(lba)
                actual_ms = line.ts_ms - start_ts
                dist = abs(to_lba - from_lba)
                seeks.append((start_ts, from_lba, to_lba, dist, est_ms, actual_ms))

    if not seeks:
        print('No seek data found.')
        return

    dists  = [s[3] for s in seeks]
    ests   = [s[4] for s in seeks]
    actuals = [s[5] for s in seeks]

    print(f'\n{BOLD}Seek Analysis ({len(seeks):,} seeks){RESET}')
    print('─' * 70)
    print(f'  Distance range  : {min(dists):,} – {max(dists):,} sectors')
    print(f'  Estimated range : {min(ests):,} – {max(ests):,} ms')
    print(f'  Actual range    : {min(actuals):,} – {max(actuals):,} ms')

    if actuals:
        avg_actual = sum(actuals) / len(actuals)
        avg_est    = sum(ests)    / len(ests)
        print(f'  Avg estimated   : {avg_est:.1f} ms')
        print(f'  Avg actual      : {avg_actual:.1f} ms')

    # Distribution: short (0-200ms), medium (200ms-1s), long (>1s)
    short  = sum(1 for a in actuals if a <  200)
    medium = sum(1 for a in actuals if 200 <= a < 1000)
    long_  = sum(1 for a in actuals if a >= 1000)
    total  = len(actuals)
    print(f'\n  Seek time distribution:')
    print(f'    Short  (<200ms)  : {short:4d}  {hbar(short,  total, 20)}')
    print(f'    Medium (200ms-1s): {medium:4d}  {hbar(medium, total, 20)}')
    print(f'    Long   (>1s)     : {long_:4d}  {hbar(long_,  total, 20)}')

    # Show 10 longest seeks
    print(f'\n  {BOLD}10 longest seeks:{RESET}')
    print(f'  {"Time":>10}  {"From LBA":>10}  {"To LBA":>10}  {"Dist":>8}  {"Est ms":>7}  {"Act ms":>7}')
    print('  ' + '─' * 62)
    for ts, frm, to, dist, est, act in sorted(seeks, key=lambda x: -x[5])[:10]:
        diff = act - est
        diff_col = RED if diff > 200 else (YELLOW if diff > 50 else GREEN)
        print(f'  {fmt_ms(ts):>10}  {frm:>10,}  {to:>10,}  {dist:>8,}  '
              f'{est:>7,}  {diff_col}{act:>7,}{RESET}')


# ─── Report: errors ──────────────────────────────────────────────────────────

def report_errors(lines: List[LogLine]):
    err_lines = [l for l in lines if l.level in ('ERR', 'WARN')]

    if not err_lines:
        print(f'{GREEN}No errors or warnings found.{RESET}')
        return

    # Group by message pattern (strip numbers for grouping)
    def normalise(msg: str) -> str:
        return re.sub(r'\d+', 'N', msg)

    by_pattern = defaultdict(list)
    for line in err_lines:
        by_pattern[normalise(line.message)].append(line)

    print(f'\n{BOLD}Errors and Warnings ({len(err_lines):,} total){RESET}')
    print('─' * 70)

    for pattern, group in sorted(by_pattern.items(), key=lambda x: -len(x[1])):
        level_col = RED if any(l.level == 'ERR' for l in group) else YELLOW
        print(f'\n  {level_col}[{group[0].level}] × {len(group)}{RESET}  {pattern}')
        # Show first 3 occurrences
        for line in group[:3]:
            print(f'    {DIM}{fmt_ms(line.ts_ms):>12}{RESET}  {line.message}')
        if len(group) > 3:
            print(f'    {DIM}... and {len(group)-3} more{RESET}')


# ─── Report: timeline ────────────────────────────────────────────────────────

def report_timeline(lines: List[LogLine], filters: List[str]):
    print(f'\n{BOLD}Event Timeline{RESET}'
          + (f' (filter: {", ".join(filters)})' if filters else ''))
    print('─' * 70)

    for line in lines:
        if filters and line.tag not in filters:
            continue
        lvl_col = col_level(line.level)
        print(f'  {DIM}{fmt_ms(line.ts_ms):>12}{RESET}  '
              f'{BOLD}{line.tag:<5}{RESET}  '
              f'{lvl_col}{line.message}{RESET}')


# ─── Report: sessions ────────────────────────────────────────────────────────

def report_sessions(lines: List[LogLine]):
    sessions = split_into_sessions(lines)
    print(f'\n{BOLD}Session Report ({len(sessions)} sessions){RESET}')
    print('─' * 70)

    for i, sess in enumerate(sessions, 1):
        duration = sess[-1].ts_ms if sess else 0
        cmds   = [l for l in sess if l.tag == 'CMD' and '→' not in l.message]
        seeks  = [l for l in sess if l.tag == 'SEEK' and 'start' in l.message]
        states = [l for l in sess if l.tag == 'DRV']
        errs   = [l for l in sess if l.level in ('ERR', 'WARN')]
        sects  = [l for l in sess if l.tag == 'SECT']

        # Find the image name from BOOT lines
        img_name = '(unknown)'
        for l in sess:
            if l.tag == 'BOOT' and '<- selected' in l.message:
                m = re.search(r'\] (.+?) <-', l.message)
                if m:
                    img_name = m.group(1).strip()
                break

        print(f'\n  {BOLD}Session {i}{RESET}  [{img_name}]')
        print(f'    Duration   : {fmt_ms(duration)}')
        print(f'    Commands   : {len(cmds):,}')
        print(f'    Seeks      : {len(seeks):,}')
        print(f'    State chgs : {len(states):,}')
        print(f'    Sectors    : {len(sects):,}')

        err_col = RED if errs else GREEN
        print(f'    Errors     : {err_col}{len(errs)}{RESET}')

        # Most-used commands this session
        cmd_counter = Counter()
        for l in cmds:
            m = re.search(r'(0x[0-9A-Fa-f]{2})\s+(\w+)', l.message)
            if m:
                cmd_counter[m.group(2)] += 1
        if cmd_counter:
            top3 = cmd_counter.most_common(3)
            tops = '  '.join(f'{n}×{c}' for n, c in top3)
            print(f'    Top cmds   : {tops}')


# ─── Report: audio ───────────────────────────────────────────────────────────

def report_audio(lines: List[LogLine]):
    audio_lines = [l for l in lines if l.tag == 'AUDI']
    underruns   = [l for l in audio_lines if 'underrun' in l.message.lower()]

    if not audio_lines:
        print('No audio events found in log.')
        return

    print(f'\n{BOLD}Audio Report ({len(audio_lines)} events){RESET}')
    print('─' * 60)
    print(f'  Total events  : {len(audio_lines)}')
    print(f'  Underruns     : '
          f'{"0  ✓" if not underruns else f"{RED}{len(underruns)}{RESET}"}')

    for line in audio_lines:
        lvl_col = RED if 'underrun' in line.message.lower() else ''
        print(f'  {DIM}{fmt_ms(line.ts_ms):>12}{RESET}  {lvl_col}{line.message}{RESET}')


# ─── Report: sectors ─────────────────────────────────────────────────────────

def report_sectors(lines: List[LogLine]):
    sect_lines = [l for l in lines if l.tag == 'SECT'
                  and '→ host' in l.message]

    if not sect_lines:
        print('No sector data found.  Enable log_sectors=1 in nebula32.cfg.')
        return

    lba_re = re.compile(r'LBA=(\d+)')
    lbas   = []
    for l in sect_lines:
        m = lba_re.search(l.message)
        if m:
            lbas.append((l.ts_ms, int(m.group(1))))

    if not lbas:
        return

    lba_vals = [lba for _, lba in lbas]
    print(f'\n{BOLD}Sector Delivery Report ({len(lbas):,} sectors){RESET}')
    print('─' * 60)
    print(f'  LBA range     : {min(lba_vals):,} – {max(lba_vals):,}')
    print(f'  Unique LBAs   : {len(set(lba_vals)):,}')

    # Inter-sector timing
    if len(lbas) > 1:
        gaps = []
        for j in range(1, len(lbas)):
            gap = lbas[j][0] - lbas[j-1][0]
            if 0 < gap < 100:  # Filter out pauses
                gaps.append(gap)
        if gaps:
            avg_gap = sum(gaps) / len(gaps)
            rate    = 1000 / avg_gap if avg_gap > 0 else 0
            print(f'  Avg gap       : {avg_gap:.1f} ms  ({rate:.1f} sectors/sec)')
            print(f'  Expected 1×   : 13.3 ms  (75/sec)')
            print(f'  Expected 2×   :  6.7 ms  (150/sec)')


# ─── Report: replay ──────────────────────────────────────────────────────────

def report_replay(lines: List[LogLine], speed: float):
    """Replay the log in real-time (or at a multiple of real-time)."""
    if not lines:
        return

    print(f'\n{BOLD}Replaying log at {speed:.1f}× speed  (Ctrl+C to stop){RESET}')
    print('─' * 70)

    t0_log = lines[0].ts_ms
    t0_real = time.monotonic()

    try:
        for line in lines:
            # Work out when this line should be displayed
            log_elapsed_s  = (line.ts_ms - t0_log) / 1000.0
            real_elapsed_s = log_elapsed_s / speed

            target = t0_real + real_elapsed_s
            now    = time.monotonic()
            if target > now:
                time.sleep(target - now)

            lvl_col = col_level(line.level)
            print(f'  {DIM}{fmt_ms(line.ts_ms):>12}{RESET}  '
                  f'{BOLD}{line.tag:<5}{RESET}  '
                  f'{lvl_col}{line.message}{RESET}')
    except KeyboardInterrupt:
        print('\n(stopped)')


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Analyse a CD32 ODE activity log (cd32_cd.log).')
    parser.add_argument('logfile', help='Path to cd32_cd.log')
    parser.add_argument('report', nargs='?', default='summary',
                        choices=['summary','commands','seeks','errors',
                                 'timeline','sessions','sectors','audio','replay'],
                        help='Report type (default: summary)')
    parser.add_argument('--filter', nargs='+', metavar='TAG',
                        help='For timeline: filter to these tags (e.g. CMD SEEK DRV)')
    parser.add_argument('--session', type=int, default=0,
                        help='Limit analysis to this boot session number (1-based, 0=all)')
    parser.add_argument('--speed', type=float, default=10.0,
                        help='Replay speed multiplier (default 10.0)')
    args = parser.parse_args()

    path = Path(args.logfile)
    if not path.exists():
        print(f'Error: {path} not found', file=sys.stderr)
        sys.exit(1)

    print(f'{DIM}Loading {path}...{RESET}', end='', flush=True)
    all_lines = load_log(path)
    print(f'\r{DIM}Loaded {len(all_lines):,} lines from {path}{RESET}')

    # Optionally filter to a single session
    if args.session > 0:
        sessions = split_into_sessions(all_lines)
        if args.session > len(sessions):
            print(f'Error: only {len(sessions)} session(s) found', file=sys.stderr)
            sys.exit(1)
        lines = sessions[args.session - 1]
        print(f'{DIM}Analysing session {args.session} of {len(sessions)}{RESET}')
    else:
        lines = all_lines

    report = args.report
    if   report == 'summary':   report_summary(lines)
    elif report == 'commands':  report_commands(lines)
    elif report == 'seeks':     report_seeks(lines)
    elif report == 'errors':    report_errors(lines)
    elif report == 'timeline':  report_timeline(lines, args.filter or [])
    elif report == 'sessions':  report_sessions(lines)
    elif report == 'sectors':   report_sectors(lines)
    elif report == 'audio':     report_audio(lines)
    elif report == 'replay':    report_replay(lines, args.speed)

    print()


if __name__ == '__main__':
    main()
