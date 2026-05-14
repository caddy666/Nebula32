# Replaying Logic Analyzer CSV Data Into a CD32 ODE

This document explains how to take a logic analyzer CSV capture and replay it into a CD32 Optical Drive Emulator (ODE) using a Raspberry Pi Pico 2.

Your uploaded CSV appears to contain:

- Timestamped GPIO state transitions
- Multiple digital channels
- Nanosecond-level timing
- Raw edge captures

Example rows:

```csv
Time [s],IF_CLK,IF_DATA,DA_LRCLK,DA_DATA,DA_BCLK,SUB_WFCLK,SUB_SCOR,SUB_DATA
2023-04-24T03:48:56.645824187+00:00,1,0,0,0,0,0,0,0
2023-04-24T03:48:56.645824250+00:00,0,0,0,0,0,0,0,0
```

This is excellent because it preserves exact timing transitions.

---

# 1. What The Replay System Actually Does

The replay harness acts like a fake CD32 or fake drive.

It reproduces:

- clock edges
- data transitions
- timing gaps
- handshake timing

into your ODE.

Architecture:

```text
CSV Capture
    ↓
Parser
    ↓
Timestamped Event Queue
    ↓
PIO/DMA Replay Engine
    ↓
GPIO Pins
    ↓
ODE Under Test
```

---

# 2. Understanding Your CSV

Your file appears to be:

```csv
Timestamp,GPIO1,GPIO2,GPIO3...
```

Each row represents:

```text
A signal change occurring at a precise timestamp.
```

Example:

```csv
2023-04-24T03:48:56.645824187+00:00,1,0,0,0,0,0,0,0
2023-04-24T03:48:56.645824250+00:00,0,0,0,0,0,0,0,0
```

This means:

```text
62ns later:
IF_CLK changed from 1 → 0
```

That timing matters.

---

# 3. First Step: Convert CSV Into Replay Events

You DO NOT want to replay:

```text
absolute timestamps
```

Instead convert into:

```text
relative delays
```

Example:

```cpp
struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};
```

Where:

```text
bit0 = IF_CLK
bit1 = IF_DATA
bit2 = DA_LRCLK
bit3 = DA_DATA
etc
```

---

# 4. Why Relative Delays Are Better

Instead of:

```text
03:48:56.645824250
```

Store:

```text
62ns later
```

This:

- compresses memory usage
- speeds replay
- simplifies timing
- enables DMA streaming

---

# 5. Example CSV Conversion Tool (Host PC)

This converts your CSV into compact replay events.

## converter.cpp

```cpp
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <ctime>

struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};

static uint64_t parse_time_ns(const std::string& s)
{
    // VERY simplified parser example.
    // Production version should properly parse ISO8601.

    auto pos = s.find_last_of(':');

    double sec = std::stod(s.substr(pos + 1));

    return (uint64_t)(sec * 1e9);
}

int main()
{
    std::ifstream file("sample_digital.csv");

    std::string line;

    std::getline(file, line);

    std::vector<ReplayEvent> events;

    uint64_t previous_time = 0;

    while(std::getline(file, line))
    {
        std::stringstream ss(line);

        std::string time_str;

        std::getline(ss, time_str, ',');

        uint64_t current_time = parse_time_ns(time_str);

        uint8_t mask = 0;

        for(int i=0;i<8;i++)
        {
            std::string bit;
            std::getline(ss, bit, ',');

            if(std::stoi(bit))
                mask |= (1 << i);
        }

        ReplayEvent e;

        e.delay_ns =
            (previous_time == 0)
            ? 0
            : (uint32_t)(current_time - previous_time);

        e.gpio_mask = mask;

        events.push_back(e);

        previous_time = current_time;
    }

    std::ofstream out("trace.bin", std::ios::binary);

    out.write(
        (char*)events.data(),
        events.size() * sizeof(ReplayEvent)
    );

    std::cout << "Converted " << events.size() << " events\n";
}
```

---

# 6. Why You Want A Binary Trace Format

CSV is:

- huge
- slow
- difficult for embedded parsing

Binary replay traces are:

- tiny
- DMA-friendly
- fast
- deterministic

---

# 7. Replay Strategy Options

There are THREE good replay methods.

---

# Method A — GPIO Bit-Banging

Simplest.

```text
CPU manually toggles pins
```

Example:

```cpp
gpio_put(PIN_CLK, clk);
gpio_put(PIN_DATA, data);
```

Pros:

- easy
- fast to prototype

Cons:

- timing jitter
- not deterministic
- difficult above a few MHz

Good for:

- initial debugging
- parser testing

---

# Method B — Hardware Timer Replay

Better.

Use:

- timer IRQs
- microsecond scheduling
- event queue

Still CPU-driven.

Better timing.

---

# Method C — PIO + DMA Replay (BEST)

This is the correct long-term solution.

Architecture:

```text
Replay Buffer
     ↓
DMA
     ↓
PIO FIFO
     ↓
GPIO Pins
```

This gives:

- deterministic timing
- minimal jitter
- hardware-accurate replay

---

# 8. Recommended Replay Hardware Setup

Use a SECOND Pico 2.

Recommended architecture:

```text
Pico #1 = Replay Generator
Pico #2 = ODE Under Test
```

This avoids:

- self-timing issues
- IRQ interference
- replay distortion

---

# 9. Wiring Example

Example:

```text
Replay Pico GPIO2  → ODE IF_CLK
Replay Pico GPIO3  → ODE IF_DATA
Replay Pico GPIO4  → ODE SUB_DATA
Replay Pico GND    → ODE GND
```

Keep wires short.

Use:

- common ground
- matched voltage levels
- proper buffering if needed

---

# 10. Basic Pico Replay Engine

## replay.cpp

```cpp
#include "pico/stdlib.h"
#include <vector>

struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};

#define PIN_IF_CLK  2
#define PIN_IF_DATA 3
#define PIN_SUBDATA 4

void apply_gpio_mask(uint8_t mask)
{
    gpio_put(PIN_IF_CLK,  mask & (1 << 0));
    gpio_put(PIN_IF_DATA, mask & (1 << 1));
    gpio_put(PIN_SUBDATA, mask & (1 << 2));
}

int main()
{
    stdio_init_all();

    gpio_init(PIN_IF_CLK);
    gpio_init(PIN_IF_DATA);
    gpio_init(PIN_SUBDATA);

    gpio_set_dir(PIN_IF_CLK, GPIO_OUT);
    gpio_set_dir(PIN_IF_DATA, GPIO_OUT);
    gpio_set_dir(PIN_SUBDATA, GPIO_OUT);

    std::vector<ReplayEvent> trace;

    // Load binary trace here

    absolute_time_t start = get_absolute_time();

    uint64_t accumulated_ns = 0;

    for(const auto& e : trace)
    {
        accumulated_ns += e.delay_ns;

        while(to_us_since_boot(get_absolute_time())
              < (accumulated_ns / 1000))
        {
        }

        apply_gpio_mask(e.gpio_mask);
    }
}
```

---

# 11. The BIG Problem: Nanosecond Timing

Your CSV appears to contain:

```text
~62ns transitions
```

Example:

```text
56.645824187
56.645824250
```

Difference:

```text
63ns
```

The CPU CANNOT accurately replay this in software.

This is why:

# You eventually need PIO.

---

# 12. Why PIO Is Perfect For This

PIO can:

- toggle GPIO deterministically
- execute exact cycles
- stream via DMA
- maintain stable timing

Perfect for:

- serial buses
- drive protocols
- replay engines

---

# 13. Better Replay Format For PIO

Instead of:

```cpp
struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};
```

Use:

```cpp
struct PIOEvent
{
    uint16_t delay_cycles;
    uint16_t pins;
};
```

Where:

```text
1 cycle = 8ns @125MHz
```

Much easier for PIO replay.

---

# 14. Example PIO Replay Concept

PIO program:

```pio
.program replay
pull block
out pins, 8
pull block
out x, 16
wait_loop:
    jmp x-- wait_loop
```

Each event:

```text
set GPIO
wait N cycles
```

DMA continuously feeds events.

This becomes:

```text
hardware-accurate protocol replay
```

---

# 15. What You Actually Validate

When replaying the trace into your ODE, you can measure:

| Metric | Why Important |
|---|---|
| ACK latency | protocol timing |
| Sector response time | FMV stability |
| Retry behavior | compatibility |
| Inter-byte timing | edge-case titles |
| FIFO underruns | DMA stability |
| Response ordering | correctness |

---

# 16. Golden Trace Validation

This is the REALLY powerful part.

You can:

1. Capture real drive behavior
2. Replay identical traffic into ODE
3. Compare ODE output against real drive

This becomes:

```text
automated regression testing
```

---

# 17. Differential Comparison

You should compare:

## Real Drive

vs

## Your Emulator

Measure:

```text
response latency
inter-byte gaps
ack timing
sector cadence
```

---

# 18. FMV Replay Testing

This is one of the BEST uses.

Capture:

- FMV playback
- long streaming sessions
- rapid seek transitions

Replay:

- exact same timing

Then monitor:

```text
deadline misses
DMA starvation
buffer low watermark
```

---

# 19. Important Replay Modes

You should support multiple levels.

| Replay Mode | Usage |
|---|---|
| Decoded commands | parser testing |
| Byte replay | protocol testing |
| Bit replay | timing testing |
| Edge replay | electrical validation |

Your CSV is already close to:

```text
edge replay
```

which is excellent.

---

# 20. Most Important Insight

The important thing is NOT replaying bytes.

The important thing is replaying:

```text
timing relationships
```

That is what reveals:

- FMV bugs
- DMA bugs
- jitter problems
- buffering flaws
- protocol race conditions

This is why replay systems are so incredibly effective for ODE development.

