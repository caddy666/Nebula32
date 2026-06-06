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
# Board target
# =============================================================================
# For the standard Pico 2 (no WiFi):
#   cmake .. -DPICO_PLATFORM=rp2350
#
# For the Pico 2 W (with CYW43439 WiFi — required for the web interface):
#   cmake .. -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2_w
#
# When building for non-W Pico 2, comment out the pico_cyw43_arch_lwip_poll
# and pico_lwip library lines in target_link_libraries below.
# webserver_init() will return false silently — everything else still works.
# =============================================================================

# =============================================================================
# Upstream cd32_pico integration options
# =============================================================================
# BUILD_WITH_COMMO: Enable the COMMO 3-wire serial bus driver.
#   Required for real CD32 hardware connection (GPIO 15/16/17).
#   When OFF: COMMO bridge compiles but does nothing (safe default).
option(BUILD_WITH_COMMO   "Enable upstream COMMO 3-wire bus (real CD32 hardware)" OFF)

# BUILD_WITH_ROTARY: Enable the MCP23017-based rotary encoder disc selector.
#   Runs over I2C1 (GPIO 26/27) — no conflict with COMMO bus (GPIO 15-17).
option(BUILD_WITH_ROTARY  "Enable rotary encoder (development/standalone)"        ON)
#PICO_BOARD=pico2
# GPIO 0-17: cd32_pico hardware interface (gpio_map.h is authoritative).
# GPIO 18-29: ODE peripherals (SDIO, display, MCP23017 rotary encoder).
# COMMO (GPIO 15-17) is a self-contained module; rotary encoder is on the
# MCP23017 I2C expander (GPIO 28-29), so COMMO and rotary never conflict.

# =============================================================================
# Compile-time pin / buffer configuration
# =============================================================================
# Pin definitions for the cd32_pico hardware interface (GPIO 0-17) live in
# upstream/include/gpio_map.h, which is on the include path for all sources.
# The compile definitions below cover ODE peripherals (GPIO 18-29) and
# CD32 26-pin drive connector signals — see gpio_map.h for the full layout.
add_compile_definitions(
    # ---- CD32 26-pin drive connector signals (GPIO 0-17) ----
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
    #PASSIVE_PIN=13          # Drive passive  not actually connected to anytihng - reuse this pin. (connector pin 23, output)
    CXD_RESET_PIN=14        # /RESET from host (connector pin 7, input)

    # ---- SD card — 4-bit SDIO (GPIO 18-23) ----
    # Matches PIN_SDIO_* in gpio_map.h.
    SDIO_CLK_PIN=18
    SDIO_CMD_PIN=19
    SDIO_D0_PIN=20
    SDIO_D1_PIN=21
    SDIO_D2_PIN=22
    SDIO_D3_PIN=23

    # ---- ST7789 240x240 display — hardware SPI1 (GPIO 13, 24, 26-27) ----
    # CS is on GPIO 13 (free in cd32_pico layout).
    # GPIO 25 is free (onboard LED not used in merged firmware).
    # GPIO 26/27: SPI1 SCK and MOSI.
    # Matches PIN_ST7789_* in gpio_map.h.
    ST7789_DC_PIN=24          # Data/Command select (GPIO out)
    ST7789_CS_PIN=13          # Chip select, active-low (GPIO out)
    ST7789_SCK_PIN=26         # SPI1 clock
    ST7789_MOSI_PIN=27        # SPI1 MOSI (DIN)

    # ---- MCP23017 I2C GPIO expander — rotary encoder (GPIO 28-29) ----
    # I2C0 on GPIO 28/29 (SPI1 occupies 26/27, ruling out I2C1).
    # Encoder CLK/DT/SW connect to MCP23017 GPA0-GPA2.
    # Matches PIN_MCP23017_* in gpio_map.h.
    MCP23017_SDA_PIN=28       # I2C0 SDA → MCP23017 SDA
    MCP23017_SCL_PIN=29       # I2C0 SCL → MCP23017 SCL
    MCP23017_I2C_ADDR=0x20    # MCP23017 address (A2=A1=A0=GND)

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
add_subdirectory(libs/wolfssl-master EXCLUDE_FROM_ALL)

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
    hardware_i2c            # I2C0 for MCP23017 GPIO expander
    hardware_spi            # SPI1 for ST7789 display
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
    target_compile_definitions(Nebula32 PRIVATE PICO_CYW43_SUPPORTED=1)

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

# ---- stdio via USB CDC (disable UART to free those pins for user GPIOs) ----
pico_enable_stdio_usb(Nebula32 1)
pico_enable_stdio_uart(Nebula32 0)

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

