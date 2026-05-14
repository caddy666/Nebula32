
# Pico 2 CD32 Optical Drive Emulator - Unit Test Examples

This archive contains example C++ unit tests for a Raspberry Pi Pico 2
CD32 optical drive emulator project.

Included:
- Command parser tests
- Sector generation tests
- Q-subcode tests
- Ring buffer tests
- DMA simulation tests
- Fuzz tests
- Golden trace replay examples

Framework:
- Catch2 single-header examples
- Designed to run on host PC first

Suggested build:
    mkdir build
    cd build
    cmake ..
    make

These are intentionally simplified examples intended as a starting point
for a production emulator firmware test suite.
