# openocd_gpio_mux.tcl — Verify IO_BANK0 GPIO function assignments
#
# Reads the FUNCSEL field of each GPIO's CTRL register and confirms the
# hardware mux matches what gpio_map.h and main.c configure.
#
# IO_BANK0_BASE = 0x40014000
# GPIO_N_CTRL   = IO_BANK0_BASE + N*8 + 4
#
# FUNCSEL values (RP2350 SDK):
#   0=XIP  1=SPI  2=UART  3=I2C  4=PWM  5=SIO  6=PIO0  7=PIO1  8=PIO2
#   9=GPCK  12=XIP_CS1  31=NULL
#
# Usage:
#   openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
#           -c "adapter speed 4000; halt" \
#           -s . -f tests/hardware/openocd_gpio_mux.tcl \
#           -c "resume; shutdown"

set pass_count 0
set fail_count 0

proc gpio_ctrl_addr {n} {
    return [expr {0x40014000 + $n * 8 + 4}]
}

proc funcsel {n} {
    return [expr {[mrw [gpio_ctrl_addr $n]] & 0x1F}]
}

proc check {cond label detail} {
    global pass_count fail_count
    if {$cond} {
        echo "  \[PASS\] $label  ($detail)"
        incr pass_count
    } else {
        echo "  \[FAIL\] $label  ($detail)"
        incr fail_count
    }
}

echo ""
echo "============================================================"
echo "GPIO mux (IO_BANK0 FUNCSEL) verification — Core2350B0"
echo "============================================================"

# DA output (PIO0 = 6)
check {[funcsel 0] == 6}  "GPIO  0 (DA_DATA)    = PIO0 (6)" "got [funcsel 0]"
check {[funcsel 1] == 6}  "GPIO  1 (DA_BCLK)   = PIO0 (6)" "got [funcsel 1]"
check {[funcsel 2] == 6}  "GPIO  2 (DA_LRCLK)  = PIO0 (6)" "got [funcsel 2]"

# C2PO / EMPH / PASSIVE / ACTIVE / DOOR — plain SIO (5)
check {[funcsel 3] == 5}  "GPIO  3 (DA_C2PO)   = SIO  (5)" "got [funcsel 3]"
check {[funcsel 4] == 5}  "GPIO  4 (DA_EMPH)   = SIO  (5)" "got [funcsel 4]"

# Subcode (PIO0 = 6 via side-set / out pins)
check {[funcsel 5] == 6}  "GPIO  5 (SUB_DATA)  = PIO0 (6)" "got [funcsel 5]"
check {[funcsel 6] == 6}  "GPIO  6 (SUB_CLK)   = PIO0 (6)" "got [funcsel 6]"

# SUB_WFCLK / SUB_SCOR driven by gpio_put, not PIO out — SIO (5)
check {[funcsel 7] == 5}  "GPIO  7 (SUB_WFCLK) = SIO  (5)" "got [funcsel 7]"
check {[funcsel 8] == 5}  "GPIO  8 (SUB_SCOR)  = SIO  (5)" "got [funcsel 8]"

# M17SINE — clock input GPIN0 — GPCK (9)
check {[funcsel 9] == 9}  "GPIO  9 (M17SINE)   = GPCK (9)" "got [funcsel 9]"

# UART0 debug (UART = 2)
check {[funcsel 16] == 2} "GPIO 16 (UART0_TX)  = UART (2)" "got [funcsel 16]"
check {[funcsel 17] == 2} "GPIO 17 (UART0_RX)  = UART (2)" "got [funcsel 17]"

# UART1 auxiliary (UART = 2)
check {[funcsel 20] == 2} "GPIO 20 (UART1_TX)  = UART (2)" "got [funcsel 20]"
check {[funcsel 21] == 2} "GPIO 21 (UART1_RX)  = UART (2)" "got [funcsel 21]"

# I2C1 MCP23017 (I2C = 3)
check {[funcsel 26] == 3} "GPIO 26 (MCP_SDA)   = I2C  (3)" "got [funcsel 26]"
check {[funcsel 27] == 3} "GPIO 27 (MCP_SCL)   = I2C  (3)" "got [funcsel 27]"

# SDIO 4-bit — library claims PIO1 SM (PIO1 = 7)
check {[funcsel 30] == 7} "GPIO 30 (SDIO_CLK)  = PIO1 (7)" "got [funcsel 30]"
check {[funcsel 31] == 7} "GPIO 31 (SDIO_CMD)  = PIO1 (7)" "got [funcsel 31]"
check {[funcsel 32] == 7} "GPIO 32 (SDIO_D0)   = PIO1 (7)" "got [funcsel 32]"

# ST7789 display: DC/CS are SIO, SCK/MOSI are SPI1 (SPI = 1)
check {[funcsel 40] == 5} "GPIO 40 (ST7789_DC) = SIO  (5)" "got [funcsel 40]"
check {[funcsel 41] == 5} "GPIO 41 (ST7789_CS) = SIO  (5)" "got [funcsel 41]"
check {[funcsel 42] == 1} "GPIO 42 (ST7789_SCK)= SPI  (1)" "got [funcsel 42]"
check {[funcsel 43] == 1} "GPIO 43 (ST7789_DIN)= SPI  (1)" "got [funcsel 43]"

# COMMO bus — PIO1 (7); proves GPIO >31 pin mux works on RP2350B
check {[funcsel 44] == 7} "GPIO 44 (IF_CLK)    = PIO1 (7)" "got [funcsel 44]"
check {[funcsel 45] == 7} "GPIO 45 (IF_DATA)   = PIO1 (7)" "got [funcsel 45]"
check {[funcsel 46] == 7} "GPIO 46 (IF_DIR)    = PIO1 (7)" "got [funcsel 46]"

# PSRAM QMI CS1 (XIP_CS1 = 12)
check {[funcsel 47] == 12} "GPIO 47 (PSRAM_CS)  = XIP_CS1 (12)" "got [funcsel 47]"

echo ""
echo "openocd_gpio_mux: $pass_count passed, $fail_count failed"
