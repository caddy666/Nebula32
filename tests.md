# Nebula32 ODE: Supplemental 50-Test Suite Specification

This document provides a technical blueprint for 50 additional test cases designed to fortify the Nebula32 firmware against structural edge cases, extreme hardware behaviors, media formatting anomalies, and concurrency conflicts on the RP2350 platform.

---

## 1. CD Audio (CDDA) & Subcode Generation (10 Tests)

These tests expand on the existing `Subcode`, `SubcodeClk`, and `QSubchannel` groups to ensure absolute timing compliance and frame-perfect subcode delivery to Akiko and the DAC.

* **Test 1 (`Subcode_PreEmphasisToggle`):** Verifies that the `DA_EMPH` pin (GPIO 4) toggles dynamically within 1 frame when switching from a track with pre-emphasis enabled to a track without it, mapping perfectly to the Q-channel control nibble changes.
* **Test 2 (`Subcode_Index00CountdownMonotonicity`):** Asserts that during a CUE sheet index 00 (pre-gap) countdown, the relative time fields (`R-MSF`) decrement strictly monotonically frame-by-frame until reaching exactly `00:00:00` at the index 01 boundary.
* **Test 3 (`Subcode_MultiIndexTransition`):** Validates subcode absolute time (`A-MSF`) progression and relative time tracking when a single track features sub-indexes beyond index 01 (e.g., Index 02, 03 for hidden milestones).
* **Test 4 (`Subcode_MaxTracksTOC`):** Validates `disc_build_toc_response` behavior under the Red Book absolute limit of 99 audio tracks, ensuring no buffer overflows occur in the stack-allocated status packet arrays.
* **Test 5 (`Subcode_LeadOutQChannel`):** Asserts that when entering the lead-out area (LBA ≥ total sectors), the Q-channel control nibble matches the final track's attributes while the track number byte switches strictly to `0xAA`.
* **Test 6 (`Subcode_ZeroLengthTrackGuard`):** Feeds a corrupt CUE descriptor containing an audio track of 0 frames. Verifies that the subcode generator skips or handles the track boundaries gracefully without causing a divide-by-zero or an infinite loop during runtime LBA lookups.
* **Test 7 (`Subcode_M17SineJitterTolerance`):** Simulates a software mock of a ±5% frequency drift on the `M17SINE` master clock reference (GPIO 9). Verifies that your subcode frame cadence calculation auto-trims safely without dropping below the target 75 Hz sector tick baseline.
* **Test 8 (`Subcode_PioFifoStallRecovery`):** Forcefully fills the subcode PIO TX FIFO without reading it to cause a total hardware block. Asserts that `subcode_push_to_pio()` clears the FIFO cleanly on the next sector synchronization pulse, throwing away orphaned data frames without locking Core 0.
* **Test 9 (`Subcode_ScorWfclkOverrun`):** Asserts that `SUB_SCOR` (GPIO 8) and `SUB_WFCLK` (GPIO 7) maintain their exact 1:98 pulse cadence ratio even when the system playback speed jumps to 2× mid-track.
* **Test 10 (`Subcode_DataAudioCtrlNibbleFlip`):** Validates that jumping across a mixed-mode disc boundary (from data Track 1 to audio Track 2) updates the Q-channel data/audio CTRL nibble instantly between `0x41` (data) and `0x01` (audio).

---

## 2. Host Bus, Protocol Fuzzing, & Wire Glitches (10 Tests)

These tests extend your `CommoProtocol` and `CommoFuzz` frameworks to stress the 3-wire physical interface and command processor.

* **Test 11 (`Commo_IfClkGlitchRejection`):** Simulates a single clock-line microsecond noise spike during an inactive COMMO phase. Asserts that the PIO RX program ignores pulses shorter than the minimal allowed hardware frame window.
* **Test 12 (`Commo_UnrecognizedOpcodeFuzz`):** Iterates through all undefined byte codes (`0x00` through `0xFF` excluding your 34 validated opcodes). Asserts that each returns a cleanly structured status response via `_build_status()` instead of triggering an internal system crash or dropping the communication line.
* **Test 13 (`Commo_DataSetupTimeViolation`):** Simulates an aggressive host that drives data (`IF_DATA`) less than 150 ns before the rising edge of `IF_CLK`. Verifies that if the packet checksum fails, the system triggers a `CMD_ERROR` and cleanly clears `last_command`.
* **Test 14 (`Commo_DirectionFlipRace`):** Simulates the host abruptly asserting an unexpected direction transition via `IF_DIR` (GPIO 46) mid-packet transmission. Asserts that the COMMO bridge safely tri-states its pins within the safe hardware margin to prevent physical bus contention.
* **Test 15 (`Commo_TxBlockedStrobeRecovery`):** Verifies that if a TX operation is blocked by a spurious data strobe on the bus line, the internal state machine cleanly timeouts after 5 ms, unblocks the core loop, and flags the error state.
* **Test 16 (`Commo_RapidFireDuplicateOpcodes`):** Rapidly fires identical `PLAY_TRACK_OPC` frames back-to-back within a microsecond window to mimic buggy Amiga game engine polling loops. Asserts that the command dispatcher processes the sequence cleanly without restarting the internal laser lens trajectory or audio DMA stream from zero.
* **Test 17 (`Commo_ResetLineBounce`):** Rapidly toggles the active-low `/RESET` pin (GPIO 14) over a 10 ms window to simulate mechanical bouncing or cold boot fluctuations. Verifies that the drive state settles cleanly into `IDLE` once the reset signal remains high and stable.
* **Test 18 (`Commo_InvalidBcdTrackSeek`):** Issues a `SEEK` or `PLAY_TRACK_OPC` payload with non-BCD values (e.g., track number `0x1A` or `0xFF`). Asserts that the parser rejects the input immediately with a clean error response, avoiding downstream lookups with out-of-bounds indices.
* **Test 19 (`Commo_StatusPacketChecksumInvariant`):** Loops over 1,000 synthesized drive states to verify that every 15-byte status response packet payload generated produces an additive checksum where the final 16-byte wire output sum equals exactly `0xFF`.
* **Test 20 (`Commo_PathA_BusySquelch`):** Validates that when Path A is marked active/busy, incoming command packets are completely squelched or deferred, ensuring core command parsing never breaks an ongoing higher-priority hardware sequence.

---

## 3. Storage Layer, Sector Cache, & Flash Failures (10 Tests)

These tests target the `SectorCache` and FatFS storage layer to expose edge cases during data starvation and I/O degradation.

* **Test 21 (`Cache_ExtremeLatencyStall`):** Introduces a simulated SD card latency spike of 2.5 seconds inside the mock read block loop. Asserts that `sector_cache_get` returns false gracefully and the audio output drops to clean digital silence instead of throwing a memory access fault or triggering an unhandled core exception.
* **Test 22 (`Cache_ConsecutiveReadFailures`):** Simulates an SDIO hard read failure that fails twice sequentially. Verifies that the slot is marked permanently invalid with `valid_bytes = 0` (sentinel miss) and does not trap the ring buffer in a perpetual, un-evictable retry spiral.
* **Test 23 (`Cache_FlushGenRaceCondition`):** Spawns concurrent threads where Core 0 executes an immediate `sector_cache_seek` (triggering a cache flush and incrementing `flush_gen`) while Core 1 is exactly mid-way through committing a slow, in-flight sector read. Asserts that the stale sector data is dropped and never overwrites the new target LBA.
* **Test 24 (`Cache_BoundaryWrapPrefetch`):** Requests an LBA address located exactly at the extreme end of a physical track image file. Asserts that the prefetch loop detects the file boundary cleanly and pads out the remaining cache ring slots with silence or returns an error instead of reading past the file handle bounds.
* **Test 25 (`Cache_CacheIsFullHoleState`):** Verifies that `sector_cache_is_full()` returns true when all 8 allocation slots are populated, and tests the "one-hole" condition to ensure a single invalid slot immediately allows prefetch allocation to resume.
* **Test 26 (`Flash_MulticoreLockoutErase`):** Focuses on settings saving in `src/config.c`. Validates that `multicore_lockout_start_blocking()` halts Core 1 execution flawlessly before executing an on-chip flash erase block, avoiding XIP flash bus data corruption or deadlocks.
* **Test 27 (`Flash_CorruptCrcRecovery`):** Modifies settings data stored in flash to contain an invalid CRC-32 checksum. Asserts that `src/config.c` rejects the payload on boot, self-recovers by wiping the corrupt region, and applies the default configuration cleanly.
* **Test 28 (`Sd_AlphabeticalStableSort`):** Seeds the simulated FatFS directory structure with mixed-case and non-sequential file entry paths. Asserts that `src/sd_card.c` uses `qsort` to enforce stable, deterministic alphabetical indexing across multiple boot cycles.
* **Test 29 (`Sd_YankCardDuringRead`):** Emulates physically removing the SD card mid-transaction. Asserts that the system drops back immediately to an unmapped "No Disc / Drive Error" status, clears any lingering cache pointers, and avoids a hard fault crash.
* **Test 30 (`FatFs_MultiPartitionMapping`):** Validates that `VolToPart[]` maps "0:/" strictly to Partition 1 (containing configuration and logging assets) and "1:/" directly to Partition 2 (the virtual ISO root directory traversal space).

---

## 4. Virtual ISO 9660 & Data Synthesis (10 Tests)

These tests expand on your `vdisc_tests` to explore filesystem anomalies, path limits, and structure formatting accuracy.

* **Test 31 (`Vdisc_MaxDirectoryDepth`):** Generates a nested directory tree deeper than the ISO 9660 standard maximum limit (8 levels). Asserts that the parser clamps or skips directories past the limit cleanly without crashing the LBA generation layout.
* **Test 32 (`Vdisc_FilenameTruncationAndClash`):** Places multiple files with long names (longer than 31 characters) sharing identical prefixes in the same directory path. Asserts that the virtual synthesis layer truncates names correctly and introduces numeric suffixing (e.g., `FILENAME01;1`, `FILENAME02;1`) to avoid directory index collisions.
* **Test 33 (`Vdisc_ZeroSizeFileHandling`):** Places an empty 0-byte file entry within the target directory. Asserts that `virtual_disc.c` maps it to a valid directory entry record with a length of zero, while assigning no data LBA blocks to it.
* **Test 34 (`Vdisc_PathTableEndianness`):** Scans the synthesized ISO layout to verify that the L-Path Table features strict Little-Endian 32-bit formatting for directory LBAs, while the M-Path Table features strict Big-Endian formatting.
* **Test 35 (`Vdisc_DirectorySectorPadding`):** Simulates a directory containing enough file records to overflow a single 2048-byte ISO sector. Asserts that the leftover bytes on the first sector are padded with zeros and the next file record starts exactly at the boundary of the next sector.
* **Test 36 (`Vdisc_SystemAreaZeros`):** Verifies that reading any LBA block between index 0 and 15 (the first 32 KB system area of the virtual ISO) returns exclusively zeroed data payloads.
* **Test 37 (`Vdisc_IllegalCharSubstitution`):** Seeds a filename containing invalid ISO characters (such as lowercase text or symbols like `#` or `@`). Asserts that the layout generator substitutes them with underscores (`_`) and appends the standard ISO version suffix ';1'.
* **Test 38 (`Vdisc_PvdValidation`):** Asserts that reading LBA 16 returns a Primary Volume Descriptor containing the valid "CD001" magic marker, volume descriptor type `0x01`, and the default volume identifier set to "NEBULA32".
* **Test 39 (`Vdisc_LargeFileMultiSector`):** Verifies that a simulated file larger than 1 MB spans across consecutive, sequential LBA blocks without overlapping neighboring entries or throwing off the total sector count.
* **Test 40 (`Vdisc_IntegrationClose`):** Confirms that executing `disc_close` safely frees the internal virtual disk pointers, unmounts the directory mapping structures, and leaves the virtual drive context clear for subsequent ISO/BIN file loads.

---

## 5. Media Parsers, Audio DMA, & Network Infrastructure (10 Tests)

These tests target the image parsing logic, audio stream serialization, and network handling within `src/webserver.c`.

* **Test 41 (`Parser_NrgV1UnalignedMemcpy`):** Focuses on the `.NRG` format parser. Validates that unaligned reads within older NRG v1 image layouts utilize `memcpy` alignment routines to avoid triggering a hardware alignment trap on the RP2350 platform.
* **Test 42 (`Parser_MdfSectorSizeZero`):** Passes a corrupted `.MDF` file header where the `sector_size` field is set to zero. Asserts that the parser layout drops out safely through its internal guards rather than triggering a division-by-zero error.
* **Test 43 (`Parser_NrgChunkSizeOverflow`):** Passes a malformed `.NRG` image file featuring an explicit chunk size field of zero or a `DAOX` length that is too short. Asserts that the internal parsing guards catch the anomaly and flag a clean format error.
* **Test 44 (`Da_DmaAbortOnTrackEnd`):** Simulates reaching the definitive end-of-disc boundary while an audio stream is actively playing. Asserts that the internal end-of-disc Interrupt Service Routine (ISR) calls `dma_channel_abort` immediately to cleanly halt the transfer loop.
* **Test 45 (`Da_I2sPolarityVerification`):** Inspects the raw bit states generated by `da_output.pio` to confirm that the word clock (`DA_LRCLK`) follows the correct standard polarity: driving low for Left-channel samples and high for Right-channel samples.
* **Test 46 (`Web_TcpRecvBeforeClose`):** Simulates an active HTTP client connection pool dropping requests rapidly. Asserts that `src/webserver.c` calls `tcp_recved` to clear the buffer window before invoking `tcp_close`, preventing network memory leaks or state desynchronization.
* **Test 47 (`Web_HcatOverflowGuard`):** Fuzzes the web server API routes with long parameter strings. Asserts that the inner string-concatenation routines trigger overflow protections, preventing memory corruption or buffer overflows.
* **Test 48 (`Web_PathTraversalGuard`):** Submits malicious URI strings containing parent directory traversal paths (such as `/api/../../etc/passwd` or `/covers/../config.cfg`). Asserts that the URI parsing logic intercepts the pattern and throws a `403 Forbidden` error response.
* **Test 49 (`Web_HtmlEscapeCharacters`):** Asserts that `html_escape()` safely replaces characters like `&`, `<`, `>`, `"`, and `'` with their proper HTML entity sequences, avoiding cross-site scripting vulnerabilities in the web dashboard.
* **Test 50 (`Logger_StateNamesBounds`):** Validates that the logging engine maps drive states accurately without encountering out-of-bounds array access errors, keeping the string reference pointer safely within array boundaries (bounds ≤7).
