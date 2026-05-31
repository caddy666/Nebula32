---
created: 2026-05-31T05:36:05.046Z
title: Harden fw flash filename guard and add missing tests
area: testing
priority: high
files:
  - src/webserver.c:877-878
  - tests/host/test_webserver.cpp:472-481
---

## Problem

Five issues found during the 2026-05-31 replica audit of `FwFlashFilename` tests:

### S1 — Security: missing `%` guard in `POST /api/fw/flash/` filename validator

The `/covers/` guard (webserver.c:911–912) explicitly rejects filenames containing `%`
to block `%2e%2e`-style bypasses of the `..` check. Its comment says: *"no
percent-encoded sequences (%2e%2e bypasses '..' check)"*.

The `/api/fw/flash/` guard at line 877 has **no `%` check**:
```c
// CURRENT — missing % guard:
if (strstr(fname, "..") || strstr(fname, "/") || strstr(fname, "\\") ||
    fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3)) {
```

In practice FatFS does not URL-decode, so `0:/%2e%2e%2fconfig.UF2` will fail to open.
However this relies on FatFS behaviour as an implicit safety net rather than an explicit
guard. If the VFS layer ever changes, the bypass becomes real.

### O1 — Optimisation: `strstr` used for single-character search

The fw/flash guard uses `strstr(fname, "/")` and `strstr(fname, "\\")` for single-char
searches. The covers guard correctly uses `strchr`. Inconsistent and slightly slower.

### O2 — Optimisation: cheap `fn_len` bound checks run after three `strstr` calls

For empty filenames (`fn_len == 0`), three `strstr("")` calls return NULL before the
trivial integer check runs. The test replica already uses the better order (bounds first);
production should match.

### T1 — Test gap: no exact boundary test for `fn_len` limit

`Oversized_Rejected` creates a string of `MAX_PATH_LEN + 3` chars — far above the
threshold. Missing: exact threshold (`MAX_PATH_LEN - 3` → reject) and one-below
(`MAX_PATH_LEN - 4` → accept).

### T2 — Design note / test gap: `strstr("..")` rejects non-traversal double-dot names

`strstr(fname, "..")` rejects any filename containing two consecutive dots, including
`firmware..v2.UF2`. No test documents this intentional false-positive behaviour.

## Solution

**S1** — Add `strchr(fname, '%')` to the fw/flash guard to match the covers endpoint:
```c
if (fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3) ||
    strstr(fname, "..") || strchr(fname, '/') || strchr(fname, '\\') ||
    strchr(fname, '%')) {
```
Also update the `fw_flash_fname_safe` replica in `tests/host/test_webserver.cpp` and
add a `PercentEncoded_Rejected` test.

**O1** — Replace `strstr(fname, "/")` / `strstr(fname, "\\")` with `strchr` in
`webserver.c:877` and in the test replica.

**O2** — Reorder the guard: bounds checks first, then string searches.

**T1** — Add two tests:
- `Oversized_ExactThreshold_Rejected`: `MAX_PATH_LEN - 3` chars → `CHECK_FALSE`
- `Oversized_OneBelowThreshold_Accepted`: `MAX_PATH_LEN - 4` chars → `CHECK_TRUE`

**T2** — Add `DoubleDotInBasename_AlsoRejected` test with a comment explaining the
conservative trade-off. If future releases need to permit `firmware..v2.UF2`, change
the check to match only path-boundary `..` (`/..`, `../`, `\..`, `..\`).
