/**
 * @file  gpio_map.h
 * @brief GPIO pin assignments — Core2350B0 (RP2350B) + RM2 WiFi module.
 *
 * Full pin grid:
 *
 *  GPIO | Signal        | Dir    | Conn | Notes
 *  -----+---------------+--------+------+-------------------------------------------
 *     0 | DA_DATA       | OUT    |   9  | I2S serial data   (PIO0 SM0)
 *     1 | DA_BCLK       | OUT    |  11  | I2S bit clock
 *     2 | DA_LRCLK      | OUT    |   8  | I2S word-select
 *     3 | DA_C2PO       | OUT    |  12  | C2 error pointer  (low = no errors)
 *     4 | DA_EMPH       | OUT    |  13  | Pre-emphasis flag (low = none)
 *     5 | SUB_DATA      | OUT    |  17  | Subcode serial    (PIO0 SM1)
 *     6 | SUB_CLK       | OUT    |  18  | Subcode clock
 *     7 | SUB_WFCLK     | OUT    |  14  | Subcode word-frame clock
 *     8 | SUB_SCOR      | OUT    |  15  | Subcode sync correlator
 *     9 | M17SINE       | IN     |   5  | 16.9344 MHz ref   (GPIN0)
 *    10 | ACTIVE        | OUT    |  24  | Drive active/spinning
 *    11 | DOOR          | IN     |  26  | Door/tray switch  (active-low)
 *    12 | (free)        | -      |   -  | Spare GPIO
 *    13 | PASSIVE       | OUT    |  23  | Drive passive/standby
 *    14 | RESET         | IN     |   7  | Active-low /RESET from CD32 host
 *    15 | (free)        | -      |   -  | Spare GPIO
 *    16 | UART0_TX      | OUT    |   -  | Debug serial TX
 *    17 | UART0_RX      | IN     |   -  | Debug serial RX
 *    18 | (free)        | -      |   -  | Spare GPIO
 *    19 | (free)        | -      |   -  | Spare GPIO
 *    20 | UART1_TX      | OUT    |   -  | Auxiliary serial TX
 *    21 | UART1_RX      | IN     |   -  | Auxiliary serial RX
 *    22 | (free)        | -      |   -  | Spare GPIO
 *    23 | WL_ON         | OUT    |   -  | WiFi RM2 power/enable  (CYW43439)
 *    24 | WL_DIN        | OUT    |   -  | WiFi RM2 SPI MOSI
 *    25 | WL_CS         | -      |   -  | WiFi RM2 SPI CS / blue LED (via CYW43439)
 *    26 | MCP23017_SDA  | BIDIR  |   -  | I2C1 SDA — rotary encoder expander
 *    27 | MCP23017_SCL  | OUT    |   -  | I2C1 SCL
 *    28 | (free)        | -      |   -  | Spare GPIO
 *    29 | WL_CLK        | OUT    |   -  | WiFi RM2 SPI CLK
 *    30 | SDIO_CLK      | OUT    |   -  | SD card clock     (D0−2; SDIO PIO constraint)
 *    31 | SDIO_CMD      | BIDIR  |   -  | SD card command   (D0−1)
 *    32 | SDIO_D0       | BIDIR  |   -  | SD card data 0
 *    33 | SDIO_D1       | BIDIR  |   -  | SD card data 1   (D0+1; auto-computed by lib)
 *    34 | SDIO_D2       | BIDIR  |   -  | SD card data 2   (D0+2)
 *    35 | SDIO_D3       | BIDIR  |   -  | SD card data 3   (D0+3)
 *    36 | (free)        | -      |   -  | Spare GPIO
 *    37 | (free)        | -      |   -  | Spare GPIO
 *    38 | (free)        | -      |   -  | Spare GPIO
 *    39 | LED_RED       | OUT    |   -  | Red LED  (RM2 — direct GPIO)
 *    40 | ST7789_DC     | OUT    |   -  | Display data/command select
 *    41 | ST7789_CS     | OUT    |   -  | Display chip-select  (SPI1 CSn — valid)
 *    42 | ST7789_SCK    | OUT    |   -  | Display SPI clock    (SPI1 SCK — valid)
 *    43 | ST7789_MOSI   | OUT    |   -  | Display SPI data     (SPI1 TX  — valid)
 *    44 | IF_CLK        | BIDIR  |  20  | COMMO clock   (PIO1 SM0/SM1)
 *    45 | IF_DATA       | BIDIR  |  21  | COMMO data
 *    46 | IF_DIR        | OUT    |  25  | COMMO direction control
 *    47 | PSRAM_CS      | OUT    |   -  | QSPI PSRAM chip-select (CS1)
 *
 * PIO allocation:
 *   PIO0 SM0 — da_output.pio        GPIO 0-2   DA_DATA / DA_BCLK / DA_LRCLK
 *   PIO0 SM1 — subcode_encoder.pio  GPIO 5-8   SUB_DATA / SUB_CLK / SUB_WFCLK / SUB_SCOR
 *   PIO1 SM0 — commo.pio            GPIO 44-46 IF_CLK / IF_DATA / IF_DIR  (RX)
 *   PIO1 SM1 — commo.pio            GPIO 44-46 (shared)                    (TX)
 *   PIO1 SM2/3 — SDIO (library)     GPIO 30-35 via pio_claim_unused_sm()
 */

#pragma once

/* =========================================================================
 * CD32 26-pin drive connector signals — GPIO 0–14
 * ========================================================================= */

/* --- DA I2S output — PIO0 SM0 (da_output.pio) ---------------------------- */
#define PIN_DA_DATA    0   /**< conn 9  — I2S serial data out                */
#define PIN_DA_BCLK    1   /**< conn 11 — I2S bit clock out                  */
#define PIN_DA_LRCLK   2   /**< conn 8  — I2S word-select out                */
#define PIN_DA_C2PO    3   /**< conn 12 — C2 error pointer out (low = ok)    */
#define PIN_DA_EMPH    4   /**< conn 13 — pre-emphasis flag out (low = none) */

/* --- Subcode output — PIO0 SM1 (subcode_encoder.pio) --------------------- */
#define PIN_SUB_DATA   5   /**< conn 17 — subcode serial data out            */
#define PIN_SUB_CLK    6   /**< conn 18 — subcode clock out                  */
#define PIN_SUB_WFCLK  7   /**< conn 14 — subcode word-frame clock out       */
#define PIN_SUB_SCOR   8   /**< conn 15 — subcode sync correlator out        */

/* --- M17SINE master clock reference — GPIN0 ------------------------------ */
#define PIN_M17SINE    9   /**< conn 5  — 16.9344 MHz CD master clock in     */

/* --- Drive status --------------------------------------------------------- */
#define PIN_ACTIVE    10   /**< conn 24 — drive active/spinning out          */
#define PIN_DOOR      11   /**< conn 26 — door/tray switch in (active-low)   */

/* --- PIN_SCOR: alias for the generated SUB_SCOR output -------------------- */
/* Upstream timer.c registers an IRQ on PIN_SCOR so scor_counter ticks once
 * per sector.  In the ODE the SCOR pulse is generated on PIN_SUB_SCOR (GPIO 8)
 * by subcode_pulse_sector_clocks(), so alias them to the same physical pin.
 * The IRQ is still gated by BUILD_WITH_COMMO in timer.c, so the counter
 * is driven by the software-tick path in the ODE build.                     */
#define PIN_SCOR      PIN_SUB_SCOR   /**< = GPIO 8 — references the generated SCOR output */

/* --- GPIO 13: PASSIVE output ---------------------------------------------- */
#define PIN_PASSIVE   13   /**< conn 23 — drive passive/standby out          */

/* --- /RESET from CD32 host ------------------------------------------------ */
#define PIN_RESET     14   /**< conn 7  — active-low /RESET from CD32 in     */

/* =========================================================================
 * Debug UARTs — GPIO 16–21
 * ========================================================================= */

#define PIN_UART0_TX  16   /**< UART0 TX — debug serial (stdio)              */
#define PIN_UART0_RX  17   /**< UART0 RX — debug serial (stdio)              */

#define PIN_UART1_TX  20   /**< UART1 TX — auxiliary serial                  */
#define PIN_UART1_RX  21   /**< UART1 RX — auxiliary serial                  */

/* =========================================================================
 * WiFi RM2 module (CYW43439) — GPIO 23–25, 29
 * Pin numbers match the pico2_w SDK board definition.
 * ========================================================================= */

/* GPIO 25 (WL_CS/blue LED) is owned by the CYW43439 driver via
 * cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, ...) — do not touch directly. */

/* =========================================================================
 * MCP23017 I2C rotary encoder — I2C1 on GPIO 26–27
 * Moved from I2C0/GPIO 28-29 so that GPIO 29 is free for WiFi SPI CLK.
 * ========================================================================= */

#define PIN_MCP23017_SDA 26  /**< I2C1 SDA                                   */
#define PIN_MCP23017_SCL 27  /**< I2C1 SCL                                   */

/* =========================================================================
 * SD card — 4-bit SDIO — GPIO 30–35
 * SDIO PIO constraint: CLK must be D0−2 (mod 32), CMD must be D0−1.
 * With D0=32: CLK=(32+30)%32=30, CMD=31 — satisfied automatically.
 * D1–D3 are auto-computed by the library as D0+1, D0+2, D0+3.
 * ========================================================================= */

#define PIN_SDIO_CLK   30
#define PIN_SDIO_CMD   31
#define PIN_SDIO_D0    32
#define PIN_SDIO_D1    33
#define PIN_SDIO_D2    34
#define PIN_SDIO_D3    35

/* =========================================================================
 * Red LED — GPIO 39
 * ========================================================================= */

#define PIN_LED_RED    39   /**< RM2 red LED — direct GPIO output             */

/* =========================================================================
 * ST7789 240×240 display — hardware SPI1 — GPIO 40–43
 * SPI1 function validity (RP2350): CSn→41, SCK→42, TX→43 all confirmed valid.
 * GPIO 43 is the only SPI1 TX option in this range (39=LED, 47=PSRAM).
 * DC is plain GPIO — no SPI function constraint.
 * ========================================================================= */

#define PIN_ST7789_DC   40  /**< Data/Command select (GPIO out)               */
#define PIN_ST7789_CS   41  /**< Chip select, active-low (SPI1 CSn)           */
#define PIN_ST7789_SCK  42  /**< SPI1 clock                                   */
#define PIN_ST7789_MOSI 43  /**< SPI1 MOSI (DIN)                              */

/* =========================================================================
 * COMMO 3-wire serial — PIO1 SM0/SM1 — GPIO 44–46
 * commo.pio requires three consecutive pins: CLK, CLK+1=DATA, CLK+2=DIR.
 * Moved from GPIO 15-17 to free those pins for UART0 (16/17).
 * ========================================================================= */

#define PIN_IF_CLK    44   /**< conn 20 — COMMO clock  (bidir, idles high)   */
#define PIN_IF_DATA   45   /**< conn 21 — COMMO data   (bidir)               */
#define PIN_IF_DIR    46   /**< conn 25 — COMMO direction control out         */

/* Aliases expected by src/commo.c and src/commo_bridge.c */
#define PIN_COMMO_CLK  PIN_IF_CLK
#define PIN_COMMO_DATA PIN_IF_DATA
#define PIN_COMMO_DIR  PIN_IF_DIR

/* =========================================================================
 * PSRAM — GPIO 47 (QMI CS1)
 * ========================================================================= */
/* PSRAM_CS1_PIN is defined in src/psram.c as 47 — matches this layout.      */
