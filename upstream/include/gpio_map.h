/**
 * @file  gpio_map.h
 * @brief GPIO pin assignments — unified layout for cd32_pico + ODE peripherals.
 *
 * GPIO 0–17: cd32_pico hardware interface (authoritative source)
 * GPIO 18–29: ODE peripherals (SD card, display, rotary encoder via MCP23017)
 *
*
 * PIO0 instruction memory layout (ODE standalone):
 *   0-12 (D[7:0], /WR, /RD, /CS, A[1:0])
 *   0-12 (shared)
 *
 * PIO1 instruction memory layout:
 *   SM0 — COMMO RX       : pins 15-17 (CLK, DATA, DIR)
 *   SM1 — COMMO TX       : pins 15-17 (shared)
 *   COMMO is a self-contained module (upstream/core/commo.c + commo.pio).
 *
 * Non-PIO pins:
 *   10  HF detector (real hardware) / /CS parallel bus (
 *   11  Door switch (real hardware) / A0 address 
 *   12  SCOR interrupt (real hardware) / A1 address 
 *   13  ST7789 display chip-select (ODE peripheral)
 *   25  free (onboard LED not used in merged firmware)
 */

#pragma once

/* =========================================================================
 * cd32_pico hardware interface — GPIO 0–17
 * ========================================================================= */

/* --- GPIO 0-1: free (available for future use) --------------------------- */

/* --- CXD2500BQ control — PIO0 SM0 ---------------------------------------- */
#define PIN_CXD_CLK    2   /**< UCL  — serial clock (side-set)  */
#define PIN_CXD_DATA   3   /**< UDAT — serial data  (out)       */
#define PIN_CXD_LAT    4   /**< ULAT — latch strobe (set)       */

/* --- DSIC2 servo IC — PIO0 SM1/SM2 --------------------------------------- */
#define PIN_DSIC_CLK   5   /**< SICL — serial clock (side-set)  */
#define PIN_DSIC_DATA  6   /**< SIDA — serial data  (bidir)     */
#define PIN_DSIC_LAT   7   /**< SILD — latch strobe (set)       */

/* --- Q-channel subcode — PIO0 SM3 ---------------------------------------- */
#define PIN_QCL        8   /**< Q-channel clock output (side-set) */
#define PIN_QDA        9   /**< Q-channel data input  (in)        */

/* --- Disc/door sense — standard GPIO ------------------------------------- */
#define PIN_HF_DET    10   /**< HF detector (disc presence, active low) */
#define PIN_DOOR      11   /**< Door/lid switch                         */

/* --- SCOR interrupt input — standard GPIO -------------------------------- */
#define PIN_SCOR      12   /**< Subcode clock, falling-edge interrupt   */

/* --- GPIO 13: free -------------------------------------------------------- */
/* --- GPIO 14: free -------------------------------------------------------- */

/* --- COMMO serial interface — PIO1 SM0/SM1 (separate module) ------------- */
/* upstream/core/commo.c owns this interface; do not use these pins elsewhere */
#define PIN_COMMO_CLK  15   /**< CLK  — bidir clock                    */
#define PIN_COMMO_DATA 16   /**< DATA — bidir data                     */
#define PIN_COMMO_DIR  17   /**< DIR  — direction control (output)     */

/* =========================================================================
 * ODE peripherals — GPIO 18–29
 * ========================================================================= */

/* --- SD card — 4-bit SDIO ------------------------------------------------ */
/* D0–D3 must be consecutive; RP2350 SDIO PIO requires this. */
#define PIN_SDIO_CLK   18
#define PIN_SDIO_CMD   19
#define PIN_SDIO_D0    20
#define PIN_SDIO_D1    21
#define PIN_SDIO_D2    22
#define PIN_SDIO_D3    23

/* --- ST7789 240×240 display — hardware SPI1 ------------------------------ */
#define PIN_ST7789_DC   24  /**< Data/Command select (GPIO out)    */
#define PIN_ST7789_CS   25  /**< Chip select, active-low (GPIO out) */
#define PIN_ST7789_SCK  26  /**< SPI1 clock                        */
#define PIN_ST7789_MOSI 27  /**< SPI1 MOSI (DIN)                   */

/* --- MCP23017 I2C GPIO expander — rotary encoder on GPA0–GPA2 ------------ */
/* I2C0 on GPIO 28/29 (SPI1 on 26/27 rules out I2C1). */
#define PIN_MCP23017_SDA 28  /**< I2C0 SDA                         */
#define PIN_MCP23017_SCL 29  /**< I2C0 SCL                         */
