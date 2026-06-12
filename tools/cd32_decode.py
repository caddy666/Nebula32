#!/usr/bin/env python3
"""
cd32_decode.py — Decode CD32 COMMO protocol from logic-analyzer CSV captures.

Reads IF_CLK / IF_DATA transitions and reconstructs COMMO bytes (LSB-first,
sampled on CLK rising edge).  Groups bytes into packets and labels each as a
HOST→DRIVE command or a DRIVE→HOST status/Q-channel response.

Usage:
  python3 cd32_decode.py [FILE] [OPTIONS]

  FILE defaults to tests/cd32_logic_replay_bundle/sample_digital.csv
       (relative to the Nebula32 project root, not the tools/ dir)

Options:
  --offset BYTES   Byte offset into the CSV file to start from  [default: 0]
  --rows   N       Max CSV rows to process                      [default: 500000]
  --no-color       Disable ANSI colours (for piping to a file)
  --commo-only     Hide raw/unclassified bytes
  --raw            Also show every decoded byte before packet grouping

Pipe to 'less' for scrollable navigation:
  python3 tools/cd32_decode.py | less -R
  python3 tools/cd32_decode.py digital.csv --offset 520000000 --rows 200000 | less -R

Signal columns in CSV (after the ISO timestamp):
  sig[0]=IF_CLK  sig[1]=IF_DATA  sig[2]=DA_LRCLK  sig[3]=DA_DATA
  sig[4]=DA_BCLK sig[5]=SUB_WFCLK sig[6]=SUB_SCOR  sig[7]=SUB_DATA

Protocol facts baked in from firmware sources:
  commo.pio:       bits are LSB-first, sampled on CLK rising edge
  commo.c:         cmd checksum = ~(sum of opcode+params), i.e. (sum+chk)==0xFF
  commo_bridge.c:  STATUS packet = 15 bytes + additive checksum = 16 wire bytes
                   Q-channel pkt = 15 bytes + additive checksum = 16 wire bytes
  commo.h:         STATUS_PACKET_LENGTH=15, Q_PACKET_LENGTH=15

Chinon legacy-drive frames (original CHINON O-658-2 drive, real captures only —
the Pico ODE never emits these):
  0x27 <status>            frame header + status (0x38=OK, 0xD8=focus error,
                           0xE0=sled error, 0xF8=no disc)
  0x27 0x01 <ASCII id>     drive identification string ("CHINON  O-658-2 24...")
  Note: 0x27+0xD8 == 0xFF, which coincidentally satisfies the Pico ODE additive
  checksum — these frames cannot be rejected by checksum alone.
"""

import sys
import os
import argparse
import textwrap

# ---------------------------------------------------------------------------
# Column detection — headers vary across capture sessions
# ---------------------------------------------------------------------------
# Required signals; decoder cannot function without these two.
_REQUIRED_SIGS = ("IF_CLK", "IF_DATA")
# Optional signals; logged when present but not used for packet decoding.
_OPTIONAL_SIGS = ("IF_DIR",)

# ---------------------------------------------------------------------------
# COMMO command length table (indexed by opcode & 0x0F)
# Matches command_length_table[] in upstream/core/commo.c.
# Length = total bytes including opcode but NOT including the trailing checksum.
# ---------------------------------------------------------------------------
CMD_LENGTH_TABLE = [1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1]

# (name, param_names)
# param_names only lists the semantically meaningful ones; extra bytes show as pN.
OPCODES = {
    0x00: ("TRAY_OUT",            []),
    0x01: ("TRAY_IN",             ["p1"]),
    0x02: ("START_UP",            []),
    0x03: ("STOP",                []),
    0x04: ("PLAY_TRACK",          ["track_bcd"]),   # 11 params total
    0x05: ("PAUSE_ON",            ["p1"]),
    0x06: ("PAUSE_OFF",           []),
    0x07: ("SEEK",                []),              # wire: 0 params; handler uses MSF from separate channel
    0x08: ("READ_TOC",            ["mm", "ss", "ff"]),
    0x09: ("READ_SUBCODE",        []),
    0x0A: ("SINGLE_SPEED",        []),
    0x0B: ("DOUBLE_SPEED",        []),
    0x0C: ("SET_VOLUME",          []),
    0x0D: ("JUMP_TRACKS",         ["p1"]),
    0x0E: ("ENTER_SERVICE_MODE",  []),
    0x0F: ("ENTER_NORMAL_MODE",   []),
    # Service-mode opcodes
    0x10: ("LASER_ON",            []),
    0x11: ("LASER_OFF",           ["p1"]),
    0x12: ("FOCUS_ON",            []),
    0x13: ("FOCUS_OFF",           []),
    0x14: ("SPINDLE_MOTOR_ON",    ["p1"]),
    0x15: ("SPINDLE_MOTOR_OFF",   ["p1"]),
    0x16: ("RADIAL_ON",           []),
    0x17: ("RADIAL_OFF",          []),
    0x18: ("MOVE_SLEDGE",         ["pos_hi", "pos_mid", "pos_lo"]),
    0x19: ("JUMP_GROOVES",        []),
    0x1A: ("WRITE_CD6",           []),
    0x1B: ("WRITE_DSIC2",         []),
    0x1C: ("READ_DSIC2",          []),
}

# Status bits (DRIVE_STATUS_* from include/cd_types.h)
DRIVE_STATUS_BUSY  = 0x80
DRIVE_STATUS_DRQ   = 0x20
DRIVE_STATUS_DISC  = 0x04
DRIVE_STATUS_ERROR = 0x01

# COMMO player status byte values (from upstream/include/commo.h)
PLAYER_STATUS = {0x00: "READY_OK", 0x01: "BUSY", 0x02: "READY_ERR"}

STATUS_PACKET_LEN = 15   # excludes trailing additive checksum
Q_PACKET_LEN      = 15   # excludes trailing additive checksum

# Chinon legacy-drive frame constants (original CHINON O-658-2 drive).
# Status byte hierarchy from akiko.cpp / CD32 ROM analysis.
CHINON_FRAME_HDR = 0x27
CHINON_STATUS = {
    0x38: ("OK",        "drive ready — focus locked, tracking"),
    0xD8: ("FOCUS_ERR", "focus failure / sync-pattern timeout"),
    0xE0: ("SLED_ERR",  "sled error"),
    0xF8: ("NO_DISC",   "no disc"),
}
CHINON_ID_MIN =  4   # min printable chars to classify as an ID string
CHINON_ID_MAX = 32   # max ID chars consumed (must fit under MAX_PENDING+2)

# ---------------------------------------------------------------------------
# ANSI colour helpers
# ---------------------------------------------------------------------------
_USE_COLOR = True

class C:
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    RED     = "\033[31m"
    GREEN   = "\033[32m"
    YELLOW  = "\033[33m"
    BLUE    = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN    = "\033[36m"

def col(s, *codes):
    if not _USE_COLOR:
        return s
    return "".join(codes) + s + C.RESET

# ---------------------------------------------------------------------------
# CSV column detection and row parsing
# ---------------------------------------------------------------------------
def detect_columns(header_line):
    """
    Parse the CSV header and return a column-index map.

    Returns a dict:
      "clk"  : int   — IF_CLK column index (0-based, after timestamp)
      "dat"  : int   — IF_DATA column index
      "dir"  : int|None — IF_DIR column index, or None if absent
      "names": list  — all signal names (for display)

    Raises SystemExit if IF_CLK or IF_DATA are not found.
    """
    names = [c.strip() for c in header_line.strip().split(",")[1:]]  # skip timestamp

    def find(name):
        try:
            return names.index(name)
        except ValueError:
            return None

    clk = find("IF_CLK")
    dat = find("IF_DATA")

    missing = [s for s in _REQUIRED_SIGS if find(s) is None]
    if missing:
        print("Error: required signal(s) not found in CSV header: %s" % missing,
              file=sys.stderr)
        print("Header signals: %s" % names, file=sys.stderr)
        sys.exit(1)

    return {"clk": clk, "dat": dat, "dir": find("IF_DIR"), "names": names}


def parse_row(line, cols):
    """
    Return (ts_str, clk, dat, dir_or_None) or None for header/bad rows.
    cols is the dict returned by detect_columns().
    """
    line = line.rstrip("\n")
    try:
        comma = line.index(",")
    except ValueError:
        return None
    ts    = line[:comma]
    parts = line[comma + 1:].split(",")

    max_idx = max(cols["clk"], cols["dat"])
    if cols["dir"] is not None:
        max_idx = max(max_idx, cols["dir"])
    if len(parts) <= max_idx:
        return None

    try:
        clk  = int(parts[cols["clk"]])
        dat  = int(parts[cols["dat"]])
        dir_ = int(parts[cols["dir"]]) if cols["dir"] is not None else None
        return ts, clk, dat, dir_
    except (ValueError, IndexError):
        return None  # header row or malformed


def ts_short(ts):
    """Trim ISO timestamp to HH:MM:SS.ffffff for display."""
    try:
        return ts[11:26]
    except Exception:
        return ts

# ---------------------------------------------------------------------------
# Bit/byte decoder  (IF_CLK / IF_DATA → bytes, LSB-first)
# ---------------------------------------------------------------------------
class ByteDecoder:
    """
    Watches IF_CLK and IF_DATA transitions.

    Protocol (from commo.pio, two state machines):
      HOST→DRIVE (RX SM): DATA falls 1→0 while CLK is idle-high (start bit),
        then host pulses CLK 8 times; drive samples DATA on each CLK rising edge.
      DRIVE→HOST (TX SM): drive takes CLK immediately (CLK falls from idle-high),
        sets DATA to each bit, raises CLK for host to sample — no separate start bit.

    Direction inference from idle state (CLK=1, DATA=1):
      - DATA falls first → HOST→DRIVE byte (start bit detected)
      - CLK falls first  → DRIVE→HOST byte (drive took CLK)

    Bits are LSB-first in both directions; sampled on CLK rising edge.
    Yields completed bytes as (ts_str, byte_value) via .drain().
    """
    def __init__(self):
        self.clk_prev   = None  # initialised from first real row
        self.dat_prev   = None
        self.bus_idle   = False  # True when CLK=1 and DATA=1
        self.collecting = False
        self.bits       = []
        self.start_ts   = ""
        self.ready      = []

    def feed(self, ts, clk, dat):
        if self.clk_prev is None:
            self.clk_prev  = clk
            self.dat_prev  = dat
            self.bus_idle  = (clk == 1 and dat == 1)
            return

        dat_fell = (dat == 0 and self.dat_prev == 1)
        clk_rose = (clk == 1 and self.clk_prev == 0)
        clk_fell = (clk == 0 and self.clk_prev == 1)

        if not self.collecting:
            if self.bus_idle:
                if dat_fell:
                    # HOST→DRIVE: DATA start-bit while CLK idle
                    self.collecting = True
                    self.bits       = []
                    self.start_ts   = ts
                elif clk_fell:
                    # DRIVE→HOST: drive seized CLK, no start-bit
                    self.collecting = True
                    self.bits       = []
                    self.start_ts   = ts
        else:
            if clk_rose:
                self.bits.append(dat)
                if len(self.bits) == 8:
                    b = 0
                    for i, bit in enumerate(self.bits):
                        b |= (bit << i)
                    self.ready.append((self.start_ts, b))
                    self.collecting = False
                    self.bits       = []

        self.bus_idle  = (clk == 1 and dat == 1)
        self.clk_prev  = clk
        self.dat_prev  = dat

    def drain(self):
        out = self.ready[:]
        self.ready = []
        return out

# ---------------------------------------------------------------------------
# Packet decoder  (byte stream → classified packets)
# ---------------------------------------------------------------------------
def _additive_ok(chunk_bytes):
    """True if the last byte equals ~(sum of all preceding bytes) mod 256."""
    if len(chunk_bytes) < 2:
        return False
    total = sum(chunk_bytes[:-1]) & 0xFF
    return (total + chunk_bytes[-1]) & 0xFF == 0xFF

def _try_cmd(buf):
    """
    Try to parse buf as a HOST→DRIVE command.
    buf is a list of (ts, byte).
    Returns (n_consumed, packet_dict) or (0, None).
    We try the canonical length from CMD_LENGTH_TABLE, but also 1, 2, 4 byte
    variants so we catch commands where the table may differ from reality.
    """
    if not buf:
        return 0, None
    ts0, b0 = buf[0]
    if b0 > 0x1C:
        return 0, None
    canonical = CMD_LENGTH_TABLE[b0 & 0x0F]
    # Try lengths: canonical first, then 1, 2, 4 (for edge cases like SEEK)
    attempts = [canonical]
    for alt in (1, 2, 4):
        if alt != canonical:
            attempts.append(alt)
    for cmd_len in attempts:
        total = cmd_len + 1   # +1 for additive checksum
        if len(buf) < total:
            continue
        chunk   = buf[:total]
        raw_b   = [b for _, b in chunk]
        if _additive_ok(raw_b):
            params = raw_b[1:-1]
            return total, _build_cmd_packet(ts0, b0, params, raw_b)
    return 0, None

def _build_cmd_packet(ts, opc, params, raw_b):
    name, pnames = OPCODES.get(opc, ("UNK_0x%02X" % opc, []))
    parts = []

    if opc == 0x04 and params:                  # PLAY_TRACK — BCD track
        bcd = params[0]
        tr  = (bcd >> 4) * 10 + (bcd & 0x0F)
        parts.append("track=%02d(BCD=%02X)" % (tr, bcd))
        for i, p in enumerate(params[1:], 2):
            parts.append("p%d=0x%02X" % (i, p))
    elif opc in (0x07, 0x08) and len(params) >= 3:  # SEEK / READ_TOC — MSF
        mm, ss, ff = params[0], params[1], params[2]
        parts.append("MSF=%02X:%02X:%02X" % (mm, ss, ff))
    elif opc == 0x0D and params:                # JUMP_TRACKS — signed 8-bit delta
        delta = params[0] if params[0] < 128 else params[0] - 256
        parts.append("delta=%+d(0x%02X)" % (delta, params[0]))
    else:
        for i, p in enumerate(params):
            label = pnames[i] if i < len(pnames) else ("p%d" % (i + 1))
            parts.append("%s=0x%02X" % (label, p))

    return {
        "type":      "CMD",
        "ts":        ts,
        "opc":       opc,
        "name":      name,
        "params":    params,
        "param_str": "  ".join(parts),
        "raw":       " ".join("%02X" % b for b in raw_b),
    }

def _try_status(buf):
    """
    Try to parse buf as a DRIVE→HOST STATUS packet (starts with 0x00).
    Wire format: 15 data bytes + 1 additive checksum = 16 bytes total.
    """
    total = STATUS_PACKET_LEN + 1
    if len(buf) < total:
        return 0, None
    chunk  = buf[:total]
    raw_b  = [b for _, b in chunk]
    if raw_b[0] != 0x00:
        return 0, None
    if not _additive_ok(raw_b):
        return 0, None
    # Sanity: player status byte must be 0,1, or 2
    if raw_b[1] > 2:
        return 0, None
    ts       = chunk[0][0]
    pstatus  = PLAYER_STATUS.get(raw_b[1], "0x%02X" % raw_b[1])
    cxd_stat = raw_b[2]
    echo     = raw_b[3]
    flags    = []
    if cxd_stat & DRIVE_STATUS_BUSY:  flags.append("BUSY")
    if cxd_stat & DRIVE_STATUS_DRQ:   flags.append("DRQ")
    if cxd_stat & DRIVE_STATUS_DISC:  flags.append("DISC")
    if cxd_stat & DRIVE_STATUS_ERROR: flags.append("ERR")
    echo_name = OPCODES.get(echo, (None,))[0] if echo in OPCODES else ("0x%02X" % echo)
    return total, {
        "type":      "STATUS",
        "ts":        ts,
        "pstatus":   pstatus,
        "cxd":       cxd_stat,
        "flags":     flags,
        "echo":      echo,
        "echo_name": echo_name,
        "raw":       " ".join("%02X" % b for b in raw_b),
    }

def _try_qchan(buf):
    """
    Try to parse buf as a DRIVE→HOST Q-channel packet (starts with 0x01).
    Wire format: 15 data bytes + 1 additive checksum = 16 bytes total.
    """
    total = Q_PACKET_LEN + 1
    if len(buf) < total:
        return 0, None
    chunk = buf[:total]
    raw_b = [b for _, b in chunk]
    if raw_b[0] != 0x01:
        return 0, None
    if not _additive_ok(raw_b):
        return 0, None
    ts    = chunk[0][0]
    qdata = raw_b[1:13]   # 12 bytes of Q-channel subcode
    ctrl  = (qdata[0] >> 4) & 0x0F
    track = qdata[1]
    idx   = qdata[2]
    rm, rs, rf = qdata[3], qdata[4], qdata[5]
    am, as_, af = qdata[7], qdata[8], qdata[9]
    tr_dec  = (track >> 4) * 10 + (track & 0x0F)
    idx_dec = (idx   >> 4) * 10 + (idx   & 0x0F)
    mode    = "DATA" if (ctrl & 0x04) else "AUDIO"
    q_str   = "trk=%02d idx=%02d  rel=%02X:%02X:%02X  abs=%02X:%02X:%02X  %s" % (
               tr_dec, idx_dec, rm, rs, rf, am, as_, af, mode)
    return total, {
        "type":  "QCHAN",
        "ts":    ts,
        "q_str": q_str,
        "raw":   " ".join("%02X" % b for b in raw_b),
    }

def _try_chinon(buf):
    """
    Try to parse buf as a Chinon legacy-drive frame (0x27 header).
    Forms:
      27 <status>            2 bytes; status must be in CHINON_STATUS
      27 01 <ASCII id>       variable; ID run ends at the first non-printable
                             byte (typically the next host command's opcode)
                             or at CHINON_ID_MAX chars
    Returns (n_consumed, pkt) or (0, None).  (0, None) with a 0x27 head byte
    also means "wait for more bytes" while an ID string is still arriving.
    """
    if not buf:
        return 0, None
    ts0, b0 = buf[0]
    if b0 != CHINON_FRAME_HDR:
        return 0, None
    if len(buf) < 2:
        return 0, None   # wait for the status / 0x01 byte
    b1 = buf[1][1]

    if b1 in CHINON_STATUS:
        name, desc = CHINON_STATUS[b1]
        return 2, {
            "type":        "CHINON",
            "ts":          ts0,
            "status":      b1,
            "status_name": name,
            "desc":        desc,
            "id_str":      None,
            "raw":         "%02X %02X" % (b0, b1),
        }

    if b1 == 0x01:
        chars      = []
        terminated = False
        for _, b in buf[2:]:
            if 0x20 <= b <= 0x7E and len(chars) < CHINON_ID_MAX:
                chars.append(b)
            else:
                terminated = True
                break
        if not terminated and len(chars) < CHINON_ID_MAX:
            return 0, None   # ID string may still be arriving
        if len(chars) < CHINON_ID_MIN:
            return 0, None   # too short — let the raw-byte fallback handle it
        total = 2 + len(chars)
        raw_b = [b for _, b in buf[:total]]
        return total, {
            "type":        "CHINON",
            "ts":          ts0,
            "status":      0x01,
            "status_name": "ID",
            "desc":        "drive identification",
            "id_str":      "".join(chr(c) for c in chars),
            "raw":         " ".join("%02X" % b for b in raw_b),
        }

    return 0, None

class PacketDecoder:
    """
    Accumulates bytes and tries to classify them into packets.

    Classification order for the first byte of a group:
      0x00 alone (+ checksum=0xFF)  → TRAY_OUT command (2 bytes)
      0x00 + 15 more                → STATUS response  (16 bytes)
      0x01 + 15 more                → Q-channel        (16 bytes)
      0x00..0x1C + cmd_length       → HOST command
      0x27 + status / 0x01+ASCII    → Chinon legacy-drive frame
      anything else                 → raw BYTE after MAX_PENDING threshold
    """
    # Emit raw byte if buffer grows beyond this.  Must exceed the longest
    # waiting classifier: a Chinon ID frame is 2 + CHINON_ID_MAX = 34 bytes,
    # otherwise the fallback would pop the 0x27 header mid-frame.
    MAX_PENDING = 40

    def __init__(self):
        self.buf     = []   # list of (ts, byte)
        self.packets = []

    def feed(self, ts, b):
        self.buf.append((ts, b))
        self._classify()

    def _classify(self):
        while self.buf:
            consumed, pkt = self._try_all()
            if pkt:
                self.packets.append(pkt)
                self.buf = self.buf[consumed:]
            elif len(self.buf) >= self.MAX_PENDING:
                ts, b = self.buf.pop(0)
                self.packets.append({"type": "BYTE", "ts": ts, "value": b})
            else:
                break

    def _try_all(self):
        if not self.buf:
            return 0, None
        _, b0 = self.buf[0]

        # Try command first (handles opc=0x00 as TRAY_OUT via short checksum)
        n, pkt = _try_cmd(self.buf)
        if pkt:
            return n, pkt

        # Try STATUS response
        n, pkt = _try_status(self.buf)
        if pkt:
            return n, pkt

        # Try Q-channel response
        n, pkt = _try_qchan(self.buf)
        if pkt:
            return n, pkt

        # Try Chinon legacy-drive frame (0x27 header, original drive captures)
        n, pkt = _try_chinon(self.buf)
        if pkt:
            return n, pkt

        return 0, None

    def flush(self):
        for ts, b in self.buf:
            self.packets.append({"type": "BYTE", "ts": ts, "value": b})
        self.buf = []

    def drain(self):
        out = self.packets[:]
        self.packets = []
        return out

# ---------------------------------------------------------------------------
# Output formatter
# ---------------------------------------------------------------------------
def format_packet(pkt, show_raw=False):
    t    = pkt["type"]
    time = ts_short(pkt["ts"])

    if t == "CMD":
        ps   = pkt["param_str"]
        line = "[%s]  HOST→DRIVE  %-20s  opc=0x%02X  %s" % (
               time, pkt["name"], pkt["opc"], ps)
        out  = col(line, C.BOLD, C.YELLOW)
        if show_raw:
            out += "\n" + col("              raw: " + pkt["raw"], C.DIM)
        return out

    if t == "STATUS":
        flags = "+".join(pkt["flags"]) if pkt["flags"] else "—"
        line  = "[%s]  DRIVE→HOST  STATUS                cxd=0x%02X [%-14s]  echo=%-20s  player=%s" % (
                time, pkt["cxd"], flags, pkt["echo_name"], pkt["pstatus"])
        if "ERR" in pkt["flags"]:
            color = C.RED
        elif "DRQ" in pkt["flags"]:
            color = C.CYAN
        elif "BUSY" in pkt["flags"]:
            color = C.MAGENTA
        else:
            color = C.GREEN
        out = col(line, color)
        if show_raw:
            out += "\n" + col("              raw: " + pkt["raw"], C.DIM)
        return out

    if t == "QCHAN":
        line = "[%s]  DRIVE→HOST  QCHANNEL              %s" % (time, pkt["q_str"])
        out  = col(line, C.BLUE)
        if show_raw:
            out += "\n" + col("              raw: " + pkt["raw"], C.DIM)
        return out

    if t == "CHINON":
        if pkt["id_str"] is not None:
            line  = "[%s]  DRIVE→HOST  CHINON ID             \"%s\"" % (time, pkt["id_str"])
            color = C.MAGENTA
        else:
            line  = "[%s]  DRIVE→HOST  CHINON FRAME          status=0x%02X [%s]  %s" % (
                    time, pkt["status"], pkt["status_name"], pkt["desc"])
            color = C.GREEN if pkt["status_name"] == "OK" else C.RED
        out = col(line, color)
        if show_raw:
            out += "\n" + col("              raw: " + pkt["raw"], C.DIM)
        return out

    if t == "BYTE":
        b    = pkt["value"]
        desc = OPCODES.get(b, (None,))[0]
        hint = (" → could be start of %s cmd" % desc) if desc else ""
        line = "[%s]  ??          BYTE                  0x%02X%s" % (time, b, hint)
        return col(line, C.DIM)

    return ""

# ---------------------------------------------------------------------------
# Opcode reference table (printed with --help-opcodes)
# ---------------------------------------------------------------------------
OPCODE_TABLE = """
Normal-mode opcodes (host → drive):
  0x00  TRAY_OUT             0 params   Open tray / eject
  0x01  TRAY_IN              1 param    Close tray / load disc
  0x02  START_UP             0 params   Spin up, read lead-in, build TOC
  0x03  STOP                 0 params   Stop motor
  0x04  PLAY_TRACK          11 params   Play from BCD track number (p1)
  0x05  PAUSE_ON             1 param    Pause playback
  0x06  PAUSE_OFF            0 params   Resume playback
  0x07  SEEK                 0 params   Seek (MSF conveyed separately)
  0x08  READ_TOC             3 params   Read TOC  (mm ss ff in BCD)
  0x09  READ_SUBCODE         0 params   Request Q-channel subcode packet
  0x0A  SINGLE_SPEED         0 params   1× playback speed
  0x0B  DOUBLE_SPEED         0 params   2× playback speed
  0x0C  SET_VOLUME           0 params   Set audio volume
  0x0D  JUMP_TRACKS          1 param    Jump ±N tracks (signed)
  0x0E  ENTER_SERVICE_MODE   0 params   Switch to service opcode space
  0x0F  ENTER_NORMAL_MODE    0 params   Return to normal opcode space

Drive → host response packets (start byte):
  0x00  STATUS  — 15 bytes + checksum (16 total)
        byte[1]: player status  (0x00=READY_OK  0x01=BUSY  0x02=READY_ERR)
        byte[2]: CXD status bits (0x80=BUSY 0x20=DRQ 0x04=DISC 0x01=ERR)
        byte[3]: echo of last command opcode
  0x01  Q-CHANNEL — 15 bytes + checksum (16 total)
        bytes[1..12]: 12-byte subcode Q data
        field layout: ctrl/adr track index rel_MSF zero abs_MSF CRC16

Chinon legacy-drive frames (0x27 header — original CHINON O-658-2 drive only;
the Pico ODE never emits these):
  0x27 0x38  OK         drive ready — focus locked, tracking
  0x27 0xD8  FOCUS_ERR  focus failure / sync-pattern timeout
  0x27 0xE0  SLED_ERR   sled error
  0x27 0xF8  NO_DISC    no disc
  0x27 0x01 <ASCII>     drive identification string ("CHINON  O-658-2 24...")
  Gotcha: 0x27+0xD8 == 0xFF, coincidentally a valid additive checksum pair.

Checksum (host commands and drive responses alike):
  Additive complement: sum(all_data_bytes) + checksum_byte == 0xFF  (mod 256)
"""

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    global _USE_COLOR

    ap = argparse.ArgumentParser(
        prog="cd32_decode.py",
        description=__doc__.split("\n")[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
          Examples:
            # Decode default sample file, pipe to scrollable pager:
            python3 tools/cd32_decode.py | less -R

            # Decode 200k rows starting 500 MB into digital.csv:
            python3 tools/cd32_decode.py digital.csv --offset 520000000 --rows 200000 | less -R

            # Show only COMMO packets, no raw bytes, from Zool2 capture:
            python3 tools/cd32_decode.py zool2.csv --commo-only --rows 1000000 | less -R

            # Print opcodes reference table:
            python3 tools/cd32_decode.py --help-opcodes
        """),
    )
    ap.add_argument("file", nargs="?", help="CSV capture file (default: sample_digital.csv)")
    ap.add_argument("--offset",       type=int,  default=0,       metavar="BYTES",
                    help="Byte offset to start reading from")
    ap.add_argument("--rows",         type=int,  default=500_000, metavar="N",
                    help="Max CSV rows to process")
    ap.add_argument("--no-color",     action="store_true",
                    help="Disable ANSI colours")
    ap.add_argument("--commo-only",   action="store_true",
                    help="Hide unclassified raw bytes")
    ap.add_argument("--raw",          action="store_true",
                    help="Show raw hex after each decoded packet")
    ap.add_argument("--help-opcodes", action="store_true",
                    help="Print opcode / response reference and exit")
    args = ap.parse_args()

    if args.help_opcodes:
        print(OPCODE_TABLE)
        return

    if args.no_color or not sys.stdout.isatty():
        _USE_COLOR = False

    # Resolve default file path relative to the project root
    if args.file:
        csv_path = args.file
    else:
        here     = os.path.dirname(os.path.abspath(__file__))
        root     = os.path.dirname(here)   # tools/ → project root
        csv_path = os.path.join(root, "tests", "cd32_logic_replay_bundle",
                                "sample_digital.csv")

    # ── Banner printed after column detection (inside the open() block below) ──
    bar = "═" * 80

    byte_dec  = ByteDecoder()
    pkt_dec   = PacketDecoder()
    row_count = 0
    pkt_count = 0

    try:
        with open(csv_path, "r", errors="replace") as fh:
            # Always read the header from position 0 to detect column layout,
            # then seek to the requested offset.
            header = fh.readline()
            cols   = detect_columns(header)
            dir_present = cols["dir"] is not None

            # ── Banner ────────────────────────────────────────────────
            print(col(bar, C.DIM))
            print(col("  CD32 COMMO decoder", C.BOLD) + col("  │  " + csv_path, C.DIM))
            col_info = "IF_CLK=col%d  IF_DATA=col%d" % (cols["clk"], cols["dat"])
            if dir_present:
                col_info += "  IF_DIR=col%d" % cols["dir"]
            print(col("  " + col_info, C.DIM))
            print(col("  offset: %-12d  max rows: %d" % (args.offset, args.rows), C.DIM))
            print(col("  " + col("HOST→DRIVE", C.YELLOW, C.BOLD) +
                      col("   STATUS", C.GREEN) +
                      col("   QCHANNEL", C.BLUE) +
                      col("   STATUS+DRQ", C.CYAN) +
                      col("   ERR", C.RED) +
                      col("   CHINON ID", C.MAGENTA) +
                      col("   raw byte", C.DIM), C.DIM))
            print(col(bar, C.DIM))

            if args.offset > 0:
                fh.seek(args.offset)
                fh.readline()  # discard partial line at seek boundary

            for line in fh:
                if row_count >= args.rows:
                    break

                parsed = parse_row(line, cols)
                if parsed is None:
                    continue

                ts, clk, dat, dir_ = parsed
                row_count += 1

                byte_dec.feed(ts, clk, dat)

                for bts, bval in byte_dec.drain():
                    if args.raw:
                        print(col("[%s]  raw byte  0x%02X" % (ts_short(bts), bval), C.DIM))
                    pkt_dec.feed(bts, bval)

                for pkt in pkt_dec.drain():
                    if args.commo_only and pkt["type"] == "BYTE":
                        continue
                    line_out = format_packet(pkt, show_raw=args.raw)
                    if line_out:
                        print(line_out)
                        pkt_count += 1

    except KeyboardInterrupt:
        pass
    except FileNotFoundError:
        print("Error: file not found: " + csv_path, file=sys.stderr)
        print("Pass the CSV path as the first argument or run from the project root.", file=sys.stderr)
        sys.exit(1)

    # Flush remaining buffered bytes
    pkt_dec.flush()
    for pkt in pkt_dec.drain():
        if args.commo_only and pkt["type"] == "BYTE":
            continue
        line_out = format_packet(pkt, show_raw=args.raw)
        if line_out:
            print(line_out)
            pkt_count += 1

    print()
    print(col(bar, C.DIM))
    print(col("  Rows processed: %-10d  Packets decoded: %d" % (row_count, pkt_count), C.DIM))
    print(col(bar, C.DIM))


if __name__ == "__main__":
    main()
