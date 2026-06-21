# Nebula32 — Test Suite

Developer-facing reference for the host test suite. End-user features are in
[`FEATURES.md`](FEATURES.md); architecture in [`CLAUDE.md`](CLAUDE.md).

The firmware is mostly **host-testable**: pure-logic modules (parsers, COMMO state
machine, ECC, subcode, carousel, config, sector cache, etc.) compile and run on a
plain PC with stubs for the Pico SDK, so the bulk of behaviour is verified in CI
without hardware. Hardware-only paths (live DA/COMMO timing, PIO/DMA register
state) are covered by [`openocd.md`](openocd.md) and
[`CAROUSEL_PHASE5_BRINGUP.md`](CAROUSEL_PHASE5_BRINGUP.md) instead.

## Running

```bash
cd tests/host
make            && ./cd32_tests          # 647 tests — the main suite
make vdisc_tests && ./vdisc_tests        #  73 tests — virtual-disc / ISO-9660 synthesis
make parser_tests && ./parser_tests      #  33 tests — disc-image parsers (FatFS sim)
make stress_sector_cache && ./stress_sector_cache   # 5 tests — TSan producer/consumer
```

Add `-v` for per-test output, `-lg` to list groups. **Total: 758 tests, 0 failures.**

**Sanitizers:** every C/C++ object and the link step build with
`-fsanitize=undefined -fno-sanitize-recover=all`; the stress binary adds
ThreadSanitizer. Warnings are `-Wall -Wextra -Werror`.

**Replica convention:** several `static` helpers (e.g. `basename_no_ext`,
`parse_bool`, the playlist slot-wrap math) are copied verbatim into the test files
and verified against the live source — if production diverges, the replica test
diverges. Each replica is commented "kept in sync with src/…".

---

## `cd32_tests` — main suite (647)

| Area | Groups |
|------|--------|
| **CD timing / DA** | `LbaMsf` `DaExpand` `DaSpeed` `DaExtended` `SectorCadence` `DmaDoubleBuffer` `AkikoDma` |
| **Subcode / Q-channel** | `Subcode` `SubcodePio` `SubcodeClk` `SubcodeExtended` `SubchannelMath` `QSubchannel` |
| **COMMO protocol** | `CommoProtocol` `CommoExtended` `CommoFuzz` `CommoPowerOn` `CommandFuzz` `OpcResponses` |
| **ECC / sectors** | `Ecc` `SectorLayout` `SectorCache` `CacheExtended` `CoreIpcDesync` `RingBuffer` `SdStallSim` |
| **Disc image / TOC** | `DiscFindTrack` `IsoLayout` `LbaFileOffset` `TocResponse` `CueMsfArith` `NrgTrackCalc` `Be32Be64` `FormatDetect` |
| **Carousel / jukebox** | `CarouselNav` `CarouselPlaylist` `CarouselPathMatch` `PlaylistMenuWrap` `DiscChange` |
| **Drive state / door** | `DoorTray` `DoorPin` `HostReset` `MotorSledFake` |
| **Display / visualiser** | `Fft` `VisAudio` `EffectsColor` `CoverDir` |
| **Web / config / logging** | `Webserver` `WebserverHtml` `WebExtended` `HtmlEscape` `FwFlashFilename` `Config` `ConfigCrc` `Logger` |
| **Firmware update** | `Uf2Parse` `Uf2Flash` `Uf2SecurityFixes` |
| **PSRAM / misc** | `PsramCache` `Maths` |
| **Hardware-signal replay** | `CsvReplay` `CsvReplayPonPoff` `CsvReplayZool2` `CsvReplayPinball` |

The `CsvReplay*` groups validate against **real 5–12 GB logic-analyser captures**
(`digital.csv`, `pon-poff-idle.csv`, `zool2.csv`, `pinball.csv`) — each test reads
a windowed slice and asserts BCLK/LRCLK period, jitter, duty cycle, and the 48-bit
frame ratio. These confirm the firmware's timing model matches real hardware.

---

## `vdisc_tests` — virtual disc (73)

ISO 9660 on-the-fly synthesis (`src/virtual_disc.c`) via a directory-traversal sim:
`VdiscMount` `VdiscLbaLayout` `VdiscPvd` `VdiscVdst` `VdiscPathTable`
`VdiscDirSector` `VdiscFileData` `VdiscToc` `VdiscIntegration` `VdiscExtended`.

## `parser_tests` — disc-image parsers (33)

ISO/BIN/NRG/MDF parsing against an injectable FatFS sim:
`ParseIso` `ParseBin` `ParseNrg` `ParseMdf` `ParseExtended` `SectorAccess`.

## `stress_sector_cache` — concurrency (5, TSan)

Dual-core producer/consumer race coverage for the sector cache: queue boundaries,
`flush_gen` stale guard, fill/drain cycles, and a concurrent 500-sector integrity
run under ThreadSanitizer.

---

> Historical test *blueprints* (aspirational "50/100-test" specs) were archived to
> `docs/history/` — they predate this catalogue and are not the current suite.
