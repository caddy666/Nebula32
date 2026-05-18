# Nebula32 ODE: Advanced Testing & Feature Expansion Roadmap

This document outlines advanced testing vectors and next-generation features tailored to the dual-core RP2350 (Pico 2) hardware architecture and verified Akiko/CXD signal pathways.

---

## 1. Deep-Fault & Edge-Case Testing Expansion

While the current test suite provides a comprehensive baseline for stable conditions, verifying firmware resilience under degraded hardware, hostile timing environments, and storage failures is crucial for long-term field stability.

### 1.1. Host-Side Timing & Bus Stress Testing
* **Aggressive Akiko DMA Desynchronization:** Extend the `AkikoDma` replica tests to introduce arbitrary host disruptions. 
    * *Test Case:* Simulate the Amiga abruptly asserting `/RESET` (GPIO 14) or halting a DMA transfer exactly mid-sector. Ensure Core 1's prefetch engine stalls gracefully without corrupting subsequent data frame sequences.
    * *Test Case:* Mock a runtime speed transition (1× to 2×) in the middle of an active sector stream, verifying that `da_output.c` updates both the PIO state machine divisor and subcode clock dividers atomically.
* **COMMO Bus Wire-Level Glitch Injection:** Create a hardware-fuzzer test targeting the 3-wire COMMO protocol (`IF_CLK`, `IF_DATA`, `IF_DIR`).
    * *Test Case:* Inject microsecond noise spikes during data phase execution to verify that `commo.pio` and `commo.c` drop the corrupted frame, trigger `CMD_ERROR`, clear `last_command`, and return to an unblocked state rather than falling into an infinite lock or stall.

### 1.2. Storage Layer & Media Fault Simulation
* **Stochastic SD Card Latency Degradation:** Implement a specialized wrapper inside `stress_sector_cache` that hooks the underlying SDIO read blocks.
    * *Test Case:* Simulate a degrading, slow SD card by injecting random latencies that spike from typical microsecond responses up to multi-second blocks (card stalling during flash garbage collection). Verify that `sector_cache.c` behaves deterministically and drops audio frames gracefully (e.g., repeating the last valid sample block or muting) instead of crashing the DMA engine.
* **Malformed Image & Filesystem Fuzzing:** Build a fuzzing harness for `src/disc_image.c`.
    * *Test Case:* Feed the disk parsers highly malformed variants of `.CUE`, `.NRG`, and `.MDF` structures (e.g., self-contradictory track indices, negative pre-gaps, truncated chunks, or cyclical directory records inside virtual ISO generation). Ensure they drop back safely to a clean `DRIVE_ERROR` status instead of hitting unhandled pointer exceptions or panic-crashing through a `hard_assert`.

---

## 2. Next-Generation Feature Roadmap

Given the processing power of the RP2350, the presence of a secondary SD partition, and the wireless network stack, Nebula32 has substantial headroom for advanced architectural additions.

### 2.1. Web & Network-Driven Capabilities
* **Wireless Network Drive Mirroring (WebDAV / SMB Streaming):** 
Store your entire CD32 library on a home server instead of local flash.
    * *Concept:* Utilize the Pico's built-in wireless stack to connect directly to a local NAS via an SMB or WebDAV mount point. By tuning the `sector_cache.c` ring buffers to aggressively prefetch over a wireless sockets pool, games can be actively streamed over the local network into the Akiko DMA buffer.
* **OTA Image Transfer and Web Management Interface:** Enhance `src/webserver.c` to act as an active file coordinator.
    * *Concept:* Implement a chunked file-upload portal in the web UI allowing users to upload or delete `.ISO`, `.BIN`, and `.CUE` images directly onto the storage layout wirelessly, eliminating the need to physically eject the SD card to update game archives.

### 2.2. Disk Image & Codec Enhancements
* **Compressed Audio Tracking (.CUE + Compressed Audio):** 
Games with large mixed-mode tracks consume substantial SD space.
    * *Concept:* Integrate a lightweight, hardware-accelerated audio decompression library on Core 1 to parse `.CUE` files that link directly to compressed formats (such as `.mp3`, `.flac`, or `.opus`), converting them on-the-fly into the raw 24-bit I2S frames expected by the `da_output.pio` state machine.
* **Integrated NVRAM / Save-Game Backup Engine:** Amiga CD32 games save data to internal non-volatile RAM via Akiko system calls.
    * *Concept:* By monitoring specific access sequences or exploiting known side-channels on the serial bus, capture non-volatile data streams and write them out directly as standard `.sav` or `.eep` files into a dedicated backup folder on the FatFS SD partition.

### 2.3. Native Amiga Integration & UI/UX
* **Amiga-Side Disc Selection Software ("Boot to Menu"):** 
Eliminates the need to physically interact with the hardware buttons, web interface, or external screen to switch games.
    * *Concept:* Synthesize a minimal, specialized Amiga executable auto-mounted on `Partition 2`'s virtual ISO layer when no real disk image is chosen. The Amiga boots this interface, allowing users to scroll through their entire SD card library using a standard CD32 gamepad, sending selection commands back down to Nebula32 via custom COMMO command structures.

