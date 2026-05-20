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
