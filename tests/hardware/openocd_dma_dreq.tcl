# openocd_dma_dreq.tcl — Verify DMA channel 0 TREQ_SEL after da_start()
#
# Must be run WHILE the DA DMA engine is active (disc playing).
# The DMA CTRL register retains its config value after a halt, so the
# approach is: start playback, then halt and read.
#
# DMA_BASE      = 0x50000000
# CH0_CTRL_TRIG = DMA_BASE + 0x000   bits [20:15] = TREQ_SEL
# CH1_CTRL_TRIG = DMA_BASE + 0x040
#
# DREQ values (RP2350):
#   DREQ_PIO0_TX0 = 0   (DMA paced by PIO0 SM0 TX FIFO — DA output)
#   DREQ_PIO0_TX1 = 1   (PIO0 SM1 TX — subcode encoder)
#
# Usage:
#   (1) Start playback via web UI or COMMO
#   (2) openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
#               -c "adapter speed 4000; halt" \
#               -s . -f tests/hardware/openocd_dma_dreq.tcl \
#               -c "resume; shutdown"

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
echo "DMA DREQ verification — Core2350B0 (requires active playback)"
echo "============================================================"

# CH0 — DA audio DMA (ping-pong buffer A)
set ch0_ctrl [mrw 0x50000000]
set ch0_treq [expr {($ch0_ctrl >> 15) & 0x3F}]
set ch0_en   [expr {$ch0_ctrl & 0x1}]
check {$ch0_treq == 0} \
    "DMA CH0 TREQ_SEL = 0 (DREQ_PIO0_TX0 — DA DMA paced by PIO0 SM0)" \
    "got $ch0_treq"
check {$ch0_en == 1} \
    "DMA CH0 enabled (EN bit set)" \
    "got $ch0_en"

# CH1 — DA audio DMA (ping-pong buffer B)
set ch1_ctrl [mrw 0x50000040]
set ch1_treq [expr {($ch1_ctrl >> 15) & 0x3F}]
check {$ch1_treq == 0} \
    "DMA CH1 TREQ_SEL = 0 (DREQ_PIO0_TX0 — DA ping-pong partner)" \
    "got $ch1_treq"

# Read addresses — should point somewhere in SRAM (0x20000000–0x2007FFFF)
set ch0_read [mrw 0x50000004]
set ch1_read [mrw 0x50000044]
set sram_lo 0x20000000
set sram_hi 0x20080000
check {$ch0_read >= $sram_lo && $ch0_read < $sram_hi} \
    "DMA CH0 READ_ADDR in SRAM (0x20000000–0x2007FFFF)" \
    "[format 0x%08X $ch0_read]"
check {$ch1_read >= $sram_lo && $ch1_read < $sram_hi} \
    "DMA CH1 READ_ADDR in SRAM" \
    "[format 0x%08X $ch1_read]"

# Transfer count — should be non-zero (mid-transfer when we halted)
set ch0_tc [mrw 0x50000008]
echo "  \[info\] DMA CH0 remaining transfers = $ch0_tc"

echo ""
echo "openocd_dma_dreq: $pass_count passed, $fail_count failed"
