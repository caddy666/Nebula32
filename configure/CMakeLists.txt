cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350-arm-s)
# =============================================================================
# CD32 Optical Drive Emulator — Sony CXD2545Q Emulation
# Target: Raspberry Pi Pico 2 (RP2350)
# =============================================================================
    if(NOT PICO_BOARD)
        set(PICO_BOARD pico2w)
        message("PICO_BOARD HASN'T BEEN DECLARED. SETTING IT TO pico2w")
    endif()

    if(NOT PICO_PLATFORM)
        set(PICO_PLATFORM rp2350-arm-s)
        message("PICO_PLATFORM HASN'T BEEN DECLARED. SETTING IT TO rp2350-arm-s")
    endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(cd32_ode C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()
#set PICO_PLATFORM=rp2350
#set PICO_BOARD=pico2_w
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

# BUILD_WITH_UPSTREAM_SERVO: Include the real hardware servo/play modules.
#   Only useful when BUILD_WITH_COMMO=ON and a real CXD2500BQ is wired up.
option(BUILD_WITH_UPSTREAM_SERVO "Include DSIC2/servo modules (real hardware)" OFF)

# =============================================================================
# Compile-time pin / buffer configuration
# =============================================================================
add_compile_definitions(
    CXD_RESET_PIN=14        # RESET from CD32 26-pin connector pin 7

    # ---- Subcode serial output (optional — for external subcode connector) ----
    CXD_SUBCODE_PIN=24      # Serialised subcode output (GPIO24, optional)

    # ---- Rotary encoder disc selector ----
    ROTARY_CLK_PIN=15      # Encoder A (CLK) — with pull-up, triggers on falling edge
    ROTARY_DT_PIN=16       # Encoder B (DT)  — with pull-up, direction detection
    ROTARY_SW_PIN=17       # Encoder push-button — active low, with pull-up

    # ---- SD card — 4-bit SDIO ----
    SDIO_CLK_PIN=18
    SDIO_CMD_PIN=19
    SDIO_D0_PIN=20
    SDIO_D1_PIN=21
    SDIO_D2_PIN=22
    SDIO_D3_PIN=23

    # ---- WiFi (Pico 2 W — CYW43439 internal SPI, no user GPIO needed) ----
    # The WiFi chip is connected to the RP2350 via an internal SPI bus managed
    # by the pico_cyw43_arch library.  No GPIO pin numbers required here.
    # Use pico_cyw43_arch_lwip_poll in CMake target_link_libraries.

    # ---- I2S audio DAC (PCM5102A or compatible) ----
    I2S_DATA_PIN=26          # I2S data output (BCK=DATA+1=27, LRCK=DATA+2=28)

    # ---- Miscellaneous ----
    LED_PIN=25              # Onboard LED
    DEBUG_UART_TX_PIN=16    # UART0 TX (spare — not wired to CD32)
    DEBUG_UART_RX_PIN=17    # UART0 RX

    # ---- Buffer sizing ----
    SECTOR_BUFFER_COUNT=8   # Number of 2352-byte sectors in the prefetch ring
    MAX_TRACKS=99
    MAX_PATH_LEN=256
    MAX_IMAGES=32
    BUILD_WITH_COMMO=$<BOOL:${BUILD_WITH_COMMO}>
    BUILD_WITH_ROTARY=$<BOOL:${BUILD_WITH_ROTARY}>
    BUILD_WITH_UPSTREAM_SERVO=$<BOOL:${BUILD_WITH_UPSTREAM_SERVO}>

    # ---- Optional: enable EDC verification on Mode 1 sectors ----
    # (uses ~1 KB extra flash for the EDC computation loop)
    # CDROM_VERIFY_EDC
)

# =============================================================================
# SD card library — no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (Carl Kugler)
# Clone this repository alongside the project directory:
#   git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git
# =============================================================================
set(FATFS_SDK_PATH
    "/home/caddy/pico-claude/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src/"
    CACHE PATH "/home/caddy/pico-claude/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src/")

#if(NOT EXISTS "${FATFS_SDK_PATH}/CMakeLists.txt")
#    message(FATAL_ERROR
#        "\n"
#        "==poo===============================================================\n"
#        "  no-OS-FatFS-SD-SDIO-SPI-RPi-Pico not found at:\n"
#        "    ${FATFS_SDK_PATH}\n"
#	" YAH::  ${CMAKE_CURRENT_SOURCE_DIR}\n"
#        "  Clone it with:\n"
#        "    git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git\n"
#        "  Place it BESIDE (not inside) the cd32_ode directory.\n"
#        "=================================================================\n")
#endif()

add_subdirectory(${FATFS_SDK_PATH} fatfs_sd)

# =============================================================================
# Main executable
# =============================================================================
add_executable(cd32_ode
    # ---- Entry point ----
    src/main.c

    # ---- CXD2545Q register and command emulation ----
    src/cxd2545q.c

    # ---- CD-ROM protocol layer (XA filter, sector routing, EDC) ----
    src/cdrom.c

    # ---- Multi-format disc image parser (ISO / BIN / NRG / MDF) ----
    src/disc_image.c

    # ---- Read-ahead sector ring buffer ----
    src/sector_cache.c

    # ---- Q-channel subcode generator ----
    src/subcode.c

    # ---- SD card SDIO mount and image scanner ----
    src/sd_card.c

    # ---- SD card hardware configuration (required by no-OS-FatFS) ----
    src/hw_config.c

    # ---- I2S audio output (PCM5102A DAC via PIO + DMA ping-pong) ----
    src/audio.c

    # ---- Hardware self-test ----
    src/selftest.c

    # ---- EDC/ECC computation for synthesised Mode 1 sectors ----
    src/ecc.c

    # ---- Persistent configuration stored in RP2350 flash ----
    src/config.c

    # ---- SD card activity logger (writes cd32_cd.log, reads cd32_ode.cfg) ----
    src/logger.c

    # ---- Rotary encoder driver (disc selector hardware input) ----
    src/rotary.c

    # ---- Disc selector UI (encoder event processing + LED patterns) ----
    src/ui.c

    # ---- HTTP web server for browser-based disc selection ----
    src/webserver.c

    # ---- COMMO bus bridge (upstream cd32_pico integration) ----
    src/commo_bridge.c

    # ---- Upstream: COMMO serial bus state machine ----
    upstream/core/commo.c
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
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/subcode_encoder.pio)
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/pio/i2s_audio.pio)

# ---- Upstream PIO programs (COMMO bus + CXD2500BQ + DSIC2 + Q-channel) ----
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/pio/commo.pio)
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/pio/cxd2500_tx.pio)
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/pio/dsic2.pio)
pico_generate_pio_header(cd32_ode
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/pio/qchannel_rx.pio)

# ---- Include directories ----
target_include_directories(cd32_ode PRIVATE
    include
    upstream/include          # upstream defs.h, gpio_map.h, commo.h, etc.
    ${FATFS_SDK_PATH}/include
    ${FATFS_SDK_PATH}/ff15/source
    ${CMAKE_CURRENT_BINARY_DIR}   # generated .pio.h headers from pioasm
)

# ---- Libraries ----
target_link_libraries(cd32_ode
    pico_stdlib
    pico_multicore          # Dual-core launch (Core 1 bus handler)
    hardware_pio            # PIO for DA output, subcode encoder, and COMMO bus
    hardware_dma            # DMA for sector transfers and I2S audio
    hardware_pwm            # (Available for future use)
    hardware_irq            # Fast IRQ for DMA completion (audio ping-pong)
    hardware_timer          # Repeating 1 ms timer for state machine
    hardware_gpio
    hardware_clocks         # set_sys_clock_khz()
    hardware_vreg           # vreg_set_voltage() for 150 MHz
    hardware_flash          # flash_range_erase/program for config persistence
    hardware_sync           # save_and_disable_interrupts for flash writes
    FatFs_SPI               # SD card FatFS (from no-OS-FatFS library)

    # ---- Pico 2 W WiFi + lwIP (for webserver.c) ----
    # Note: This requires building for Pico 2 W target (PICO_BOARD=pico2_w).
    # For non-W Pico 2, comment out these four lines and webserver.c will
    # compile but WiFi functions will return false.
    pico_cyw43_arch_lwip_poll   # CYW43439 WiFi, poll mode (no RTOS required)
                                # Note: only available when PICO_BOARD=pico2_w
                                # Replace with pico_cyw43_arch_none if WiFi unused
)

# ---- stdio via USB CDC (disable UART to free those pins for user GPIOs) ----
pico_enable_stdio_usb(cd32_ode 1)
pico_enable_stdio_uart(cd32_ode 0)

# ---- Extra outputs: .uf2 for drag-and-drop flashing, .bin, .hex ----
pico_add_extra_outputs(cd32_ode)

# ---- Compiler warnings ----
target_compile_options(cd32_ode PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter   # Several callbacks take unused context pointers
    -O2                     # Optimise for speed (important for Core 1 latency)
)

# =============================================================================
# Optional: standalone self-test binary
# Uncomment to build a test-only UF2 that runs selftest_run() and halts.
# Flash this first to verify hardware before building the full firmware.
# =============================================================================
# add_executable(cd32_ode_selftest
#     src/selftest_main.c   # (create a trivial main that calls selftest_run())
#     src/selftest.c
#     src/cxd2545q.c
#     src/disc_image.c
#     src/sector_cache.c
#     src/subcode.c
#     src/sd_card.c
#     src/hw_config.c
#    src/audio.c
#     src/ecc.c
#     src/config.c
#     src/logger.c          # Logger needed for LOG_* macros in cxd2545q.c
# )
# target_include_directories(cd32_ode_selftest PRIVATE include ${FATFS_SDK_PATH}/include)
 #target_link_libraries(cd32_ode_selftest pico_stdlib pico_multicore
 #    hardware_pio hardware_dma hardware_irq hardware_timer hardware_gpio
 #    hardware_clocks hardware_vreg hardware_flash hardware_sync FatFs_SPI)
 #pico_enable_stdio_usb(cd32_ode_selftest 1)
# pico_add_extra_outputs(cd32_ode_selftest)
