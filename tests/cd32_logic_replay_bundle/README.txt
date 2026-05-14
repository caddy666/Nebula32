
# CD32 ODE Logic Analyzer Replay Guide

This bundle contains:
- Detailed replay guide
- CSV parser example
- Replay engine example
- PIO replay concept
- Timing notes
- Architecture notes
- Conversation summary

Purpose:
Replay real CD32/CD drive logic analyzer captures into a Raspberry Pi Pico 2
optical drive emulator for deterministic validation and regression testing.

Core Concept:
You are replaying timing relationships, not just bytes.

Main Architecture:

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

Recommended:
- Pico #1 = replay generator
- Pico #2 = ODE under test

Recommended replay modes:
1. Command replay
2. Byte replay
3. Bit replay
4. Edge replay

PIO + DMA replay is strongly recommended for nanosecond-scale timing.
