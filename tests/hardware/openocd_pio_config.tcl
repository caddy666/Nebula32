# openocd_pio_config.tcl — Verify PIO state-machine configuration registers
#
# Run after flashing Nebula32.uf2 and BEFORE disc playback starts
# (FDEBUG TXSTALL will be set once DMA is running — do not check it here).
#
# Usage:
#   openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
#           -c "adapter speed 4000" \
#           -c "program build-core2350b/Nebula32.uf2 verify reset" \
#           -c "sleep 3000" \
#           -c "halt" \
#           -s . -f tests/hardware/openocd_pio_config.tcl \
#           -c "resume; shutdown"
#
# RP2350 register addresses
#   PIO0_BASE = 0x50200000   PIO1_BASE = 0x50300000
#   SM0_CLKDIV offset = 0x0C8   SM1_CLKDIV offset = 0x0E0
#   SM0_PINCTRL offset = 0x0D4  SM1_PINCTRL offset = 0x0EC
#   PINCTRL bit layout (6-bit fields on RP2350B):
#     [31:29] SIDESET_COUNT  [28:26] SET_COUNT  [25:20] OUT_COUNT
#     [19:14] IN_BASE        [13:8]  SIDESET_BASE  [7:2] SET_BASE  [1:0] OUT_BASE (partial)
#   Note: on RP2350B PINCTRL fields are extended to 6 bits to address GPIO 0-47.
#   OUT_BASE is bits [5:0], IN_BASE is bits [21:16], SIDESET_BASE is bits [15:10].
#   (Exact offsets — verify against RP2350 datasheet section 3.5.4.)

set pass_count 0
set fail_count 0

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
echo "PIO configuration register verification — Core2350B0"
echo "============================================================"

# ---- PIO0 SM0: DA output (da_output.pio, clkdiv int=32) ----
set pio0_sm0_clkdiv [mrw 0x502000C8]
set da_clkdiv_int   [expr {($pio0_sm0_clkdiv >> 16) & 0xFFFF}]
check {$da_clkdiv_int == 32} \
    "PIO0 SM0 (DA output) CLKDIV int = 32 → BCLK ≈ 2.12 MHz" \
    "got $da_clkdiv_int"

# ---- PIO0 SM1: subcode encoder (subcode_encoder.pio, clkdiv int=24) ----
set pio0_sm1_clkdiv [mrw 0x502000E0]
set sub_clkdiv_int  [expr {($pio0_sm1_clkdiv >> 16) & 0xFFFF}]
check {$sub_clkdiv_int == 24} \
    "PIO0 SM1 (subcode encoder) CLKDIV int = 24" \
    "got $sub_clkdiv_int"

# ---- PIO1 SM0: COMMO RX — IN_BASE must be GPIO 44 ----
# On RP2350B, SM PINCTRL layout places IN_BASE at bits [21:16] (6-bit field).
set pio1_sm0_pinctrl [mrw 0x503000D4]
set commo_in_base    [expr {($pio1_sm0_pinctrl >> 16) & 0x3F}]
check {$commo_in_base == 44} \
    "PIO1 SM0 (COMMO RX) IN_BASE = GPIO 44 (moved from GPIO 15)" \
    "got $commo_in_base"

# ---- PIO1 SM1: COMMO TX — SIDESET_BASE must be GPIO 44 ----
# SIDESET_BASE is at bits [15:10] in the 6-bit extended layout.
set pio1_sm1_pinctrl [mrw 0x503000EC]
set commo_side_base  [expr {($pio1_sm1_pinctrl >> 10) & 0x3F}]
check {$commo_side_base == 44} \
    "PIO1 SM1 (COMMO TX) SIDESET_BASE = GPIO 44" \
    "got $commo_side_base"

# ---- PIO0 FSTAT: both SMs should not be stalled (pre-playback) ----
# FSTAT bits [19:16] = TXFULL per SM; bits [3:0] = RXEMPTY per SM.
# Pre-playback: TX FIFOs are empty (TXFULL=0) — acceptable.
set pio0_fstat [mrw 0x50200004]
echo "  [info] PIO0 FSTAT = 0x[format %08X $pio0_fstat]  (TX empty expected pre-playback)"

echo ""
echo "openocd_pio_config: $pass_count passed, $fail_count failed"
