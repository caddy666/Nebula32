cmake_minimum_required(VERSION 3.13)

# =============================================================================
# Nebula32 — CD32 Optical Drive Emulator
# Target: Raspberry Pi Pico 2 (RP2350)
# =============================================================================

# Custom board headers (boards/core2350b0_w.h) must be discoverable before the
# SDK resolves PICO_BOARD.  Must be set prior to pico_sdk_init().
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(Nebula32 C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # emit compile_commands.json for clang-tidy

pico_sdk_init()

# WiFi/TLS is built for any CYW43-equipped board: the Pico 2 W and our custom
# Core2350B0 + RM2 board (which inherits the pico2_w radio).  Gate the WiFi/wolfSSL
# link block on this rather than a single board name, so renaming the board target
# never silently drops the radio.
if(PICO_BOARD STREQUAL "pico2_w" OR PICO_BOARD STREQUAL "core2350b0_w")
    set(NEBULA_WIFI ON)
endif()

# =============================================================================
# Board target — Waveshare Core2350B0 (RP2350B) + RM2 WiFi module
# =============================================================================
# Target hardware: Waveshare Core2350B0 with external RM2 (CYW43439) WiFi.
# Preferred build (use the wrapper, which selects the correct board):
#   ./build.sh core2350b            # → build-core2350b/  (WiFi + PSRAM)
#
# Build with WiFi (web interface), explicit cmake form:
#   cmake .. -DPICO_PLATFORM=rp2350 -DPICO_BOARD=core2350b0_w -DBUILD_WITH_PSRAM=ON
#
# Board file: boards/core2350b0_w.h inherits the pico2_w radio pinout but sets
# PICO_RP2350A=0 so the SDK knows this is the 48-GPIO RP2350B.  Building as plain
# pico2_w forces PICO_RP2350A=1 (NUM_BANK0_GPIOS=30): GPIO 30–47 (SDIO/display/
# COMMO/PSRAM) then only work in release builds (param-asserts compiled out) and
# panic in any debug build.  Always build via core2350b0_w, not pico2_w.
#
# Build without WiFi (COMMO + SD only, no web interface):
#   cmake .. -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 -DBUILD_WITH_PSRAM=ON
# =============================================================================

# =============================================================================
# Upstream cd32_pico integration options
# =============================================================================
# BUILD_WITH_COMMO: Enable the COMMO 3-wire serial bus driver.
#   Required for real CD32 hardware connection (GPIO 44/45/46).
#   When OFF: COMMO bridge compiles but does nothing (safe default).
set(BUILD_WITH_COMMO   ON)

# BUILD_WITH_ROTARY: Enable the direct GPIO rotary encoder disc selector.
#   Encoder on GPIO 12/15/18/19 — no conflict with COMMO bus (GPIO 44-46).
set(BUILD_WITH_ROTARY  ON)

# GPIO 0-14:  CD32 connector signals (gpio_map.h is authoritative).
# GPIO 16-17: UART0 TX/RX (debug serial — stdio).
# GPIO 20-21: UART1 TX/RX (auxiliary serial).
# GPIO 23-25,29: WiFi RM2 (CYW43439).
# GPIO 12/15/18/19: Direct GPIO rotary encoder (ENC_A/B/SW/LOG).
# GPIO 30-35: SDIO (SD card).
# GPIO 40-43: ST7789 display (SPI1).
# GPIO 44-46: COMMO 3-wire bus (PIO1 SM0/SM1) — moved from 15-17 for UART.
# GPIO 47:    PSRAM CS1.

# =============================================================================
# Compile-time pin / buffer configuration
# =============================================================================
# Pin definitions for the full GPIO map (GPIO 0-47, Core2350B0 / RP2350B) live in
# include/gpio_map.h, which is on the include path for all sources.
# The compile definitions below duplicate the numeric values for code that
# uses -D defines rather than including gpio_map.h directly (da_output.c,
# display.cpp, hw_config.c etc.).  Values must stay in sync with gpio_map.h.
add_compile_definitions(
    # ---- CD32 26-pin drive connector signals (GPIO 0-14) ----
    DA_DATA_PIN=0           # DA serial data  (connector pin 9)
    DA_BCLK_PIN=1           # DA bit clock    (connector pin 11)
    DA_LRCLK_PIN=2          # DA LR clock     (connector pin 8)
    DA_C2PO_PIN=3           # DA C2 pointer   (connector pin 12)
    DA_EMPH_PIN=4           # DA emphasis     (connector pin 13)
    SUB_DATA_PIN=5          # Subcode data    (connector pin 17)
    SUB_CLK_PIN=6           # Subcode clock   (connector pin 18)
    SUB_WFCLK_PIN=7         # Subcode WFCLK   (connector pin 14)
    SUB_SCOR_PIN=8          # Subcode SCOR    (connector pin 15)
    M17SINE_PIN=9           # 16.9344 MHz ref (connector pin 5, GPIN0 input)
    ACTIVE_PIN=10           # Drive active    (connector pin 24, output)
    # PIN_RESET=14 is defined in gpio_map.h — no duplicate compile-define needed

    # ---- Debug UARTs ----
    PICO_DEFAULT_UART=0       # stdio on UART0
    PICO_DEFAULT_UART_TX_PIN=16  # GPIO 16 — matches PIN_UART0_TX in gpio_map.h
    PICO_DEFAULT_UART_RX_PIN=17  # GPIO 17 — matches PIN_UART0_RX in gpio_map.h

    # ---- SD card — 4-bit SDIO (GPIO 30-35) ----
    # CLK must be D0−2 (mod 32): with D0=32, CLK=(32+30)%32=30. Verified.
    # Matches PIN_SDIO_* in gpio_map.h.
    SDIO_CLK_PIN=30
    SDIO_CMD_PIN=31
    SDIO_D0_PIN=32
    SDIO_D1_PIN=33
    SDIO_D2_PIN=34
    SDIO_D3_PIN=35

    # ---- ST7789 240x240 display — hardware SPI1 (GPIO 40-43) ----
    # SPI1 CSn→41, SCK→42, TX→43 are all valid RP2350 SPI1 function pins.
    # DC is plain GPIO (no SPI function constraint) → GPIO 40.
    # Matches PIN_ST7789_* in gpio_map.h.
    ST7789_DC_PIN=40          # Data/Command select (GPIO out)
    ST7789_CS_PIN=41          # Chip select, active-low (SPI1 CSn)
    ST7789_SCK_PIN=42         # SPI1 clock
    ST7789_MOSI_PIN=43        # SPI1 MOSI (DIN) — only valid option: 39=LED, 47=PSRAM

    # ---- Direct GPIO rotary encoder — GPIO 12/15/18/19 ----
    # No MCP23017 I2C expander; encoder connected directly to RP2350B GPIOs.
    # Matches PIN_ENC_A/B/SW/LOG in gpio_map.h.
    ENC_A_PIN=12              # Encoder quadrature A (CLK)
    ENC_B_PIN=15              # Encoder quadrature B (DT)
    ENC_SW_PIN=18             # Encoder push-button (active-low)
    ENC_LOG_PIN=19            # Logger toggle button (active-low)

    # ---- Red LED (RM2 direct GPIO) ----
    LED_RED_PIN=39            # matches PIN_LED_RED in gpio_map.h

    # ---- Buffer sizing ----
    SECTOR_BUFFER_COUNT=8   # Number of 2352-byte sectors in the prefetch ring
    MAX_TRACKS=99
    MAX_PATH_LEN=256
    MAX_IMAGES=32
    BUILD_WITH_COMMO=$<BOOL:${BUILD_WITH_COMMO}>

    # ---- Optional: enable EDC verification on Mode 1 sectors ----
    # (uses ~1 KB extra flash for the EDC computation loop)
    # CDROM_VERIFY_EDC
)

# =============================================================================
# External libraries (all under libs/)
# =============================================================================

# SD card — no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (Carl Kugler)
#   https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
set(FATFS_SDK_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico"
    CACHE PATH "Path to no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library")

# Sentinel: src/CMakeLists.txt is the upstream entry point used by add_subdirectory
# below (the repo has no root CMakeLists.txt). It's a git submodule — an empty dir
# means it hasn't been initialised.
if(NOT EXISTS "${FATFS_SDK_PATH}/src/CMakeLists.txt")
    message(FATAL_ERROR
        "\n"
        "================================================================\n"
        "  no-OS-FatFS-SD-SDIO-SPI-RPi-Pico not found at:\n"
        "    ${FATFS_SDK_PATH}\n"
        "  It is a git submodule — initialise it with:\n"
        "    git submodule update --init libs/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico\n"
        "================================================================\n")
endif()

add_subdirectory(${FATFS_SDK_PATH}/src fatfs_sd)

# wolfSSL TLS engine (CYW43 boards only; EXCLUDE_FROM_ALL means it is only built when linked)
# user_settings.h in include/ shadows the vendor copy via WOLFSSL_USER_SETTINGS=ON.
set(WOLFSSL_USER_SETTINGS   ON  CACHE BOOL "" FORCE)
set(WOLFSSL_SINGLE_THREADED ON  CACHE BOOL "" FORCE)
# Without this, wolfSSL's install(EXPORT wolfssl-targets) requires every target
# linked into wolfssl to be exported too — which forbids linking pico-sdk
# interface targets below.
set(WOLFSSL_INSTALL         "no" CACHE STRING "" FORCE)
add_subdirectory(libs/wolfssl-master EXCLUDE_FROM_ALL)
# WOLFSSL_USER_SETTINGS makes every wolfcrypt source #include "user_settings.h",
# but the vendor target has no path to our shadow copy in include/ — inject it
# here rather than editing vendor files.  wolfio.c's WOLFSSL_LWIP_NATIVE path
# additionally needs lwIP headers, whose arch/cc.h pulls pico.h and pico/rand.h —
# link the *_headers interface targets (headers only: linking pico_lwip_core
# itself would compile a second copy of the lwIP sources into wolfssl and
# collide with Nebula32's own lwIP objects).
if(TARGET wolfssl)
    target_include_directories(wolfssl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(wolfssl PRIVATE
        pico_lwip_core_headers
        pico_lwip_arch_headers   # arch/cc.h port glue
        pico_base_headers
        pico_rand_headers)
endif()

# =============================================================================
# Main executable
# =============================================================================
add_executable(Nebula32
    # ---- Entry point ----
    src/main.c

    # ---- DA serial output PIO driver (DA_DATA/BCLK/LRCLK → Akiko + LC78835M) ----
    src/da_output.c

    # ---- Multi-format disc image parser (ISO / BIN / NRG / MDF) ----
    src/disc_image.c

    # ---- Virtual disc ISO 9660 synthesiser (SD partition 2 → virtual CD) ----
    src/virtual_disc.c

    # ---- Read-ahead sector ring buffer ----
    src/sector_cache.c

    # ---- Q-channel subcode generator ----
    src/subcode.c

    # ---- SD card SDIO mount and image scanner ----
    src/sd_card.c

    # ---- SD card hardware configuration (required by no-OS-FatFS) ----
    src/hw_config.c

    # ---- Hardware self-test ----
    src/selftest.c

    # ---- EDC/ECC computation for synthesised Mode 1 sectors ----
    src/ecc.c

    # ---- Persistent configuration stored in RP2350 flash ----
    src/config.c

    # ---- SD-card UF2 firmware self-update ----
    src/fw_update.c

    # ---- SD card activity logger (writes cd32_cd.log, reads Nebula32.cfg) ----
    src/logger.c

    # ---- ST7789 display + JPEGDEC cover art ----
    src/display.cpp
    libs/JPEGDEC/src/JPEGDEC.cpp

    # ---- Rotary encoder driver (direct GPIO — no MCP23017 expander) ----
    src/rotary_gpio.c

    # ---- Disc selector UI (encoder event processing + LED patterns) ----
    src/ui.c

    # ---- Multi-disc carousel model + .m3u playlist parser ----
    src/carousel.c

    # ---- HTTP web server for browser-based disc selection ----
    src/webserver.c

    # ---- Visualiser: FFT + demoscene effects + DA audio snoop ----
    src/fft.c
    src/effects.c
    src/vis_audio.c

    # ---- COMMO bus: bridge, state machine, BCD arithmetic, timers ----
    src/commo_bridge.c
    src/commo.c
    src/commo_hal_pico.c

)

# ---- PIO programs (assembled at build time by pioasm) ----
pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/da_output.pio)
pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/subcode_encoder.pio)

pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/commo.pio)

# ---- Include directories ----
target_include_directories(Nebula32 PRIVATE
    include
    ${FATFS_SDK_PATH}/include
    ${FATFS_SDK_PATH}/src/include
    ${FATFS_SDK_PATH}/src/ff15/source
    ${FATFS_SDK_PATH}/src/sd_driver
    libs/JPEGDEC/src           # JPEGDEC JPEG decoder
    ${CMAKE_CURRENT_BINARY_DIR}   # generated .pio.h headers from pioasm
)

# ---- Libraries ----
target_link_libraries(Nebula32
    pico_stdlib
    pico_multicore          # Dual-core launch (Core 1 bus handler)
    hardware_pio            # PIO for DA output and subcode encoder
    hardware_dma            # DMA for sector transfers and I2S audio
    hardware_irq            # Fast IRQ for DMA completion (audio ping-pong)
    hardware_timer          # Repeating 1 ms timer for state machine
    hardware_gpio
    hardware_clocks         # set_sys_clock_khz()
    hardware_flash          # flash_range_erase/program for config persistence and fw_update
    hardware_sync           # save_and_disable_interrupts for flash writes
    hardware_watchdog       # watchdog_reboot() for fw_update self-reboot
    hardware_uart           # UART0 debug + UART1 auxiliary
    hardware_spi            # SPI1 for ST7789 display
    hardware_interp         # INTERP0 blend mode for effects.c gradient lerp
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico   # SD card FatFS (from no-OS-FatFS library)

)

# ---- WiFi + lwIP + TLS — linked for any CYW43 board (pico2_w / core2350b0_w) ----
if(NEBULA_WIFI)
    target_link_libraries(Nebula32
        pico_cyw43_arch_lwip_poll
        wolfssl
        pico_rand   # RP2350 TRNG — required by wolfSSL Pico port
    )
    # wolfSSL Pico TRNG port — not auto-included by wolfSSL CMake
    target_sources(Nebula32 PRIVATE
        libs/wolfssl-master/wolfcrypt/src/port/rpi_pico/pico.c
    )
    # Vendor bug: pico.c uses BAD_FUNC_ARG without including error-crypt.h.
    # Force-include it rather than editing the vendor file.
    set_source_files_properties(libs/wolfssl-master/wolfcrypt/src/port/rpi_pico/pico.c
        PROPERTIES COMPILE_OPTIONS "-include;wolfssl/wolfcrypt/error-crypt.h")
    # WOLFSSL_USER_SETTINGS: the vendor CMake defines this only on its own
    # target — Nebula32's wolfSSL-consuming TUs (webserver.c and the pico.c
    # TRNG port above) need it too or settings.h skips user_settings.h.
    target_compile_definitions(Nebula32 PRIVATE
        PICO_CYW43_SUPPORTED=1
        WOLFSSL_USER_SETTINGS)

    # TLS certificate check — runs every WiFi build.
    # Generates SDCARD_BASE/certs/nebula32.{crt,key}.der (the SD-card skeleton) and
    # include/tls_cert.h (compiled-in fallback) if missing or expiring.
    add_custom_target(tls_cert_check ALL
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_tls_cert.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking TLS certificate validity"
        VERBATIM
    )
    add_dependencies(Nebula32 tls_cert_check)
endif()

# ---- Optional QSPI PSRAM (RP2350 CS1, e.g. APS6404L 8 MB) ----
# Enable on custom hardware that wires a PSRAM chip to the RP2350 QMI CS1 pin.
# Standard Pico 2 / Pico 2 W builds leave this OFF — all code compiles to no-ops.
#
# Usage:
#   cmake .. -DPICO_BOARD=pico2 -DBUILD_WITH_PSRAM=ON
#   cmake .. -DPICO_BOARD=pico2_w -DBUILD_WITH_PSRAM=ON
#
# Before enabling, edit src/psram.c:
#   PSRAM_CS1_PIN  — GPIO wired to PSRAM /CE on your PCB
#   PSRAM_CLKDIV   — QSPI clock divider (2 = 67.5 MHz for APS6404L at 135 MHz)
#   PSRAM_RXDELAY  — RX sample delay half-cycles (2–4 typical)
#   QMI timing constants in psram_fw_init() for your specific chip
set(BUILD_WITH_PSRAM ON)
if(BUILD_WITH_PSRAM)
    target_compile_definitions(Nebula32 PRIVATE BUILD_WITH_PSRAM=1)
    target_sources(Nebula32 PRIVATE src/psram.c)
    # SDK 2.x provides hardware_psram; if your SDK version lacks it, remove this line
    # and configure the QMI registers directly in src/psram.c (which the code already does).
    if(TARGET hardware_psram)
        target_link_libraries(Nebula32 hardware_psram)
    endif()
    # Slot count is a C define (SECTOR_BUFFER_COUNT_PSRAM in include/psram.h),
    # not a CMake variable — don't try to interpolate it here.
    message(STATUS "PSRAM: enabled — sector cache expanded (see SECTOR_BUFFER_COUNT_PSRAM in include/psram.h)")
endif()

# ---- stdio: USB CDC primary + UART0 mirror (GPIO 16/17) ----
# Both enabled: printf goes to USB CDC for interactive console
# AND to UART0 for bench debugging via a USB-serial adapter on GPIO 16/17.
pico_enable_stdio_usb(Nebula32 1)
pico_enable_stdio_uart(Nebula32 1)

# ---- Extra outputs: .uf2 for drag-and-drop flashing, .bin, .hex ----
pico_add_extra_outputs(Nebula32)

# JPEGDEC uses #ifdef __LINUX__ to avoid including <Arduino.h>
set_source_files_properties(libs/JPEGDEC/src/JPEGDEC.cpp PROPERTIES
    COMPILE_DEFINITIONS "__LINUX__"
)

# ---- Debug build toggle ----
# NEBULA_DEBUG=ON (via `./build.sh <board> --debug`) swaps the baseline -O2 for
# -Og -g3 so SWD/gdb stepping is clean (most locals live, sane line tables).
# NOT timing-accurate — DA/COMMO real-time behaviour still can't be breakpointed
# (see openocd.md). Builds into a separate *-debug/ dir so it never clobbers the
# release cache. The per-file -O3 compute kernels below are left untouched.
option(NEBULA_DEBUG "Build with -Og -g3 for SWD/gdb debugging" OFF)
if(NEBULA_DEBUG)
    set(NEBULA_OPT_LEVEL -Og -g3)
    message(STATUS "Nebula32: DEBUG build (-Og -g3) — for gdb stepping, not timing-accurate")
else()
    set(NEBULA_OPT_LEVEL -O2)   # Optimise for speed (important for Core 1 latency)
endif()

# ---- Compiler warnings ----
target_compile_options(Nebula32 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter   # Several callbacks take unused context pointers
    # Extra warnings not in -Wall/-Wextra. -Wformat=2 catches %u-vs-uint32_t
    # (uint32_t is 'unsigned long' on this ABI); -Wdouble-promotion catches
    # silent float->double on the M33's single-precision FPU.
    -Wformat=2
    -Wdouble-promotion
    -Wshadow
    -Wpointer-arith
    -Wnull-dereference
    ${NEBULA_OPT_LEVEL}     # -O2 (release) or -Og -g3 (NEBULA_DEBUG)
    -fstack-usage           # Emit .su files — per-function stack depth for RP2350 SRAM auditing
)

# Pure-compute kernels (no I/O, loop-dominated) get -O3: the unrolling pays off
# and the extra code size stays off the XIP-resident hot path.  Everything else
# keeps -O2 to limit XIP cache pressure.  -flto is deliberately NOT used: it
# breaks pico-sdk's linker symbol wrapping (--wrap=printf etc.).
set_source_files_properties(
    src/ecc.c       # ECMA-130 P/Q parity — byte-loop dominated
    src/fft.c       # 256-pt Q15 radix-2 butterflies
    src/effects.c   # per-pixel scanline rendering
    PROPERTIES COMPILE_OPTIONS "-O3"
)

