---
created: 2026-05-30T12:42:34.965Z
title: Add FwFlashFilename tests for fw flash security guard
area: testing
priority: high
files:
  - tests/host/test_webserver.cpp
  - src/webserver.c:877-885
---

## Problem

The filename traversal guard in `handle_request()` for `POST /api/fw/flash/{filename}` (webserver.c:877–885) is a security-critical path with no test coverage. The guard rejects:
- `..` in the filename (directory traversal)
- `/` or `\` in the filename (path separator injection — including Windows-style backslash per the S1 fix comment)
- Empty filename (`fn_len == 0`)
- Oversized filename (`fn_len >= MAX_PATH_LEN - 3`)

These are security controls, and regressions here would allow an attacker with web access to flash arbitrary files from anywhere on the SD card.

## Solution

Add a `FwFlashFilename` test group to `tests/host/test_webserver.cpp` replicating the guard logic (same pattern as `covers_path_safe`):

```c
static bool fw_flash_fname_safe(const char *raw_fname) {
    size_t fn_len = 0;
    while (raw_fname[fn_len] && raw_fname[fn_len] != '?' && fn_len < MAX_PATH_LEN - 1)
        fn_len++;
    if (fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3)) return false;
    char fname[MAX_PATH_LEN];
    memcpy(fname, raw_fname, fn_len);
    fname[fn_len] = '\0';
    return !(strstr(fname, "..") || strstr(fname, "/") || strstr(fname, "\\"));
}
```

Test cases: dot-dot rejected, slash rejected, backslash rejected, empty rejected, oversized rejected, query-string stripped before check, valid name accepted.
