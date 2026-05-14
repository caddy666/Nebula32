#pragma once
// =============================================================================
// selftest.h — Hardware Self-Test
// =============================================================================
// Call selftest_run() from main() on first boot (or whenever the board is
// re-wired) to verify all hardware connections are correct before the CD32
// firmware is started.
// =============================================================================


#include <stdbool.h>

// Run all hardware self-tests and print results over USB serial.
// Returns true if all tests pass, false if any fail.
bool selftest_run(void);

