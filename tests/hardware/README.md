# Hardware Tests — Nebula32 / Core2350B0

Unit tests in `tests/host/` run on a PC with no hardware. These tests fill the
gaps that only real hardware can cover: live signal timing, GPIO mux state in
silicon, and runtime firmware behaviour during actual disc playback.

## Tools required

| Test type | Tool | How to run |
|-----------|------|-----------|
| Logic Analyzer (LA) | [LogicAnalyzer](https://github.com/gusmanb/logicanalyzer) `TerminalCapture` CLI | `python3 la_*.py <serial-port>` |
| OpenOCD | picoprobe or J-Link + `openocd` | `openocd … -f tests/hardware/openocd_*.tcl` |
| UART1 monitor | USB-UART adapter or Pi UART on GPIO 20/21 | `python3 uart1_*.py <serial-port>` |

Python dependencies: `pip install pyserial`  (UART1 scripts only)

---

## Logic Analyzer tests

All three scripts support `--replay <csv>` to re-analyse a previously saved
capture without reconnecting the analyzer.

### Wiring guide

Connect **analyzer GND to Nebula32 GND** for every capture.  
The analyzer's channel 0 must be wired before running any script.

```
la_da_timing.py
  Analyzer Ch0 → GPIO 1  (DA_BCLK)
  Analyzer Ch1 → GPIO 2  (DA_LRCLK)
  Analyzer Ch2 → GPIO 0  (DA_DATA)
  Prerequisite: Nebula32 playing a disc (DA DMA active)

la_commo_direction.py
  Analyzer Ch0 → GPIO 44  (IF_CLK)
  Analyzer Ch1 → GPIO 45  (IF_DATA)
  Analyzer Ch2 → GPIO 46  (IF_DIR)
  Prerequisite: CD32 powered on and connected (sends SPINDLE_MOTOR_OFF at boot)

la_subcode.py
  Analyzer Ch0 → GPIO 5   (SUB_DATA)
  Analyzer Ch1 → GPIO 6   (SUB_CLK)
  Analyzer Ch2 → GPIO 7   (SUB_WFCLK)
  Analyzer Ch3 → GPIO 8   (SUB_SCOR)
  Prerequisite: Nebula32 playing a disc (subcode generated per-sector)
```

### Running

```bash
cd tests/hardware

# Live capture (replace /dev/ttyACM1 with your analyzer serial port)
python3 la_da_timing.py /dev/ttyACM1
python3 la_commo_direction.py /dev/ttyACM1
python3 la_subcode.py /dev/ttyACM1

# Replay a saved CSV (no analyzer needed)
python3 la_da_timing.py --replay /path/to/capture.csv
```

### What each test asserts

**`la_da_timing.py`** — same thresholds as CsvReplay W0–W9 host tests:
- BCLK half-period mean 150–400 ns (target 236 ns → 2.12 MHz)
- BCLK half-period σ < 60 ns
- BCLK high time > low time (duty cycle > 50%)
- LRCLK half-period mean 9 000–14 000 ns (target 11 339 ns → 44.1 kHz)
- BCLK/LRCLK ratio 44–52 (target 48 → 24-bit I²S frames)
- Timestamps monotonic, all values binary

**`la_commo_direction.py`**:
- IF_CLK (GPIO 44) has ≥ 8 edges — proves RP2350B pins above GPIO 31 work
- IF_DIR transitions observed (both RX=0 and TX=1 phases)
- CLK edges present during both DIR=0 and DIR=1 phases
- IF_DIR returns to 0 after TX burst (not stuck high)
- First TX byte = 0x27 (COMMO frame header)

**`la_subcode.py`**:
- SUB_CLK active and within 200 kHz–5 MHz range
- SUB_DATA transitions (not stuck)
- SUB_WFCLK and SUB_SCOR pulse at least once per capture window
- Both signals return low (not latched high)
- SUB_CLK frequency confirms it is a separate SM from DA_BCLK (< 3 MHz)

---

## OpenOCD tests

All three scripts run after `halt`. `openocd_dma_dreq.tcl` requires a disc
to be playing before you halt (DMA config is only valid during playback).

### OpenOCD command template

```bash
openocd \
  -f interface/cmsis-dap.cfg \
  -f target/rp2350.cfg \
  -c "adapter speed 4000; halt" \
  -s tests/hardware \
  -f openocd_gpio_mux.tcl \
  -c "resume; shutdown"
```

Replace `openocd_gpio_mux.tcl` with the desired script.  
For `openocd_dma_dreq.tcl`, start playback first:

```bash
# 1. Start disc playback (via web UI or CD32 PLAY command)
# 2. Then:
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 4000; halt" \
  -s tests/hardware -f openocd_dma_dreq.tcl \
  -c "resume; shutdown"
```

### What each script checks

**`openocd_pio_config.tcl`**:
- PIO0 SM0 CLKDIV int = 32 (DA output → 2.12 MHz BCLK)
- PIO0 SM1 CLKDIV int = 24 (subcode encoder)
- PIO1 SM0 IN_BASE = GPIO 44 (COMMO RX, moved from GPIO 15)
- PIO1 SM1 SIDESET_BASE = GPIO 44 (COMMO TX)

**`openocd_gpio_mux.tcl`** — reads IO_BANK0 CTRL for 24 GPIOs:
- DA/subcode PIO0 pins (GPIO 0–8)
- UART0/1 (GPIO 16–17, 20–21)
- I2C1 MCP23017 (GPIO 26–27)
- SDIO PIO1 (GPIO 30–32)
- ST7789 SPI1 (GPIO 40–43)
- COMMO PIO1 (GPIO 44–46) — proves high-pin mux works
- PSRAM XIP_CS1 (GPIO 47)

**`openocd_dma_dreq.tcl`** (requires active playback):
- DMA CH0 and CH1 TREQ_SEL = 0 (DREQ_PIO0_TX0)
- CH0 and CH1 EN bit set
- READ_ADDR of both channels in SRAM range

---

## UART1 monitor tests

Connect to Nebula32 GPIO 20 (UART1_TX) at 115200 baud.

```bash
# Boot sequence check (power-cycle Nebula32, then run)
python3 uart1_boot.py /dev/ttyUSB0

# Playback health (start disc playing first, then run)
python3 uart1_playback.py /dev/ttyUSB0 --duration 60

# Replay saved logs
python3 uart1_boot.py --log boot.log
python3 uart1_playback.py --log playback.log
```

**`uart1_boot.py`**:
- Boot reaches IDLE drive state (main.c 13-step sequence complete)
- WiFi CYW43439 init returned 0 (GPIO 23/24/25/29 RM2 module alive)
- PSRAM init succeeded (logged during boot)
- SD image scan completed
- First COMMO opcode from CD32 received and logged (requires real CD32)

**`uart1_playback.py`** (60 s monitor window):
- s_buf_lba advances monotonically (Core 1 prefetch not stuck)
- Total sector cache misses ≤ 10 over monitor period
- clkdiv trim converges (last 3 values within ±1 of each other)
- clkdiv final value in range [14, 34] (1× = 32, 2× = 16)
- DRQ event count logged (storm warning if > 20 events)

---

## Gap coverage summary

| Gap (from openocd.md) | Covered by |
|------------------------|-----------|
| BCLK 2.12 MHz live (not replay) | `la_da_timing.py` |
| 48-cycle I²S frame on real hardware | `la_da_timing.py` |
| IF_DIR (GPIO 46) toggles on TX | `la_commo_direction.py` |
| GPIO >31 (44–46) accessible via PIO | `la_commo_direction.py` |
| 0x27 frame header on IF_DATA wire | `la_commo_direction.py` |
| SUB_CLK / WFCLK / SCOR timing | `la_subcode.py` |
| PIO0 SM0 CLKDIV = 32 (register) | `openocd_pio_config.tcl` |
| COMMO RX IN_BASE = GPIO 44 | `openocd_pio_config.tcl` |
| All 24 GPIO mux assignments verified | `openocd_gpio_mux.tcl` |
| DMA DREQ paced by PIO0 TX FIFO | `openocd_dma_dreq.tcl` |
| Boot reaches IDLE state | `uart1_boot.py` |
| WiFi CYW43 brings up (GPIO 23/24/25/29) | `uart1_boot.py` |
| First COMMO opcode from real CD32 | `uart1_boot.py` |
| s_buf_lba advances monotonically | `uart1_playback.py` |
| Sector cache keeps up at 1× speed | `uart1_playback.py` |
| clkdiv trim converges to M17SINE | `uart1_playback.py` |
| DRQ packet timing | `uart1_playback.py` |
