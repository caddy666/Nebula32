// =============================================================================
// selftest.c — Hardware Self-Test Utility
// =============================================================================
//
// Run this BEFORE connecting the Pico 2 to the CD32 motherboard to verify:
//
//   1. GPIO DA/SUB serial output pins pull up and down freely (no shorts to VCC/GND)
//   2. SD card mounts and a disc image can be opened and read
//
// Pure-logic tests (MSF/LBA conversion, subcode CRC, EDC/ECC, sector
// synthesis layout, format extension matching) live in the host CppUTest
// suite (tests/host/) where they run without hardware on every build.
//
// TO BUILD AS STANDALONE TEST:
//   In CMakeLists.txt, uncomment the selftest target at the bottom.
//
// TO BUILD AS RUNTIME CHECK (default):
//   Call selftest_run() from main() before launching Core 1.
//   It prints a PASS/FAIL report over USB serial and returns true on pass.
//
// =============================================================================

#include "selftest.h"
#include "cd_types.h"
#include "disc_image.h"
#include "sd_card_api.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include <stdio.h>

// Pass/fail counters
static int s_pass = 0;
static int s_fail = 0;

// Helper macros for test assertions
#define TEST_ASSERT(cond, name) \
    do { \
        if (cond) { \
            printf("  [PASS] %s\n", name); \
            s_pass++; \
        } else { \
            printf("  [FAIL] %s\n", name); \
            s_fail++; \
        } \
    } while(0)

#define TEST_SECTION(name) \
    printf("\n--- %s ---\n", name)

// ---------------------------------------------------------------------------
// Test 1: GPIO configuration
// ---------------------------------------------------------------------------
static void test_gpio(void) {
    TEST_SECTION("GPIO Configuration");

    // Verify DA/SUB serial output pins (0-8) are configurable as inputs (no short to VCC/GND)
    static const char *const pin_names[] = {
        "DA_DATA", "DA_BCLK", "DA_LRCLK", "DA_C2PO", "DA_EMPH",
        "SUB_DATA", "SUB_CLK", "SUB_WFCLK", "SUB_SCOR"
    };
    for (int pin = 0; pin <= 8; pin++) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        // A shorted pin would read back a fixed level regardless of pull direction
        gpio_pull_up(pin);
        sleep_us(10);
        bool read_high = gpio_get(pin);
        gpio_pull_down(pin);
        sleep_us(10);
        bool read_low = gpio_get(pin);
        gpio_disable_pulls(pin);

        char label[48];
        snprintf(label, sizeof(label), "GPIO%d (%s) can be pulled high and low",
                 pin, pin_names[pin]);
        TEST_ASSERT(read_high && !read_low, label);
    }

    // /RESET input — pull up (host drives low to reset; should be high normally)
    gpio_init(CXD_RESET_PIN);
    gpio_set_dir(CXD_RESET_PIN, GPIO_IN);
    gpio_pull_up(CXD_RESET_PIN);
    sleep_us(10);
    bool reset_idle_high = gpio_get(CXD_RESET_PIN);
    TEST_ASSERT(reset_idle_high,
                "/RESET idle is high (no unexpected ground on pin)");
}

// ---------------------------------------------------------------------------
// Test 2: SD card
// ---------------------------------------------------------------------------
static void test_sd_card(void) {
    TEST_SECTION("SD Card (4-bit SDIO)");

    bool mounted = sd_card_init_and_mount();
    TEST_ASSERT(mounted, "SD card mounts successfully");

    if (!mounted) return;

    // Scan for images
    static char paths[4][MAX_PATH_LEN];
    uint32_t count = sd_scan_images(paths, 4, "0:/", 0);
    TEST_ASSERT(count > 0, "At least one disc image found on SD card");

    if (count == 0) return;

    // Try opening the first image
    disc_image_t disc;
    bool opened = disc_open(&disc, paths[0]);
    TEST_ASSERT(opened, "First disc image opens without error");

    if (opened) {
        TEST_ASSERT(disc.first_track >= 1 && disc.first_track <= 99,
                    "first_track is valid (1-99)");
        TEST_ASSERT(disc.last_track  >= disc.first_track,
                    "last_track >= first_track");
        TEST_ASSERT(disc.total_sectors > 0,
                    "Total sector count is non-zero");

        printf("         Image: %s\n", paths[0]);
        printf("         Tracks: %d-%d, Sectors: %lu\n",
               disc.first_track, disc.last_track, disc.total_sectors);

        // Try reading sector 0
        uint8_t sector_buf[SECTOR_RAW_SIZE];
        uint32_t bytes = disc_read_sector(&disc, 0, sector_buf, SECTOR_MODE_RAW);
        TEST_ASSERT(bytes > 0, "Sector 0 read returns data");

        disc_close(&disc);
    }
}

// ---------------------------------------------------------------------------
// Main self-test entry point
// ---------------------------------------------------------------------------
bool selftest_run(void) {
    s_pass = 0;
    s_fail = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  CD32 ODE Self-Test                      ║\n");
    printf("╚══════════════════════════════════════════╝\n");

    test_gpio();
    test_sd_card();

    printf("\n══════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_pass, s_fail);
    printf("══════════════════════════════════════════\n\n");

    if (s_fail > 0) {
        printf("⚠  SELF-TEST FAILED — resolve failures before connecting to CD32\n\n");
        return false;
    } else {
        printf("✓  All tests passed — safe to connect to CD32\n\n");
        return true;
    }
}
