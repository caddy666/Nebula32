---
created: 2026-05-30T12:45:01.770Z
title: Audit all test replicas match production code
area: testing
priority: high
files:
  - tests/host/
  - src/
  - upstream/core/
---

## Problem

The host test suite uses a "replica" pattern throughout: static helpers that can't be compiled without hardware deps (lwIP, PIO, CYW43) are copied verbatim into test files and tested in isolation. If production code changes and the replica isn't updated, tests keep passing while the real code drifts — a silent false-positive.

We already caught one instance of this (webserver.c `parse_cfg_line` comment claiming it's "static in webserver.c" when `read_wifi_config()` actually delegates to `logger_get_config()`).

16 of 34 test files explicitly flag replicated helpers with comments like "replica", "replicated", "stays in sync", "mirrors", "mimics". These are the highest-risk files. The remaining 18 files may also have inline logic copied from production.

## Solution

Go through each test file that contains replicated helpers and diff the replica against the current production source:

1. Identify the replica (grep for "replica", "replicated", "stays in sync", "must stay in sync")
2. Find the corresponding production function (file + line number)
3. Verify the logic is byte-for-byte identical (modulo variable names and `static` qualifiers)
4. Flag any divergence as a bug — either the test replica is stale or the production code changed without updating the test

High-priority files (most replica surface area):
- `tests/host/test_webserver.cpp` — basename_no_ext, state_name, covers_path_safe, load_index_valid, extract_request_token, parse_cfg_line, html_escape, fw_token_valid
- `tests/host/test_da_speed.cpp` — clkdiv logic
- `tests/host/test_akiko_dma.cpp` — DMA fake engine
- `tests/host/test_logger.cpp` — parse_bool, trim, ring buffer
- `tests/host/test_commo_protocol.cpp` / `test_commo_fuzz.cpp` — COMMO state machine

For each divergence found: either fix the replica or open a new todo if the production code itself is wrong.
