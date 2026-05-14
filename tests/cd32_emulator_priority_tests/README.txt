
CD32 Optical Drive Emulator Test Suite
======================================

This archive contains prioritized validation tests for a Raspberry Pi Pico 2
CD32 optical drive emulator project.

Structure:
- Each folder contains:
  - description.txt
  - standalone .cpp example

Priority Order:
1 = Most critical for compatibility

Recommended Execution Order:
1. Sector cadence/deadline testing
2. Buffer underrun testing
3. SD stall injection
4. Q-subcode validation
5. Parser fuzzing
6. DMA swap testing

These tests are intended as production-oriented validation examples.
