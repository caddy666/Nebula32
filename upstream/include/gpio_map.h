/**
 * @file  gpio_map.h
 * @brief GPIO pin assignments — 26-pin Commodore CD32 drive connector + ODE peripherals.
 *
 * GPIO 0–17  : 26-pin drive connector signals (see table below)
 * GPIO 18–29 : ODE peripherals (SD card SDIO, ST7789 display, MCP23017 rotary encoder)
 *
 * 26-pin connector → Pico 2 GPIO mapping (authoritative):
 *
 *   Conn | Signal      | GPIO | Dir    | Notes
 *   -----+-------------+------+--------+---------------------------------------------
 *     9  | DA_DATA     |   0  | OUT    | I2S serial data (audio + sector bits)
 *    11  | DA_BCLK     |   1  | OUT    | I2S bit clock (~2.12 MHz at 1× speed)
 *     8  | DA_LRCLK    |   2  | OUT    | I2S word-select (44.1 kHz)
 *    12  | DA_C2PO     |   3  | OUT    | C2 error pointer   (drive low = no errors)
 *    13  | DA_EMPH     |   4  | OUT    | Pre-emphasis flag  (drive low = no emphasis)
 *    17  | SUB_DATA    |   5  | OUT    | Subcode serial data
 *    18  | SUB_CLK     |   6  | OUT    | Subcode clock
 *    14  | SUB_WFCLK   |   7  | OUT    | Subcode word-frame clock
 *    15  | SUB_SCOR    |   8  | OUT    | Subcode sync correlator
 *     5  | M17SINE     |   9  | IN     | 16.9344 MHz master clock ref (GPIN0)
 *    24  | ACTIVE      |  10  | OUT    | Drive active/spinning
 *    26  | DOOR        |  11  | IN     | Door/tray switch (active-low)
 *     -  | (free)      |  12  | IN     | Unconnected; upstream timer registers SCOR IRQ
 *                                        here for compat — never fires in ODE mode.
 *    23  | (PASSIVE)   |  13  | OUT    | Repurposed as ST7789_CS (not wired to connector)
 *     7  | RESET       |  14  | IN     | Active-low /RESET from CD32 host
 *    20  | IF_CLK      |  15  | BIDIR  | COMMO clock  (idles high, active-low pulses)
 *    21  | IF_DATA     |  16  | BIDIR  | COMMO data   (setup ≥150 ns before CLK edge)
 *    25  | IF_DIR      |  17  | OUT    | COMMO direction control
 *
 * PIO allocation (ODE build):
 *   PIO0 SM0 — da_output.pio        GPIO 0-2   DA_DATA / DA_BCLK / DA_LRCLK
 *   PIO0 SM1 — subcode_encoder.pio  GPIO 5-8   SUB_DATA / SUB_CLK / SUB_WFCLK / SUB_SCOR
 *   PIO1 SM0 — commo.pio            GPIO 15-17 IF_CLK / IF_DATA / IF_DIR  (RX)
 *   PIO1 SM1 — commo.pio            GPIO 15-17 (shared)                    (TX)
 */

#pragma once

/* =========================================================================
 * 26-pin drive connector — GPIO 0–17
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

/* --- GPIO 12: free (unconnected in ODE) ----------------------------------- */
/* The upstream timer module registers a SCOR falling-edge IRQ here for source
 * compatibility.  In the ODE the pin floats high (pull-up in main.c); the IRQ
 * is registered but never fires.  Do not drive this pin as output.           */
#define PIN_SCOR      12   /**< upstream SCOR compat — free GPIO in ODE      */

/* --- GPIO 13: ST7789 display chip-select (ODE peripheral) ----------------- */
/* Reuses the PASSIVE pad (conn 23) which is not routed to the ODE connector. */
/* Defined in CMakeLists.txt as ST7789_CS_PIN=13; do not redefine here.       */

/* --- /RESET from CD32 host ------------------------------------------------ */
#define PIN_RESET     14   /**< conn 7  — active-low /RESET from CD32 in     */

/* --- COMMO 3-wire serial interface — PIO1 SM0/SM1 ------------------------- */
/* upstream/core/commo.c owns this interface; do not drive these pins elsewhere. */
#define PIN_IF_CLK    15   /**< conn 20 — COMMO clock  (bidir, idles high)   */
#define PIN_IF_DATA   16   /**< conn 21 — COMMO data   (bidir)               */
#define PIN_IF_DIR    17   /**< conn 25 — COMMO direction control out         */

/* Aliases expected by upstream/core/commo.c and commo_bridge.c */
#define PIN_COMMO_CLK  PIN_IF_CLK
#define PIN_COMMO_DATA PIN_IF_DATA
#define PIN_COMMO_DIR  PIN_IF_DIR

/* =========================================================================
 * ODE peripherals — GPIO 18–29
 * ========================================================================= */

/* --- SD card — 4-bit SDIO (D0–D3 must be consecutive for RP2350 SDIO PIO) */
#define PIN_SDIO_CLK   18
#define PIN_SDIO_CMD   19
#define PIN_SDIO_D0    20
#define PIN_SDIO_D1    21
#define PIN_SDIO_D2    22
#define PIN_SDIO_D3    23

/* --- ST7789 240×240 display — hardware SPI1 ------------------------------ */
#define PIN_ST7789_DC   24  /**< Data/Command select (GPIO out)               */
#define PIN_ST7789_CS   25  /**< Chip select, active-low (GPIO out)           */
#define PIN_ST7789_SCK  26  /**< SPI1 clock                                   */
#define PIN_ST7789_MOSI 27  /**< SPI1 MOSI (DIN)                              */

/* --- MCP23017 I2C GPIO expander — rotary encoder on GPA0–GPA2 ------------ */
/* I2C0 on GPIO 28/29 (SPI1 on 26/27 rules out I2C1). */
#define PIN_MCP23017_SDA 28  /**< I2C0 SDA                                   */
#define PIN_MCP23017_SCL 29  /**< I2C0 SCL                                   */
