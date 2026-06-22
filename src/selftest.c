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
#include "gpio_map.h"
#ifdef BUILD_WITH_PSRAM
#include "psram.h"
#endif

#include "pico/stdlib.h"
#include "hardware/clocks.h"
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
// Test 1: System clock
// ---------------------------------------------------------------------------
static void test_sys_clock(void) {
    TEST_SECTION("System Clock");
    uint32_t hz = clock_get_hz(clk_sys);
    printf("         sys_clk = %lu Hz\n", (unsigned long)hz);
    TEST_ASSERT(hz >= 135000000U && hz <= 136000000U,
                "sys_clk in range 135.0-136.0 MHz (target 135,475,200)");
}

// ---------------------------------------------------------------------------
// Test 2: GPIO mux assignments — verify peripheral remapping (non-destructive)
// ---------------------------------------------------------------------------
static void test_gpio_mux(void) {
    TEST_SECTION("GPIO Mux Assignments");

    // DA I2S output — PIO0 (GPIO 0-2 via pio_gpio_init; 3/4 plain gpio_init)
    TEST_ASSERT(gpio_get_function(PIN_DA_DATA)  == GPIO_FUNC_PIO0, "GPIO0  (DA_DATA)    = PIO0");
    TEST_ASSERT(gpio_get_function(PIN_DA_BCLK)  == GPIO_FUNC_PIO0, "GPIO1  (DA_BCLK)    = PIO0");
    TEST_ASSERT(gpio_get_function(PIN_DA_LRCLK) == GPIO_FUNC_PIO0, "GPIO2  (DA_LRCLK)   = PIO0");
    TEST_ASSERT(gpio_get_function(PIN_DA_C2PO)  == GPIO_FUNC_SIO,  "GPIO3  (DA_C2PO)    = SIO (plain output)");
    TEST_ASSERT(gpio_get_function(PIN_DA_EMPH)  == GPIO_FUNC_SIO,  "GPIO4  (DA_EMPH)    = SIO (plain output)");

    // Subcode — PIO0 (GPIO 5-6); WFCLK/SCOR plain gpio_init outputs
    TEST_ASSERT(gpio_get_function(PIN_SUB_DATA)  == GPIO_FUNC_PIO0, "GPIO5  (SUB_DATA)   = PIO0");
    TEST_ASSERT(gpio_get_function(PIN_SUB_CLK)   == GPIO_FUNC_PIO0, "GPIO6  (SUB_CLK)    = PIO0");
    TEST_ASSERT(gpio_get_function(PIN_SUB_WFCLK) == GPIO_FUNC_SIO,  "GPIO7  (SUB_WFCLK)  = SIO (plain output)");
    TEST_ASSERT(gpio_get_function(PIN_SUB_SCOR)  == GPIO_FUNC_SIO,  "GPIO8  (SUB_SCOR)   = SIO (plain output)");

    // M17SINE master clock — GPCK (clock input)
    TEST_ASSERT(gpio_get_function(PIN_M17SINE) == GPIO_FUNC_GPCK, "GPIO9  (M17SINE)    = GPCK");

    // UART0 debug (GPIO 16/17) and UART1 auxiliary (GPIO 20/21)
    TEST_ASSERT(gpio_get_function(PIN_UART0_TX) == GPIO_FUNC_UART, "GPIO16 (UART0_TX)   = UART");
    TEST_ASSERT(gpio_get_function(PIN_UART0_RX) == GPIO_FUNC_UART, "GPIO17 (UART0_RX)   = UART");
    TEST_ASSERT(gpio_get_function(PIN_UART1_TX) == GPIO_FUNC_UART, "GPIO20 (UART1_TX)   = UART");
    TEST_ASSERT(gpio_get_function(PIN_UART1_RX) == GPIO_FUNC_UART, "GPIO21 (UART1_RX)   = UART");

    // Direct GPIO rotary encoder (GPIO 12, 15, 18, 19) — all SIO plain inputs
    TEST_ASSERT(gpio_get_function(PIN_ENC_A)   == GPIO_FUNC_SIO, "GPIO12 (ENC_A)      = SIO (input)");
    TEST_ASSERT(gpio_get_function(PIN_ENC_B)   == GPIO_FUNC_SIO, "GPIO15 (ENC_B)      = SIO (input)");
    TEST_ASSERT(gpio_get_function(PIN_ENC_SW)  == GPIO_FUNC_SIO, "GPIO18 (ENC_SW)     = SIO (input)");
    TEST_ASSERT(gpio_get_function(PIN_ENC_LOG) == GPIO_FUNC_SIO, "GPIO19 (ENC_LOG)    = SIO (input)");

    // SDIO 4-bit (GPIO 30-35 via PIO1 — library uses pio_claim_unused_sm on PIO1)
    TEST_ASSERT(gpio_get_function(PIN_SDIO_CLK) == GPIO_FUNC_PIO1, "GPIO30 (SDIO_CLK)   = PIO1");
    TEST_ASSERT(gpio_get_function(PIN_SDIO_CMD) == GPIO_FUNC_PIO1, "GPIO31 (SDIO_CMD)   = PIO1");
    TEST_ASSERT(gpio_get_function(PIN_SDIO_D0)  == GPIO_FUNC_PIO1, "GPIO32 (SDIO_D0)    = PIO1");

    // ST7789 display: DC/CS are plain GPIO, SCK/MOSI are SPI1
    TEST_ASSERT(gpio_get_function(PIN_ST7789_DC)   == GPIO_FUNC_SIO, "GPIO40 (ST7789_DC)  = SIO (plain GPIO)");
    TEST_ASSERT(gpio_get_function(PIN_ST7789_CS)   == GPIO_FUNC_SIO, "GPIO41 (ST7789_CS)  = SIO (plain GPIO)");
    TEST_ASSERT(gpio_get_function(PIN_ST7789_SCK)  == GPIO_FUNC_SPI, "GPIO42 (ST7789_SCK) = SPI");
    TEST_ASSERT(gpio_get_function(PIN_ST7789_MOSI) == GPIO_FUNC_SPI, "GPIO43 (ST7789_MOSI)= SPI");

    // COMMO bus (GPIO 44-46 — moved from GPIO 15-17; all PIO1 via pio_gpio_init)
    TEST_ASSERT(gpio_get_function(PIN_IF_CLK)  == GPIO_FUNC_PIO1, "GPIO44 (IF_CLK)     = PIO1");
    TEST_ASSERT(gpio_get_function(PIN_IF_DATA) == GPIO_FUNC_PIO1, "GPIO45 (IF_DATA)    = PIO1");
    TEST_ASSERT(gpio_get_function(PIN_IF_DIR)  == GPIO_FUNC_PIO1, "GPIO46 (IF_DIR)     = PIO1");

    // PSRAM QMI CS1 (GPIO 47)
    TEST_ASSERT(gpio_get_function(47U) == GPIO_FUNC_XIP_CS1, "GPIO47 (PSRAM_CS)   = XIP_CS1");
}

// ---------------------------------------------------------------------------
// Test 3: PIO configuration registers (non-destructive reads)
// ---------------------------------------------------------------------------
static void test_pio_config(void) {
    TEST_SECTION("PIO Configuration");

    // PIO0 SM0 — DA output clkdiv integer = 32 → BCLK ≈ 2.117 MHz (1× CD-DA)
    // clkdiv register [31:16] = integer part, [15:8] = 1/256 fractional
    uint32_t da_clkdiv_int  = pio0->sm[0].clkdiv >> 16;
    uint32_t da_clkdiv_frac = (pio0->sm[0].clkdiv >> 8) & 0xFFU;
    printf("         PIO0 SM0 (DA) clkdiv = %lu.%lu\n",
           (unsigned long)da_clkdiv_int, (unsigned long)da_clkdiv_frac);
    TEST_ASSERT(da_clkdiv_int == 32U,
                "PIO0 SM0 (DA output) CLKDIV int = 32 (2.117 MHz BCLK)");

    // PIO0 SM1 — subcode encoder clkdiv integer = 24 → 176,400 bps bit clock
    // 135,475,200 / (176,400 × 32) = 24.0 exactly
#if BUILD_WITH_COMMO
    uint32_t sub_clkdiv_int  = pio0->sm[1].clkdiv >> 16;
    uint32_t sub_clkdiv_frac = (pio0->sm[1].clkdiv >> 8) & 0xFFU;
    printf("         PIO0 SM1 (subcode) clkdiv = %lu.%lu\n",
           (unsigned long)sub_clkdiv_int, (unsigned long)sub_clkdiv_frac);
    TEST_ASSERT(sub_clkdiv_int == 24U,
                "PIO0 SM1 (subcode encoder) CLKDIV int = 24 (176,400 bps)");
#endif

    // PIO1 SM0 — COMMO RX: IN_BASE should be PIN_IF_CLK (GPIO 44)
    // PINCTRL register [19:15] = IN_BASE (5 bits)
    uint32_t commo_in_base = (pio1->sm[0].pinctrl >> 15) & 0x1FU;
    printf("         PIO1 SM0 (COMMO RX) IN_BASE = GPIO%lu (expect %d)\n",
           (unsigned long)commo_in_base, PIN_IF_CLK);
    TEST_ASSERT(commo_in_base == (uint32_t)PIN_IF_CLK,
                "PIO1 SM0 (COMMO RX) IN_BASE = GPIO44 (IF_CLK — remapped from GPIO15)");

    // PIO1 SM1 — COMMO TX side-set base should also be PIN_IF_CLK (GPIO 44)
    // PINCTRL register [14:10] = SIDESET_BASE (5 bits)
    uint32_t commo_side_base = (pio1->sm[1].pinctrl >> 10) & 0x1FU;
    printf("         PIO1 SM1 (COMMO TX) SIDESET_BASE = GPIO%lu (expect %d)\n",
           (unsigned long)commo_side_base, PIN_IF_CLK);
    TEST_ASSERT(commo_side_base == (uint32_t)PIN_IF_CLK,
                "PIO1 SM1 (COMMO TX) SIDESET_BASE = GPIO44 (IF_CLK)");
}

// ---------------------------------------------------------------------------
// Test 4: PSRAM (QMI CS1 on GPIO 47)
// ---------------------------------------------------------------------------
static void test_psram(void) {
    TEST_SECTION("PSRAM (GPIO47 QMI CS1)");
#ifdef BUILD_WITH_PSRAM
    TEST_ASSERT(psram_available(), "PSRAM initialised (psram_available() = true)");
    void *p = psram_alloc(4096);
    TEST_ASSERT(p != NULL, "PSRAM alloc 4 KB returns non-NULL pointer");
    // Bump allocator never reclaims; psram_free() just clears the magic guard.
    if (p) psram_free(p);
#else
    printf("  [SKIP] BUILD_WITH_PSRAM not set\n");
#endif
}

// ---------------------------------------------------------------------------
// Test 5: Direct GPIO encoder — pull-up integrity (GPIO 12/15/18/19)
// ---------------------------------------------------------------------------
static void test_encoder_gpio(void) {
    TEST_SECTION("Encoder GPIO (direct — no MCP23017)");
    // rotary_init() enables pull-ups on all four pins.  With nothing pressing
    // the encoder or button, each pin should read high.  A low reading means
    // either a short to GND or a missing pull-up.
    TEST_ASSERT(gpio_get(PIN_ENC_A),   "GPIO12 (ENC_A)   reads high (pull-up OK)");
    TEST_ASSERT(gpio_get(PIN_ENC_B),   "GPIO15 (ENC_B)   reads high (pull-up OK)");
    TEST_ASSERT(gpio_get(PIN_ENC_SW),  "GPIO18 (ENC_SW)  reads high (pull-up OK)");
    TEST_ASSERT(gpio_get(PIN_ENC_LOG), "GPIO19 (ENC_LOG) reads high (pull-up OK)");
}

// ---------------------------------------------------------------------------
// Test 6: GPIO configuration (pin integrity — pull-up / pull-down)
// NOTE: This test reconfigures GPIO 0-8 as software inputs, overriding PIO.
//       It must run LAST. Normal firmware operation requires a power-cycle
//       after selftest to restore PIO ownership of these pins.
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
    gpio_init(PIN_RESET);
    gpio_set_dir(PIN_RESET, GPIO_IN);
    gpio_pull_up(PIN_RESET);
    sleep_us(10);
    bool reset_idle_high = gpio_get(PIN_RESET);
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
        uint8_t sector_buf[SECTOR_RAW_BYTES];
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

    // Non-destructive hardware config checks (all peripherals already init'd)
    test_sys_clock();
    test_gpio_mux();
    test_pio_config();
    test_psram();
    test_encoder_gpio();
    test_sd_card();
    // GPIO pull integrity MUST run last: reconfigures GPIO 0-8 as SW inputs,
    // overriding PIO0 ownership.  Power-cycle required to restore normal DA output.
    test_gpio();

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
