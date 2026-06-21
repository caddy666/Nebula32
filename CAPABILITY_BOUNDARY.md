# Nebula32 — CD-bus capability boundary

A one-page reference for "can the ODE do X?" questions. The ODE sits on the CD32
**CD drive connector**, speaking the COMMO command bus + the DA/subcode serial
streams. That position fixes what is cheap, what is precious, and what is
impossible — independent of how clever the firmware is.

## TL;DR — the rule

**Reads are free. Writes are precious. Some things are off-bus entirely.**

Decide any new idea in ten seconds:
- Read-mostly (serve content to the Amiga)? → **feasible**, often with no Amiga glue.
- Small, infrequent writes (Amiga → ODE)? → **feasible** via the covert seek channel.
- Write-heavy, or needs a bus we aren't on (floppy/IDE/SCSI)? → **don't**.

## Why the asymmetry exists — there are two pipes, not one

The CD connector carries **two physically separate data paths**, and they are
wildly different widths:

| Pipe | Direction | What it's for | Throughput |
|------|-----------|---------------|-----------|
| **DA / sector stream** (I2S + subcode, DMA'd by Akiko) | drive → Akiko only | audio + data sectors | **~176 KB/s @1×, ~353 KB/s @2×** |
| **COMMO command bus** (IF_CLK/DATA/DIR) | bidirectional, command-oriented | opcodes + status | **~100 kbps raw**; effective payload far less |

The wide pipe is **unidirectional, drive→Akiko**. There is no Akiko→drive *data*
pipe at all. So everything the Amiga sends us must ride the **narrow COMMO bus**,
and there is no "write sector" opcode — the only inbound signal we can repurpose is
*which LBA the Amiga seeks to*. That is the entire reason writes are ~2–3 orders of
magnitude slower than reads: **reads use the wide one-way pipe; writes are stuck on
the skinny command pipe, smuggled inside SEEK addresses.**

## What's physically on (and off) the connector

- **On:** DA_DATA/BCLK/LRCLK, C2PO/EMPH, SUB_*, M17SINE clock, COMMO 3-wire,
  ACTIVE/PASSIVE/DOOR/RESET. (See gpio_map.h.)
- **Off (cannot be reached from here):** the Paula/Gary **floppy** subsystem (MFM,
  disk-DMA, CIA step/index) and any **IDE/SCSI** hard-disk bus. Those live on the
  motherboard / SX-1/SX32 expansion, not the CD connector. No firmware trick adds them.

## Capability table (features discussed)

| Capability | Verdict | Why |
|---|---|---|
| Multi-disc carousel / hot-swap | ✅ built | command-channel only; reuses eject→insert |
| Redbook audio override (own music) | ✅ specced | read-side: substitute PCM the drive streams |
| Run WHDLoad/game content from CD (incl. HDF *contents* as ISO) | ✅ feasible | read-mostly; serve via virtual_disc, often **no Amiga glue** |
| Per-game saves (≤ few KB) | ✅ specced | small writes via covert seek channel |
| `SAVES:` mounted volume, **batch-on-close** | ✅ feasible | same channel; flush whole file on Close(), not per block |
| HDF mounted **read-only** (block-read device) | ⚠️ possible | needs a custom Amiga `.device`; reads cheap, but glue required |
| `SAVES:`/HDF as a **live writable block device** | ❌ impractical | every 512 B block = ~seconds over the seek channel |
| `vf0:`/`DF` floppy, IDE/SCSI hard disk | ❌ impossible | those buses aren't on the CD connector |

The unifying point: read-only bulk content is cheap and often glue-free; a writable
*trickle* is fine; a writable *firehose* (live FS) is not; off-bus devices never.

## Can the write channel be faster? — yes, a bit; no, not fundamentally

The covert write channel encodes save bytes into `SEEK` commands. Two things bound it:
**(a) seeks per second**, and **(b) bytes per seek.**

### (a) Seeks per second
A `SEEK` frame is ~5 wire bytes (opcode + 3 MSF + checksum); a status packet is ~16.
At ~100 kbps (~10 bits/byte framed):
- **Verified** (wait for status each seek): ~2 ms/seek → **~475 seeks/s**.
- **Fire-and-forget** (no per-seek status; rely on one final CRC + whole-save retry):
  ~0.5 ms/seek → **~2000 seeks/s**.
Real Amiga-handler loop overhead and any seek-settle delay only lower these.

### (b) Bytes per seek
- **Spec today: 1 byte/seek** (`offset = BASE + byte`, value 0–255). Simple, but the
  window must sit above the disc within MSF range, and uses only 8 of ~16.7 available bits.
- **2 bytes/seek (16 bits):** the magic window above a ≤74-min disc has ~109k codes
  (> 65536), so a single seek can carry a full 16-bit value. **~2× faster.**
- **3 bytes/seek (~24 bits):** read the SEEK's **raw p1/p2/p3 parameter bytes**
  *before* `msf_to_lba`, during an *armed* session (after a BEGIN marker, every SEEK
  is data until a length-delimited end). This drops the "window above disc" constraint
  entirely and carries 3 arbitrary bytes per seek. **~3× faster**, and simpler. This is
  the recommended future refinement to save-channel-wire-format.md.

### Practical ceiling
Combining fire-and-forget (~2000 seeks/s) with raw 3-byte symbols ≈ **~6 KB/s**.
Verified 1-byte mode ≈ **~0.5 KB/s**. So the channel spans roughly **0.5–6 KB/s**.

### Why it can never approach read speed
Even the optimistic ~6 KB/s is **~30–60× slower** than the read pipe (176–353 KB/s),
and that gap is structural, not an implementation detail:
1. Writes ride the **command bus** (~100 kbps), reads ride the **sector stream**
   (~1.4–2.8 Mbps). Different physical pipes, ~20× raw bandwidth apart.
2. Writes carry only a few **bytes per command** (smuggled in an address), with
   per-command framing/handshake overhead; reads stream **2352 bytes per sector**
   back-to-back with DMA.
3. The wide pipe is **drive→Akiko only** — it cannot be turned around to accept Amiga
   writes. There is no faster inbound path to switch to.

So: tune the write channel 3–10× with smarter encoding and fire-and-forget, and that
comfortably covers **game saves (KB)**. It will **never** cover **disk images (MB)** —
a 2 MB HDF write at 6 KB/s is ~5½ minutes, and a live FS does many such writes. That
is why "saves yes, writable disks no" is a hard line, not a TODO.

## Decision rule (pin this above your desk)

> Inbound from the Amiga = the skinny command pipe. Budget it in **kilobytes, written
> rarely**. Outbound to the Amiga = the wide sector pipe. Spend it freely. Anything
> needing a floppy/IDE/SCSI bus isn't on this connector at all.

## References
- Wire format: `~/.claude/plans/save-channel-wire-format.md`
- Save-disk plan: `~/.claude/plans/adf-save-disks.md`
- Redbook override: `~/.claude/plans/redbook-override-spec.md`
- Carousel: `~/.claude/plans/multi-disc-carousel-spec.md`
