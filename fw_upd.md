#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/scb.h"
#include "hardware/pio.h"

typedef void (*entry_point_t)(void);

void safe_firmware_handover(uint32_t target_flash_address) {
    // Step 1: Kill Core 1 so it doesn't try to touch PIO during teardown
    multicore_reset_core1();

    // Step 2: Clear all CPU interrupts on Core 0
    __disable_irq();

    // Step 3: Tear down PIO hardware to clear old state machines
    teardown_all_pio_hardware();

    // Step 4: Map the Cortex-M33 VTOR to the new firmware bank
    scb_hw->vtor = target_flash_address;

    // Step 5: Extract the Stack Pointer and Reset Vector from the new binary
    uint32_t *new_vector_table = (uint32_t *)target_flash_address;
    uint32_t initial_stack_pointer = new_vector_table[0];
    entry_point_t new_reset_handler = (entry_point_t)new_vector_table[1];

    // Step 6: Set Stack Pointer and execute the new firmware
    __set_MSP(initial_stack_pointer);
    new_reset_handler(); 
}

#include "hardware/regs/addressmap.h"
#include "hardware/structs/scb.h"

typedef void (*entry_point_t)(void);

void jump_to_new_firmware(uint32_t flash_slot_address) {
    // 1. Stop Core 1 safely so it doesn't fire interrupts mid-handover
    multicore_reset_core1();

    // 2. Disable all local interrupts on Core 0
    __disable_irq();

    // 3. Point the M33 VTOR to the start of the new firmware's vector table
    // For Slot B, this might be (XIP_BASE + 0x100000)
    scb_hw->vtor = flash_slot_address;

    // 4. Extract the initial Stack Pointer (SP) and Reset Vector from the new table
    uint32_t *vector_table = (uint32_t *)flash_slot_address;
    uint32_t new_stack_pointer = vector_table[0];
    entry_point_t new_reset_handler = (entry_point_t)vector_table[1];

    // 5. Set the MSPl (Main Stack Pointer) and jump execution
    __set_MSP(new_stack_pointer);
    new_reset_handler(); // Goodbye old firmware!
}


#include "pico/multicore.h"

void core1_ode_entry() {
    // This core strictly runs the CD32 emulation loops.
    // Absolutely NO flash writing or file system calls allowed here.
    while(1) {
        run_cd32_emulation_tick();
    }
}

int main() {
    stdio_init_all();
    
    // Launch critical CD32 emulation loop on Core 1 immediately
    multicore_launch_core1(core1_ode_entry);

    // Core 0 handles the firmware update checking safely
    if (sd_card_detect_inserted()) {
        check_and_flash_sd_update(); 
    }

    // Continue to normal Core 0 tasks (ISO loading, UI handling, etc.)
    while(1) {
        run_core0_management_systems();
    }
}

#include "pico/bootrom.h"
#include "hardware/watchdog.h"

// Check data integrity (e.g., CRC32 calculation over the written flash space vs file header)
if (verify_flash_checksum(FLASH_TARGET_OFFSET, expected_crc)) {
    
    // Optional: Rename file on SD so it doesn't loop
    f_rename("cd32_update.bin", "cd32_update.bak");
    f_unmount("0:");

    // Tell the Pico 2 to switch active banks upon reboot
    // The RP2350 bootrom checks this configuration
    flash_select_app_bank(1); // Switch to Bank 1 (Slot B)
    
    // Force a hardware watchdog reset
    watchdog_reboot(0, 0, 0); 
} else {
    // Validation failed! Fail safe. Erase the corrupt Slot B and boot Slot A normally.
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    f_unmount("0:");
}

#include "pico/flash.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

#define FLASH_TARGET_OFFSET  0x100000 // Target Offset for Slot B (1MB mark)
#define BUFFER_SIZE          4096

uint8_t read_buf[BUFFER_SIZE];

// inside your update function...
UINT bytes_read;
uint32_t current_offset = FLASH_TARGET_OFFSET;

while (f_read(&fil, read_buf, BUFFER_SIZE, &bytes_read) == FR_OK && bytes_read > 0) {
    // Flash writes must be aligned to 256-byte pages, erases to 4096-byte sectors
    uint32_t ints = save_and_disable_interrupts();
    
    // Erase the sector first if we are at a 4KB boundary
    if (current_offset % FLASH_SECTOR_SIZE == 0) {
        flash_range_erase(current_offset, FLASH_SECTOR_SIZE);
    }
    
    // Program the data page
    flash_range_program(current_offset, read_buf, bytes_read);
    
    restore_interrupts(ints);
    current_offset += bytes_read;
}

f_close(&fil);

---

## TODO — bugs, security holes, optimisations found during 2026-05-31 audit

### Security

**S1 — Missing `%` guard in `POST /api/fw/flash/` filename validator** (`src/webserver.c:877`)

The `/covers/` guard (line 911–912) explicitly rejects any filename containing `%` to
block `%2e%2e`-style bypasses of the `..` check. Its own comment says: *"no
percent-encoded sequences (%2e%2e bypasses '..' check)"*.

The `POST /api/fw/flash/` guard at line 877 has no such `%` check:
```c
// CURRENT — missing % guard:
if (strstr(fname, "..") || strstr(fname, "/") || strstr(fname, "\\") ||
    fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3)) {

// CONSISTENT with covers endpoint:
if (strstr(fname, "..") || strchr(fname, '/') || strchr(fname, '\\') ||
    strchr(fname, '%') ||                          // ← add this
    fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3)) {
```

In practice FatFS does not URL-decode, so `0:/%2e%2e%2fconfig.UF2` will fail to
open and the flash operation is never attempted. However this relies on an implicit
property of the FatFS implementation as a safety net rather than an explicit guard.
If the VFS layer ever changes, the `%` bypass would become real. Add `strchr(fname,
'%')` to harden the guard to match the covers endpoint's defence-in-depth posture.

Also add a test in `FwFlashFilename`:
```cpp
TEST(FwFlashFilename, PercentEncoded_Rejected)
{
    CHECK_FALSE(fw_flash_fname_safe("%2e%2e%2fconfig.UF2"));
}
```

---

### Optimisations

**O1 — `strstr` used for single-character search in fw/flash guard** (`src/webserver.c:877`)

The fw/flash guard uses `strstr(fname, "/")` and `strstr(fname, "\\")` to detect
single characters. The covers guard correctly uses `strchr`. These are semantically
identical but `strchr` is more efficient and more idiomatic:

```c
// CURRENT:
strstr(fname, "/") || strstr(fname, "\\")

// BETTER (matches covers endpoint):
strchr(fname, '/') || strchr(fname, '\\')
```

Also update the `fw_flash_fname_safe` replica in `tests/host/test_webserver.cpp` to
match.

**O2 — Cheap `fn_len` bound checks run after three `strstr` calls** (`src/webserver.c:877–878`)

Current evaluation order: `strstr(..) || strstr(..) || strstr(..) || fn_len == 0 || fn_len >= MAX`.
For an empty filename (`fn_len == 0`) three `strstr("")` calls return NULL before the
trivial integer check is reached. Preferred order — bounds first, string search only
if needed:

```c
if (fn_len == 0 || fn_len >= (size_t)(MAX_PATH_LEN - 3) ||
    strstr(fname, "..") || strchr(fname, '/') || strchr(fname, '\\') || strchr(fname, '%')) {
```

The test replica in `fw_flash_fname_safe()` already uses this order — update
production to match.

---

### Test gaps

**T1 — `Oversized_Rejected` does not test the exact rejection boundary** (`tests/host/test_webserver.cpp`)

`Oversized_Rejected` creates a string of `MAX_PATH_LEN + 3` characters — far above
the threshold. Add two precision tests:

```cpp
TEST(FwFlashFilename, Oversized_ExactThreshold_Rejected)
{
    // fn_len == MAX_PATH_LEN - 3 is the rejection threshold (>=).
    char at[MAX_PATH_LEN - 3 + 1];
    memset(at, 'A', MAX_PATH_LEN - 3);
    at[MAX_PATH_LEN - 3] = '\0';
    CHECK_FALSE(fw_flash_fname_safe(at));
}

TEST(FwFlashFilename, Oversized_OneBelowThreshold_Accepted)
{
    // fn_len == MAX_PATH_LEN - 4 is the last accepted length.
    char at[MAX_PATH_LEN - 4 + 1];
    memset(at, 'A', MAX_PATH_LEN - 4);
    at[MAX_PATH_LEN - 4] = '\0';
    CHECK_TRUE(fw_flash_fname_safe(at));
}
```

**T2 — No test documents the `..` false-positive for legitimate double-dot filenames** (`tests/host/test_webserver.cpp`)

`strstr(fname, "..")` rejects any filename containing two consecutive dots, including
non-traversal names like `firmware..v2.UF2`. Add a test that documents this behaviour
so the trade-off is explicit and reviewers understand why such filenames are blocked:

```cpp
TEST(FwFlashFilename, DoubleDotInBasename_AlsoRejected)
{
    // strstr(..) blocks ANY double-dot, including firmware..v2.UF2
    // which cannot traverse but is rejected conservatively.
    // This is intentional — see webserver.c:877 comment.
    CHECK_FALSE(fw_flash_fname_safe("firmware..v2.UF2"));
}
```

If a future release needs to support such filenames, the fix is to check for
`/..`, `\..`, `../`, `..\` boundaries rather than bare `..`.

---

### Replica drift note (not a bug — already fixed this session)

`parse_cfg_line()` in `tests/host/test_webserver.cpp` and `cmd_name()` /
`decode_status_flags()` in `tests/host/test_logger.cpp` are orphaned replicas —
the production functions they tracked were removed. Comments updated 2026-05-31 to
document them as standalone reference implementations rather than live replicas.
No functional impact; flagged here so the next replica audit skips them.
