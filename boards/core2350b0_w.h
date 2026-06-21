/*
 * core2350b0_w.h — Waveshare Core2350B0 (RP2350B) + Raspberry Pi RM2 (CYW43439)
 *
 * -----------------------------------------------------
 * NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER, SO IT
 *       MUST CONSIST ONLY OF PREPROCESSOR DIRECTIVES.
 * -----------------------------------------------------
 *
 * Why this file exists:
 *   The stock pico2_w board file targets the RP2350A (30-GPIO QFN-60) and hard-
 *   defines PICO_RP2350A=1.  That sets NUM_BANK0_GPIOS=30 (see the SDK's
 *   platform_defs.h).  The Core2350B0 carries the RP2350B (48-GPIO QFN-80), and
 *   Nebula32 uses pins 30-47 for SDIO, the display, COMMO and PSRAM.  Building
 *   as pico2_w "works" only because release builds compile out the GPIO range
 *   asserts — any debug build panics, and SDK loops over NUM_BANK0_GPIOS skip the
 *   high pins.  This board file tells the SDK the truth.
 *
 * What it keeps from pico2_w:
 *   The RM2 radio module is a CYW43439 wired to the exact Pico 2 W pinout
 *   (WL_REG_ON=23, WL_DATA=24, WL_CS=25, WL_CLK=29), so we inherit the entire
 *   CYW43 configuration unchanged and only correct the chip variant + GPIO count.
 */

#ifndef _BOARDS_CORE2350B0_W_H
#define _BOARDS_CORE2350B0_W_H

/* The SDK extracts pico_board_cmake_set() directives by TEXT-SCANNING this file
 * (the header named by PICO_BOARD); it does NOT follow #include.  So the
 * directives that pico2_w.h sets must be restated literally here, or the CMake
 * side never learns the board is CYW43-capable and the pico_cyw43_arch include
 * paths fail to propagate (webserver.c then can't find pico/cyw43_arch.h) even
 * though the C-preprocessor -D is present.  These lines are no-ops at C/asm
 * compile time (the SDK defines the macros away). */
pico_board_cmake_set(PICO_PLATFORM, rp2350)
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)

/* Inherit the Pico 2 W radio pinout, flash stage2, UART/SPI/I2C defaults, and
 * the full CYW43 (RM2) configuration.  Supported inheritance pattern — pico2_w.h
 * documents that it may be included by other board headers as "boards/pico2_w.h".*/
#include "boards/pico2_w.h"

/* --- Correct the RP2350 variant: B (48 GPIO), not A (30 GPIO). --- */
/* pico2_w.h does an unconditional `#define PICO_RP2350A 1`, so undef first. */
#undef  PICO_RP2350A
#define PICO_RP2350A 0

/* Board detection hook for application code that wants to special-case this PCB. */
#define WAVESHARE_CORE2350B0_W

/* --- Flash size: 16 MB on this Core2350B0 ---
 * fw_update.c's dual-bank (Bank0/Bank1) layout and the bootrom partition layout
 * derive from PICO_FLASH_SIZE_BYTES.  pico2_w.h defaults to 4 MB; override to the
 * board's real 16 MB so the self-update banks and any partition table use the
 * full device.  Must override BOTH the cmake-default directive (text-scanned, not
 * #include-followed — see the PICO_CYW43_SUPPORTED note above) and the C macro. */
pico_board_cmake_set(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#undef  PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)

#endif /* _BOARDS_CORE2350B0_W_H */
