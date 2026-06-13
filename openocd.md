# OpenOCD & On-Hardware Testing — Nebula32 / Core2350B0

## Critical constraint

**OpenOCD halts both cores.** The DA DMA stream stops mid-buffer, COMMO times out, sector
prefetch stalls. OpenOCD is only useful for inspecting *configuration* (registers, mux state)
and post-failure forensics — not for testing anything that runs in real time.

**UART1 (GPIO 20/21) is the bigger win.** The firmware keeps running, the CD32 stays happy, and
a Pi or FTDI dongle on GPIO 20/21 reads structured pass/fail lines. USB CDC cannot do this —
the CD32 has no USB host, so any test that requires the device to be connected to a real CD32
can only report results over UART1.

---

## What unit tests cannot reach

### DA output

| What to test | Why host tests miss it | How to test it |
|---|---|---|
| PIO0 SM0 CLKDIV register = 32 | `DaExpand` verifies the formula; no test checks the register was written | OpenOCD: `mrw 0x502000C8` → bits [31:16] = 32 |
| DMA DREQ paced by PIO0 TX FIFO | `DmaDoubleBuffer` mocks DMA; DREQ_PIO0_TX0 never verified | OpenOCD: DMA CH0 CTRL → TREQ_SEL field = 0 |
| PIO FIFO never starves during playback | No real timer in host tests | UART1: read `pio0->fdebug` after each ISR — TXSTALL bit must stay 0 |
| `s_buf_lba` advances monotonically | `DaSpeed` mocks transitions | UART1: print `s_buf_lba[0/1]` each second; repeated value = stuck |
| DRQ packet storm on cache miss | `SectorCadence` uses fake timing | UART1: timestamp `s_drq_pending` true→cleared; > 1 sector period = trouble |
| clkdiv trim converges to M17SINE | Arithmetic tested; feedback loop not | UART1: log `s_clkdiv_fixed` every 2 s; should reach a stable value |

### COMMO bus

| What to test | Why host tests miss it | How to test it |
|---|---|---|
| PIO1 SM0 PINCTRL base = 44 (remapped from 15) | `CommoProtocol` uses `commo_hal_stub`, never touches PIO regs | OpenOCD: `mrw 0x50300000 + SM0_PINCTRL` → OUT_BASE = 44 |
| IF_DIR (GPIO 46) actually toggles on TX | Stub captures `txd()` calls; GPIO never moves | OpenOCD watchpoint on `gpio_put(PIN_IF_DIR, ...)` |
| Power-on 0x27 byte reaches PIO1 TXFIFO | `CommoPowerOn` verifies byte sequence; PIO FIFO never loaded | UART1: log `commo_hal_txd()` calls; check FSTAT TX-not-full |
| COMMO RX gets data from real CD32 | Tests inject via `commo_hal_stub_push_rx()` | Needs a real CD32; UART1 logs the first received opcode byte |

### CD32 compatibility

| What to test | Why host tests miss it | How to test it |
|---|---|---|
| Boot reaches `IDLE` state | `main.c` boot sequence has no unit test | UART1: print state at `commo_bridge_init()` entry |
| SD mount + image list populated | `selftest.c` covers mount; image count not checked | Extend selftest: print `s_total_count` via UART1 |
| Sector cache keeps up at 1× speed | `SdStallSim` is stochastic; real SD latency unknown | UART1: count `sector_cache_get()` misses over 60 s of play |
| PSRAM available + allocations succeed | No host test for QMI hardware init | UART1: `psram_available()` + `psram_alloc(4096)` must be non-NULL |
| I2C1 bus live, MCP23017 ACKs at 0x20 | Rotary logic tested only via MCP23017 lib | `selftest.c`: `i2c_read_blocking(i2c1, 0x20, ...)` — NACK = absent |
| WiFi CYW43 brings up on GPIO 23/24/25/29 | No unit test touches CYW43 | UART1: `cyw43_arch_init()` return code logged at boot |
| GPIO mux assigned correctly after remap | CMakeLists sets compile-defs; nobody reads IO_BANK0 CTRL | OpenOCD: GPIO 16 CTRL (0x40014040) → GPIO_FUNC_UART = 2 |

### What only a logic analyzer can test

- **Akiko accepting the I2S stream** — only a real CD32 responding to COMMO confirms this
- **Real BCLK jitter and duty cycle** — already captured in `digital.csv`; OpenOCD cannot measure timing
- **COMMO bit timing vs CD32 master clock** — protocol timing only provable on real hardware
- **Audio quality under 2× speed** — buffer underrun manifests as silence/glitch on real hardware

---

## RP2350 register quick-reference for OpenOCD scripts

```
PIO0_BASE        = 0x50200000
PIO1_BASE        = 0x50300000
  FSTAT  offset  = 0x004   (bits [19:16] TXFULL, [3:0] RXEMPTY per SM)
  FDEBUG offset  = 0x008   (bits [19:16] TXSTALL, [3:0] RXSTALL per SM)
  SM0_CLKDIV     = +0x0C8  (bits [31:16] INT, [15:8] FRAC)
  SM0_PINCTRL    = +0x0D4  (bits [4:0] OUT_BASE, [9:5] SET_BASE, [14:10] SIDESET_BASE, [19:15] IN_BASE)

DMA_BASE         = 0x50000000
  CH0_CTRL_TRIG  = +0x000  (bits [20:15] TREQ_SEL; DREQ_PIO0_TX0 = 0)
  CH0_READ_ADDR  = +0x004
  CH0_TRANS_COUNT= +0x008

IO_BANK0_BASE    = 0x40014000
  GPIO_N_CTRL    = 0x40014000 + N*8 + 4   (bits [4:0] FUNCSEL)
    FUNCSEL values: 0=XIP, 1=SPI, 2=UART, 3=I2C, 4=PWM, 5=SIO, 6=PIO0, 7=PIO1, 8=PIO2
                    9=GPCK, 12=XIP_CS1(RP2350 only), 31=NULL
```

Example TCL — verify DA PIO config:
```tcl
halt
set clkdiv [mrw 0x502000C8]
echo "PIO0 SM0 CLKDIV int=[expr ($clkdiv >> 16) & 0xFFFF] (expect 32)"
set fdebug [mrw 0x50200008]
echo "FDEBUG TXSTALL=[expr ($fdebug >> 16) & 0xF] (expect 0)"
set g16ctrl [mrw 0x40014084]
echo "GPIO16 FUNCSEL=[expr $g16ctrl & 0x1F] (expect 2 = UART)"
set g44ctrl [mrw 0x40014164]
echo "GPIO44 FUNCSEL=[expr $g44ctrl & 0x1F] (expect 7 = PIO1)"
resume
```

---

## Implementation tiers

### Tier 1 — extend `selftest.c` (no debugger needed)

Run at `#` keypress during boot; output to USB CDC + UART1. All checks are plain C via the SDK.

```c
// PIO0 SM0 CLKDIV integer part — verifies da_output_init() wrote the right value
uint32_t clkdiv_reg = pio0->sm[0].clkdiv;
uint16_t clkdiv_int = clkdiv_reg >> 16;
TEST_ASSERT(clkdiv_int == 32, "PIO0 SM0 CLKDIV int=32 (2.12 MHz BCLK)");

// GPIO mux: COMMO pins must be PIO1 (funcsel 7)
TEST_ASSERT(gpio_get_function(PIN_IF_CLK) == GPIO_FUNC_PIO1, "GPIO44 mux = PIO1");
TEST_ASSERT(gpio_get_function(PIN_IF_DIR)  == GPIO_FUNC_PIO1, "GPIO46 mux = PIO1");

// UART0 debug on GPIO 16
TEST_ASSERT(gpio_get_function(PIN_UART0_TX) == GPIO_FUNC_UART, "GPIO16 mux = UART");

// I2C1 bus: MCP23017 ACKs at 0x20
uint8_t dummy;
int rc = i2c_read_blocking(i2c1, MCP23017_I2C_ADDR, &dummy, 1, false);
TEST_ASSERT(rc >= 0, "MCP23017 ACKs on I2C1 (GPIO 26/27)");

// PSRAM
TEST_ASSERT(psram_available(), "PSRAM initialised (GPIO 47 QMI CS1)");
void *p = psram_alloc(4096);
TEST_ASSERT(p != NULL, "PSRAM alloc 4 KB succeeds");
```

### Tier 2 — OpenOCD TCL scripts (needs picoprobe or J-Link on SWD pads)

Static register checks after flashing — run once to validate the Core2350B0 remap is wired
correctly before PCB assembly, or as a post-flash sanity check in CI.

Suggested scripts (place in `tests/openocd/`):

- `verify_pio_config.tcl` — CLKDIV, FDEBUG, PINCTRL for PIO0 SM0 and PIO1 SM0
- `verify_gpio_mux.tcl` — IO_BANK0 CTRL for GPIO 16, 20, 26, 30, 40, 44, 47
- `verify_dma_dreq.tcl` — DMA CH0/CH1 CTRL TREQ_SEL after `da_start()`

### Tier 3 — UART1 runtime monitor

A Python script on a Raspberry Pi connected to GPIO 20/21 parses tagged log lines already
emitted by existing `printf` calls in `da_output.c`, `commo_bridge.c`, and `sector_cache.c`.
No new firmware instrumentation needed for basic monitoring.

```python
# tests/uart1_monitor.py — connect Pi GPIO to Nebula32 GPIO 20 (TX) / 21 (RX)
import serial, re, sys

port = serial.Serial('/dev/ttyS0', 115200, timeout=5)
results = {}

while True:
    line = port.readline().decode(errors='replace').strip()
    if m := re.match(r'\[DA\].*clkdiv=(\d+)', line):
        results['clkdiv'] = int(m.group(1))
    if m := re.match(r'\[DA\].*miss=(\d+)', line):
        results['cache_misses'] = int(m.group(1))
    if '[PASS]' in line or '[FAIL]' in line:
        print(line)
```

---

## Connection diagram (Core2350B0)

```
Core2350B0          picoprobe / Pi
  SWDIO (pin)  ──→  SWDIO
  SWDCLK (pin) ──→  SWDCLK
  GND          ──→  GND

  GPIO 20 (TX) ──→  Pi GPIO 15 / FTDI RX  (UART1 test reporter)
  GPIO 21 (RX) ──→  Pi GPIO 14 / FTDI TX  (optional command injection)
```

SWD pads are on the Waveshare Core2350B0 board edge (see board silkscreen).
UART1 test output is independent of SWD — the two can be used simultaneously.


add tests to suite:
Test 1: Core 0 Liveness (Verify ARM Cortex-M33 Core 0 registers are accessible).

    Test 2: Core 1 Liveness (Verify Core 1 can be brought out of reset cleanly).

    Test 3: Bootrom Revision Verification (Read address 0x00000000 for expected chip stepping ID).

    Test 4: Main SRAM Bank A Pattern Fill (0x55555555 alignment verification across 0x20000000).

    Test 5: Main SRAM Bank B Pattern Fill (0xAAAAAAAA inversion verification).

    Test 6: QSPI Flash JEDEC ID Validation (Query onboard Winbond/Waveshare flash hardware signature).

    Test 7: Flash Block Erasure Verification (Prove OpenOCD can clear storage sectors).

    Test 8: PSRAM Continuity Map (Verify execution capability past 0x11000000 boundary if equipped).

    Test 9: SysClock PLL Lock Status (Verify internal clock controller registers settle at targeted frequency).

    Test 10: Watchdog Timer Register Access (Ensure watchdog registers accept raw binary configurations).

    Test 11: Main Binary Checksum Verification (Calculate hardware MD5/CRC matching against the compiled desktop ELF).

    Test 12: VREG (Voltage Regulator) Control Verification (Read internal power management status bits).

    Test 13: Default GPIO State Check (Confirm all uninitialized pins sit at high-impedance floating levels).

    Test 14: CoreSight Debug Port Interrogation (Confirm raw SWD break vectors behave as expected).

    Test 15: Flash Write Protection Verification (Ensure boot sectors reject raw destructive manipulation commands).

2. Akiko Protocol & CD-ROM Logic Engine (Tests 16–45)

Isolated C99 host-side unit tests inside CppUTest using local mocks and captures.

    Test 16: Sync Pattern Recognition (Identify the standard 12-byte 00 FF FF... CD sector preamble).

    Test 17: Sector Header Parsing (Extract Minute, Second, Frame, and Mode bytes from a raw stream).

    Test 18: Mode 1 Data Extraction (Verify the 2048-byte payload boundary offset logic).

    Test 19: Mode 2 Form 1 Data Extraction (Verify 2048-byte form offset matching).

    Test 20: Mode 2 Form 2 Data Extraction (Verify 2324-byte raw streaming offset parameters).

    Test 21: Subchannel Q-Code Deserialization (Parse 96-bit packets into structured absolute/relative position frames).

    Test 22: Subchannel Track Number (TRK) Validation (Confirm BCD values parse smoothly from 01 to 99).

    Test 23: Index (IDX) Processing (Handle special index flags like 00 for pre-gap spacing).

    Test 24: Subchannel CRC-16 Calculation (Implement the standard X16+X12+X5+1 polynomial validation).

    Test 25: Subchannel CRC Error Rejection (Ensure corrupt timing codes are flagged and dropped safely).

    Test 26: Absolute Time (AMIN, ASEC, AFRAME) Monotonicity Check (Assert time frames always increment sequentially).

    Test 27: Relative Time (MIN, SEC, FRAME) Zero-Reset Detection (Assert track relative counters flip appropriately).

    Test 28: Catalog Number (UPC/EAN) Decode Engine (Validate ASCII conversions from subchannel R-W streams).

    Test 29: International Standard Recording Code (ISRC) String Capture (Extract tracking metadata from audio channels).

    Test 30: Akiko Target Sector Math (Convert Minute/Second/Frame layouts straight into absolute Logical Block Addresses).

    Test 31: Reverse LBA Conversion (Verify inverse logic converts raw integer blocks back into clean timestamp structs).

    Test 32: TOC (Table of Contents) Memory Matrix Generation (Construct track layout lists from a mock binary index file).

    Test 33: TOC Lead-In Command Match (Verify behavior when Akiko requests track layout parameters).

    Test 34: TOC Lead-Out Boundary Check (Assert seek failures flag correctly when crossing maximum disc limits).

    Test 35: Multi-Session Offset Parsing (Ensure index reads gracefully hop over complex index session mappings).

    Test 36: Drive Command Packet Parsing (Decode incoming multi-byte packets routed through Akiko's command register).

    Test 37: Play Audio Command Parsing (Verify track parameters are isolated smoothly from raw bytes).

    Test 38: Pause/Resume Command Parsing (Verify state transitions behave correctly).

    Test 39: Read Drive Status Command Execution (Verify returned status flags match a standard active spinner).

    Test 40: Stop Motor Execution (Verify structural timeout flags match spinning conditions).

    Test 41: Spin-Up Timing State Simulation (Confirm status bits accurately mirror spin tracking intervals).

    Test 42: Audio Status Flag Updates (Verify audio playing/paused flags mirror state changes correctly).

    Test 43: Disc Change Injection Simulation (Verify code steps match mechanical door/eject line toggling).

    Test 44: Drive Error Status Generation (Verify execution path when responding with a "No Disc" flag).

    Test 45: Command Checksum Validation (Ensure corrupted control packets throw distinct parsing faults).

3. PIO State Machine Assembly Validation (Tests 46–65)

Hardware-in-the-Loop (HIL) testing running raw loopbacks across physical pins on the Core2350B.

    Test 46: PIO Bit Shifter Clock Integrity (Verify the state machine shifts output data bits on the exact clock edge).

    Test 47: PIO Frame Sync Pulse Width Verification (Assert framing strobe signals remain high for precise bit cycles).

    Test 48: TX FIFO Empty Flag Routing (Assert state machine stalls cleanly when data buffers run completely dry).

    Test 49: RX FIFO Full Flag Integrity (Verify the state machine rejects incoming streams if internal buffers overflow).

    Test 50: PIO Instruction Pointer (PC) Initialization (Verify state machine starts at the correct mapped offset address).

    Test 51: PIO Jump Condition Verification (Ensure state machine branches correctly based on external pin levels).

    Test 52: PIN Direction Toggling Test (Verify PIO instructions smoothly switch lines between Input and Output modes).

    Test 53: Continuous Data Bit-Stream Loopback (Stream 32-bit test vectors across jumper lines; assert zero bit errors).

    Test 54: PIO Multi-SM Sync Pulse Verification (Confirm two separate state machines execute instructions simultaneously).

    Test 55: Side-Set Pin Contention Test (Verify peripheral pins toggle states cleanly without shifting data delays).

    Test 56: PIO Counter Delay Down-Counting (Assert program delay parameters wait for exact targeted cycle times).

    Test 57: High-Speed Clock Divider Isolation (Verify stability when running the PIO clock at divided fractions).

    Test 58: Subchannel Serial Line Encoding Logic (Confirm subchannel clock pulses map out every bit frame perfectly).

    Test 59: Akiko Custom Sub-Bus Read Emulation (Assert PIO reads bus lines within target nanosecond limits).

    Test 60: Akiko Custom Sub-Bus Write Emulation (Verify data hold stability times match Amiga specifications).

    Test 61: PIO Interrupt Service Routine (ISR) Vector Trip (Verify PIO can force an interrupt back to the ARM cores).

    Test 62: Scratch Register X Manipulation (Assert internal math variables accumulate data without leakage).

    Test 63: Scratch Register Y Manipulation (Verify secondary variable assignment loops behave predictably).

    Test 64: Input Inversion Mask Verification (Verify input pins with inverted logic read states correctly).

    Test 65: PIO Execution Base-Shift Unlocking (Ensure pins above GPIO 31 are accessible via base offsets).

4. Memory, DMA, & Peripheral Subsystems (Tests 66–85)

HIL testing verifying high-speed data flow paths inside the RP2350 silicon.

    Test 66: DMA Channel 0 to PIO TX FIFO Handshake (Verify data blocks feed into the PIO block without CPU intervention).

    Test 67: PIO RX FIFO to DMA Channel 1 Streaming (Confirm received bytes stream into SRAM arrays automatically).

    Test 68: DMA Ping-Pong Buffer Swap Logic (Assert double-buffering addresses rotate instantly on full block reads).

    Test 69: DMA Bytes-Transferred Counter Verification (Ensure block sizes count down accurately to zero).

    Test 70: DMA Interrupt Pointer Toggling (Assert the CPU wakes up instantly when a block transfer completes).

    Test 71: Inter-Core Ring Buffer Push Execution (Verify Core 0 can stream data blocks over to Core 1 securely).

    Test 72: Inter-Core Ring Buffer Pull Execution (Verify Core 1 can fetch data elements without race conditions).

    Test 73: SIO Hardware Mutex Lock Verification (Assert spinlocks prevent both cores from editing the sector buffer simultaneously).

    Test 74: SIO Hardware Mutex Unlock Routing (Confirm locks release cleanly to prevent software deadlocks).

    Test 75: High-Speed Microsecond Timer Validation (Verify internal system tickers match physical wall time).

    Test 76: Low-Power Sleep Entry Isolation (Confirm core states wake up smoothly when a bus interrupt hits).

    Test 77: Core 1 Task Allocation Logic (Verify software loops launch on Core 1 cleanly via the SDK).

    Test 78: UART0 Baud Rate Integrity (Verify data stays uncorrupted when communicating with the HIL runner).

    Test 79: UART1 Console Passthrough Check (Confirm debug print channels remain structurally separate).

    Test 80: GPIO Interrupt Assignment Setup (Confirm edge-triggered interrupts fire cleanly on input changes).

    Test 81: Internal Temperature Sensor Polling (Verify internal chip ADC lines read back valid temperature metrics).

    Test 82: Watchdog Reset Assertion Path (Force a system freeze to confirm the chip resets automatically).

    Test 83: PSRAM Cache Block Read Hit Logic (Verify high-speed memory streaming functions properly).

    Test 84: Multi-Sector Cache Miss Realignment (Ensure cache reads fetch data correctly when jumping sectors).

    Test 85: Diagnostic User LED Toggling Map (Confirm GPIO 39 drives the onboard LED correctly).

5. System Integration & Real-World Stress Profiles (Tests 86–100)

Full-stack automated HIL test profiles mimicking aggressive real-world console usage.

    Test 86: Continuous Audio Playback Seek Tracking (Simulate tracking over 5,000 consecutive blocks).

    Test 87: Rapid Random Track Seek Injection (Spam rapid seek changes to verify state engine stability).

    Test 88: Degraded Buffer Sync Recover Verification (Inject corrupt sync preambles; assert engine recovers smoothly).

    Test 89: Multi-Sector Read Pipeline Latency (Assert sector transmission processing finishes under 1ms).

    Test 90: Power-On Reset (POR) Initialization Timing (Confirm startup routines finish before the Amiga bus times out).

    Test 91: Subchannel Interleave Drift Test (Simulate data streams where audio and time tracks drift apart slightly).

    Test 92: Sector Read Timeout Recovery Validation (Verify firmware handles unexpected read drops cleanly).

    Test 93: Long-Duration Streaming Memory Leak Verification (Track heap and stack allocations over a 30-minute test loop).

    Test 94: Hot-Reset Signal Handling (Ensure the chip resets its internal state registers when the Amiga reboots).

    Test 95: Invalid Host Command Rejection Loop (Spam garbage data packets into the command engine; assert zero crashes).

    Test 96: Concurrent Inter-Core Bus Load Stress (Force heavy computation loops on Core 0 while Core 1 streams sectors).

    Test 97: DMA High-Throughput Bus Contention Map (Run concurrent memory transfers to verify data bus arbitration holds).

    Test 98: Audio Buffer Underrun Flag Safety (Verify zero-fill frames inject safely if data streams fall behind).

    Test 99: Continuous Mode Switching Abuse (Rapidly switch between data sector reads and CD-DA audio playing states).

    Test 100: Full-Disc Simulation Run (Automate a sequential play-through of a mock 650MB disc layout file).
