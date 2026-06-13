cmake_minimum_required(VERSION 3.13)

# =============================================================================
# Nebula32 — CD32 Optical Drive Emulator
# Target: Raspberry Pi Pico 2 (RP2350)
# =============================================================================

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(Nebula32 C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# =============================================================================
# Board target — Waveshare Core2350B0 (RP2350B) + RM2 WiFi module
# =============================================================================
# Target hardware: Waveshare Core2350B0 with external RM2 (CYW43439) WiFi.
#
# Build with WiFi (required for web interface):
#   cmake .. -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2_w -DBUILD_WITH_PSRAM=ON
#
# Note: pico2_w targets RP2350A in the SDK board file.  The Core2350B0 uses
# RP2350B (GPIO 0–47).  GPIOs 30–47 are accessed directly — they work on the
# physical RP2350B regardless of the board file.  A custom board file
# (boards/core2350b0_w.h) can be added later for full SDK-level GPIO awareness.
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

# BUILD_WITH_ROTARY: Enable the MCP23017-based rotary encoder disc selector.
#   Runs over I2C1 (GPIO 26/27) — no conflict with COMMO bus (GPIO 44-46).
set(BUILD_WITH_ROTARY  ON)

# GPIO 0-14:  CD32 connector signals (gpio_map.h is authoritative).
# GPIO 16-17: UART0 TX/RX (debug serial — stdio).
# GPIO 20-21: UART1 TX/RX (auxiliary serial).
# GPIO 23-25,29: WiFi RM2 (CYW43439).
# GPIO 26-27: I2C1 SDA/SCL (MCP23017 rotary encoder).
# GPIO 30-35: SDIO (SD card).
# GPIO 40-43: ST7789 display (SPI1).
# GPIO 44-46: COMMO 3-wire bus (PIO1 SM0/SM1) — moved from 15-17 for UART.
# GPIO 47:    PSRAM CS1.

# =============================================================================
# Compile-time pin / buffer configuration
# =============================================================================
# Pin definitions for the full GPIO map (GPIO 0-47, Core2350B0 / RP2350B) live in
# upstream/include/gpio_map.h, which is on the include path for all sources.
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

    # ---- MCP23017 I2C GPIO expander — rotary encoder (I2C1, GPIO 26-27) ----
    # Moved from I2C0/GPIO 28-29: GPIO 29 is now WiFi RM2 SPI CLK.
    # I2C1 SDA=26, SCL=27 are valid I2C1 function pins on RP2350.
    # Matches PIN_MCP23017_* in gpio_map.h.
    MCP23017_SDA_PIN=26       # I2C1 SDA → MCP23017 SDA
    MCP23017_SCL_PIN=27       # I2C1 SCL → MCP23017 SCL
    MCP23017_I2C_ADDR=0x20    # MCP23017 address (A2=A1=A0=GND)

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

if(NOT EXISTS "${FATFS_SDK_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "\n"
        "================================================================\n"
        "  no-OS-FatFS-SD-SDIO-SPI-RPi-Pico not found at:\n"
        "    ${FATFS_SDK_PATH}\n"
        "  Clone it into libs/ with:\n"
        "    git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git libs/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico\n"
        "================================================================\n")
endif()

add_subdirectory(${FATFS_SDK_PATH}/src fatfs_sd)

# MCP23017 I2C GPIO expander (adamboardman/pico-mcp23017)
add_subdirectory(libs/pico-interfaces)
add_subdirectory(libs/pico-mcp23017)

# wolfSSL TLS engine (pico2_w only; EXCLUDE_FROM_ALL means it is only built when linked)
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

    # ---- Rotary encoder driver (via MCP23017 I2C expander) ----
    src/rotary_mcp.cpp

    # ---- Disc selector UI (encoder event processing + LED patterns) ----
    src/ui.c

    # ---- HTTP web server for browser-based disc selection ----
    src/webserver.c

    # ---- Visualiser: FFT + demoscene effects + DA audio snoop ----
    src/fft.c
    src/effects.c
    src/vis_audio.c

    # ---- COMMO bus bridge (upstream cd32_pico integration) ----
    src/commo_bridge.c

    # ---- Upstream: COMMO serial bus state machine ----
    upstream/core/commo.c
    upstream/core/commo_hal_pico.c
    upstream/core/dispatcher.c
    upstream/core/cmd_hndl.c
    upstream/core/sts_q_id.c

    # ---- Upstream: BCD/time arithmetic and software timers ----
    upstream/utils/maths.c
    upstream/utils/timer.c

    # ---- Upstream player shim: satisfies player_interface linkage for upstream cmd_hndl.c ----
    src/upstream_player_shim.c

)

# ---- PIO programs (assembled at build time by pioasm) ----
pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/da_output.pio)
pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/subcode_encoder.pio)

# ---- Upstream PIO programs (COMMO bus) ----
pico_generate_pio_header(Nebula32
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/pio/commo.pio)

# ---- Include directories ----
target_include_directories(Nebula32 PRIVATE
    include
    upstream/include          # upstream defs.h, gpio_map.h, commo.h, etc.
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
    hardware_i2c            # I2C1 for MCP23017 GPIO expander
    hardware_uart           # UART0 debug + UART1 auxiliary
    hardware_spi            # SPI1 for ST7789 display
    hardware_interp         # INTERP0 blend mode for effects.c gradient lerp
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico   # SD card FatFS (from no-OS-FatFS library)
    pico_mcp23017           # MCP23017 I2C GPIO expander (adamboardman library)

)

# ---- Pico 2 W WiFi + lwIP + TLS — only link when building for pico2_w ----
if(PICO_BOARD STREQUAL "pico2_w")
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

    # TLS certificate check — runs every pico2_w build.
    # Generates certs/server.{crt,key}.der and include/tls_cert.h if missing or expiring.
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

# ---- Compiler warnings ----
target_compile_options(Nebula32 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter   # Several callbacks take unused context pointers
    -O2                     # Optimise for speed (important for Core 1 latency)
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

