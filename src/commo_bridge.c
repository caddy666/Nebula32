// =============================================================================
// commo_bridge.c — COMMO Bus Bridge Implementation
// =============================================================================
//
// Connects the upstream cd32_pico COMMO serial bus driver to our ODE's
// CXD2545Q emulation pipeline.  Translates between the two command sets
// and formats status responses correctly for the CD32 Amiga chipset.
//
// COMMO PACKET FORMAT (host → drive):
//   Byte 0:  opcode (lower nibble) | sequence counter (upper nibble)
//   Byte 1:  parameter 1 (if any)
//   Byte 2:  parameter 2 (if any)
//   Byte 3:  parameter 3 (if any)
//   Byte N:  checksum (XOR of all preceding bytes)
//
// STATUS PACKET FORMAT (drive → host, 15 bytes):
//   Byte 0:  status identifier 0x00
//   Byte 1:  player status (COMMO_READY_WITHOUT_ERROR etc.)
//   Byte 2:  error code
//   Byte 3:  command echo
//   Bytes 4-14: reserved / zeros
//   Last byte: checksum
//
// Q-CHANNEL PACKET FORMAT (drive → host, 15 bytes):
//   Byte 0:  packet type 0x01
//   Bytes 1-12: 12 bytes of Q-channel subcode
//   Bytes 13-14: reserved
//   (No explicit checksum — same framing as status packets)
// =============================================================================

#include "commo_bridge.h"
#include "cd_types.h"
#include "da_output.h"
#include "disc_image.h"
#include "sector_cache.h"
#include "subcode.h"
#include "logger.h"
#include "upstream_types.h"

// Upstream COMMO interface (bare includes resolve via upstream/include in CMakeLists path)
#include "commo.h"        // COMMO_INTERFACE, NEW_CMD_RECEIVED, GET_BUFFER, etc.
#include "defs.h"         // interface_field_t, opcodes, cd_time_t, IDLE_OPC, READY
#include "pio_hw.h"       // PIO_COMMO_RX/TX, SM_COMMO_RX/TX, g_offset_commo_* externs
#include "cmd_hndl.h"     // command_handler(), Init_command_handler()
#include "sts_q_id.h"     // Store_update_status(), Get_update_status()
#include "player.h"       // player_interface extern, player_error, process_id
#include "gpio_map.h"     // PIN_IF_CLK/DATA/DIR → PIN_COMMO_CLK/DATA/DIR aliases
                          // PIN_ACTIVE, PIN_DOOR, PIN_SCOR
#include "timer.h"        // timer_init()

// Generated PIO header from upstream/pio/commo.pio (built by pioasm)
#include "commo.pio.h"    // commo_rx_program, commo_tx_program, commo_program_init()

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>

// External upstream globals (defined in upstream_player_shim.c)
extern interface_field_t player_interface;

// External ODE globals (defined in main.c)
extern disc_image_t   g_disc;
extern sector_cache_t g_cache;

// ---------------------------------------------------------------------------
// PIO program offset globals
// ---------------------------------------------------------------------------
// upstream/include/pio_hw.h declares all six offsets as extern.
// upstream/hal/pio_hw.c (deleted — contained real-hardware init for
// CXD2500BQ/DSIC2/QCHAN) would have defined them, but its pio_hw_init()
// would claim PIO0 SM0–SM3 which the ODE already owns.
//
// We provide the definitions here instead.  Only the COMMO pair are ever
// non-zero; the CXD/DSIC/QCHAN offsets stay at zero (unused in ODE mode).
// g_qchan_ready is likewise defined here to satisfy pio_hw.h's extern.
uint g_offset_cxd      = 0;   // CXD2500BQ — not used in ODE mode
uint g_offset_dsic_tx  = 0;   // DSIC2 TX  — not used in ODE mode
uint g_offset_dsic_rx  = 0;   // DSIC2 RX  — not used in ODE mode
uint g_offset_qchan    = 0;   // Q-channel — not used in ODE mode
uint g_offset_commo_rx = 0;   // COMMO RX  — set by commo_bridge_init()
uint g_offset_commo_tx = 0;   // COMMO TX  — set by commo_bridge_init()
volatile bool g_qchan_ready = false; // Q-channel capture done — not used

// ---------------------------------------------------------------------------
// PIO1 IRQ handler — clears COMMO RX/TX interrupt flags
// commo.c polls via pio_commo_rx_ready(); we only need to clear the flag here
// ---------------------------------------------------------------------------
static void __not_in_flash_func(s_pio1_irq_handler)(void) {
    if (pio_interrupt_get(PIO_COMMO_RX, 0))
        pio_interrupt_clear(PIO_COMMO_RX, 0);
    if (pio_interrupt_get(PIO_COMMO_TX, 1))
        pio_interrupt_clear(PIO_COMMO_TX, 1);
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static uint8_t _handle_opc(uint8_t opc, uint8_t p1, uint8_t p2, uint8_t p3);
static void    _update_active_pin(void);
static void    _send_toc_packets(void);

// ---------------------------------------------------------------------------
// Fake drive timing
// ---------------------------------------------------------------------------
// A real Panasonic CR-563 (the drive fitted to production CD32 units) takes
// roughly 1.5 s to spin up from cold and 50–500 ms to seek across the disc.
// Some titles (e.g. Zool 2, Banshee) time the gap between TRAY_IN and the
// first successful read; an ODE that responds in microseconds can confuse
// that check.
//
// Uncomment the line below to activate fake delays.  Everything else below
// compiles either way — with FAKE_TIMING off the drive advances states
// instantly (current behaviour); with it on the deadlines are enforced.
//
// #define FAKE_TIMING

#define SPINUP_DELAY_US         1500000u  // 1.5 s  — disc accelerate + lead-in read
#define SEEK_DELAY_MIN_US         50000u  // 50 ms  — head settle + track buffer
#define SEEK_DELAY_MAX_US        500000u  // 500 ms — full-disc crossing
#define SEEK_DELAY_US_PER_TRACK     700u  // ~0.7 ms per track of travel

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static bool          s_active             = false;
static uint8_t       s_last_cmd_echo      = 0;
static drive_state_t s_drive_state        = DRIVE_IDLE;
static uint32_t      s_seek_lba           = 0;   // last seek target
static uint32_t      s_current_lba        = 0;   // last confirmed head position
static uint32_t      s_state_deadline_us  = 0;   // 0 = no pending delay
static bool          s_door_prev          = true; // set to real pin state in init

// Advance through transient states (SPINUP → READY, SEEKING → READY).
// With FAKE_TIMING defined, the transition is held until s_state_deadline_us
// has passed.  Without it, the transition is immediate.
static void _maybe_advance_state(void) {
    if (s_drive_state != DRIVE_SPINUP && s_drive_state != DRIVE_SEEKING) return;

#ifdef FAKE_TIMING
    // Stay in the current state until the deadline has elapsed.
    // The subtraction wraps correctly for uint32_t timer rollover.
    if (s_state_deadline_us != 0 &&
        (int32_t)(time_us_32() - s_state_deadline_us) < 0) {
        return;  // deadline not yet reached
    }
    s_state_deadline_us = 0;
#endif

    if (s_drive_state == DRIVE_SPINUP)  s_drive_state = DRIVE_READY;
    if (s_drive_state == DRIVE_SEEKING) {
        s_current_lba = s_seek_lba;
        s_drive_state = DRIVE_READY;
    }
    _update_active_pin();
}

static uint8_t _build_status(void) {
    uint8_t s = DRIVE_STATUS_DISC;  // disc always present in ODE mode
    if (s_drive_state == DRIVE_SPINUP || s_drive_state == DRIVE_SEEKING)
        s |= DRIVE_STATUS_BUSY;
    if (s_drive_state == DRIVE_ERROR)
        s |= DRIVE_STATUS_ERROR;
    return s;
}

// PIN_ACTIVE (conn 24, GPIO 10): high whenever the motor is spinning.
// Also drives the drive LED on the CD32 front panel — low = LED off.
// Low only in DRIVE_IDLE (tray open / motor stopped) and DRIVE_ERROR.
static void _update_active_pin(void) {
    bool active = (s_drive_state != DRIVE_IDLE && s_drive_state != DRIVE_ERROR);
    gpio_put(PIN_ACTIVE, active ? 1 : 0);
}

// ---------------------------------------------------------------------------
// _wait_commo_ready — spin until COMMO TX bus is free (or timeout)
// ---------------------------------------------------------------------------
static void _wait_commo_ready(void) {
    const uint32_t timeout_us = 20000;   // 20 ms per packet — generous
    uint32_t waited = 0;
    while (SEND_STRING_READY() == COMMO_BUSY && waited < timeout_us) {
        sleep_us(10);
        waited += 10;
    }
}

// ---------------------------------------------------------------------------
// _send_toc_packets — synthesise Q-channel TOC packets from g_disc and
// transmit them one by one over the COMMO bus.
//
// Red Book lead-in Q-channel format (mode 1, ADR=1):
//   Byte 0: CTRL|ADR — 0x41=data track / 0x01=audio track (high=CTRL, low=ADR)
//   Byte 1: TNO      — 0x00 (lead-in)
//   Byte 2: POINT    — 0xA0 / 0xA1 / 0xA2 / 01-99 BCD
//   Byte 3-5:        — r_time (relative time, zeros in lead-in)
//   Byte 6:          — ZERO
//   Byte 7-9:        — a_time MSF in BCD  (or first/last track# in .min for 0xA0/A1)
//   Byte 10-11:      — CRC (set to 0; Akiko does not check it)
// ---------------------------------------------------------------------------
static void _send_toc_packets(void) {
    uint8_t qbuf[12];

    // Determine CTRL nibble for the first track — used for 0xA0 and 0xA2 CONAD.
    // CTRL = 0x04 if first track is a data track; 0x00 if audio.
    const track_t *trk1 = &g_disc.tracks[g_disc.first_track - 1];
    uint8_t ctrl_first = (trk1->type != TRACK_TYPE_AUDIO) ? 0x04u : 0x00u;

    // Helper macro: encode track number as BCD
    #define TO_BCD(n)  (uint8_t)(((uint8_t)((n) / 10) << 4) | ((uint8_t)((n) % 10)))

    // ---- 0xA0 — first track number + disc type ----
    memset(qbuf, 0, sizeof(qbuf));
    qbuf[0] = (uint8_t)((ctrl_first << 4) | 0x01u);   // CTRL|ADR
    qbuf[1] = 0x00;    // TNO: lead-in
    qbuf[2] = 0xA0;    // POINT: first track info
    // a_time.min = first track BCD, .sec = disc type (0x00=CD-DA, 0x10=Mode1)
    qbuf[7] = TO_BCD(g_disc.first_track);
    qbuf[8] = (ctrl_first & 0x04u) ? 0x10u : 0x00u;   // disc type
    _wait_commo_ready();
    commo_bridge_send_qchannel(qbuf);

    // ---- 0xA1 — last track number ----
    memset(qbuf, 0, sizeof(qbuf));
    const track_t *trkL = &g_disc.tracks[g_disc.last_track - 1];
    uint8_t ctrl_last  = (trkL->type != TRACK_TYPE_AUDIO) ? 0x04u : 0x00u;
    qbuf[0] = (uint8_t)((ctrl_last << 4) | 0x01u);
    qbuf[1] = 0x00;
    qbuf[2] = 0xA1;    // POINT: last track info
    qbuf[7] = TO_BCD(g_disc.last_track);
    _wait_commo_ready();
    commo_bridge_send_qchannel(qbuf);

    // ---- 0xA2 — lead-out start time ----
    memset(qbuf, 0, sizeof(qbuf));
    msf_t lo_msf = lba_to_msf(g_disc.total_sectors);
    qbuf[0] = (uint8_t)((ctrl_first << 4) | 0x01u);
    qbuf[1] = 0x00;
    qbuf[2] = 0xA2;    // POINT: lead-out
    qbuf[7] = lo_msf.minute;
    qbuf[8] = lo_msf.second;
    qbuf[9] = lo_msf.frame;
    _wait_commo_ready();
    commo_bridge_send_qchannel(qbuf);

    // ---- One entry per track (POINT = track number BCD) ----
    for (int i = g_disc.first_track; i <= g_disc.last_track; i++) {
        const track_t *trk = &g_disc.tracks[i - 1];
        uint8_t ctrl  = (trk->type != TRACK_TYPE_AUDIO) ? 0x04u : 0x00u;
        msf_t   start = lba_to_msf(trk->start_lba);

        memset(qbuf, 0, sizeof(qbuf));
        qbuf[0] = (uint8_t)((ctrl << 4) | 0x01u);
        qbuf[1] = 0x00;
        qbuf[2] = TO_BCD((uint8_t)i);     // POINT = track number
        qbuf[7] = start.minute;
        qbuf[8] = start.second;
        qbuf[9] = start.frame;
        _wait_commo_ready();
        commo_bridge_send_qchannel(qbuf);
    }

    #undef TO_BCD
}

// ---------------------------------------------------------------------------
// commo_bridge_init
// ---------------------------------------------------------------------------
// Initialises the COMMO side of the ODE hardware:
//   1. PIN_ACTIVE (GPIO 10, conn 24): drive-active output + drive LED, cleared to 0 (idle)
//   2. COMMO PIO programs loaded onto PIO1 SM0/SM1 (commo_program_init)
//   3. PIO1 IRQ routed to the COMMO RX/TX interrupt handler
//   4. Upstream command-handler pipeline (Init_command_handler)
//
// timer_init(), DOOR (GPIO 11), and SCOR (GPIO 12) are configured by
// main.c before this is called.
//
// Critically, we do NOT call the full pio_hw_init() or driver_init() because
// those would try to add programs to PIO0 SM0/SM1 (CXD2500BQ, DSIC2, QCHAN)
// which are already claimed by our DA output PIO loaded in main.c.
//
// PIO allocation in merged ODE+COMMO build:
//   PIO0 SM0: da_output      (DA_DATA/BCLK/LRCLK → Akiko + LC78835M)
//   PIO0 SM1: subcode_encoder (SUB_DATA/CLK/WFCLK/SCOR → Akiko)
//   PIO0 SM2: (free)
//   PIO0 SM3: (free)
//   PIO1 SM0: commo_rx       (upstream COMMO, loaded here)
//   PIO1 SM1: commo_tx       (upstream COMMO, loaded here)
void commo_bridge_init(void) {
#if !BUILD_WITH_COMMO
    printf("[COMMO] BUILD_WITH_COMMO=0 — COMMO bridge disabled\n");
    s_active = false;
    return;
#endif

    // PIN_DOOR (GPIO 11), PIN_SCOR/GPIO 12, and timer_init() are configured
    // unconditionally in main.c before this function is called.

    // ACTIVE (conn 24, GPIO 10): drive-active output to CD32 mainboard.
    // Start low (drive idle — motor not yet spinning).
    gpio_init(PIN_ACTIVE);
    gpio_set_dir(PIN_ACTIVE, GPIO_OUT);
    gpio_put(PIN_ACTIVE, 0);

    // Snapshot the door pin so the first poll doesn't fire a false eject event
    // if the door happens to be open at boot.
    s_door_prev = gpio_get(PIN_DOOR);

    // Step 3: Load COMMO PIO programs onto PIO1 SM0 (RX) and SM1 (TX) only.
    // This is extracted from pio_hw_init() — just the COMMO section.
    g_offset_commo_rx = pio_add_program(PIO_COMMO_RX, &commo_rx_program);
    g_offset_commo_tx = pio_add_program(PIO_COMMO_TX, &commo_tx_program);
    commo_program_init(PIO_COMMO_RX, SM_COMMO_RX, SM_COMMO_TX,
                       g_offset_commo_rx, g_offset_commo_tx,
                       PIN_COMMO_CLK, COMMO_BIT_FREQ_HZ);

    // Enable PIO1 IRQ for COMMO RX/TX events
    pio_set_irq0_source_enabled(PIO_COMMO_RX, pis_interrupt0, true);
    pio_set_irq0_source_enabled(PIO_COMMO_TX, pis_interrupt1, true);
    irq_set_exclusive_handler(PIO1_IRQ_0, s_pio1_irq_handler);
    irq_set_enabled(PIO1_IRQ_0, true);

    // Step 4: Initialise the upstream command handler pipeline
    // (dispatcher + cmd_hndl + sts_q_id — no hardware, pure state machines)
    player_interface.p_status  = READY;
    player_interface.a_command = IDLE_OPC;
    Init_command_handler();

    s_active = true;

    LOG_INFO_MSG("COMMO", "COMMO bridge ready (PIO1 SM%d/SM%d GPIO CLK=%d DATA=%d DIR=%d)",
                 SM_COMMO_RX, SM_COMMO_TX,
                 PIN_COMMO_CLK, PIN_COMMO_DATA, PIN_COMMO_DIR);
    printf("[COMMO] COMMO bus ready — http interface still works; audio disabled\n");
}

// ---------------------------------------------------------------------------
// commo_bridge_poll
// ---------------------------------------------------------------------------
// Called from Core 0 main loop.  Steps the COMMO state machine and
// checks for completed command packets.  Also polls player_interface
// for commands placed there by the upstream cmd_hndl.c pipeline.
bool commo_bridge_poll(void) {
    if (!s_active) return false;

    // Advance transient states (SPINUP/SEEKING → READY) once their deadline
    // has elapsed.  With FAKE_TIMING disabled this is immediate; with it on
    // the drive holds BUSY until the simulated mechanical delay expires.
    _maybe_advance_state();

    // ---- Door pin monitor — rising edge (LOW→HIGH) means door opened ----
    // PIN_DOOR is active-low with a pull-up: door closed = LOW, door open = HIGH.
    // On the rising edge we stop playback and send the 0x00 eject status so
    // Akiko clears the DISC bit without waiting for a TRAY_OUT_OPC command.
    // NOTE: this logic is replicated verbatim in tests/host/test_door_tray.cpp
    // (DoorPin test group).  Update that replica whenever this block changes.
    bool door_now = gpio_get(PIN_DOOR);
    if (door_now && !s_door_prev) {
        printf("[COMMO] DOOR open — stopping drive\n");
        LOG_INFO_MSG("COMMO", "DOOR open");
        da_stop();
        s_drive_state = DRIVE_IDLE;
        _update_active_pin();
        commo_bridge_send_status(0x00);
        s_door_prev = door_now;
        return true;
    }
    s_door_prev = door_now;

    // ---- Step upstream modules ----
    // Step the dispatcher (routes commands and status packets)
    Dispatcher();
    // Step the command handler (writes validated commands into player_interface)
    command_handler();
    // Step the COMMO state machine (serial RX/TX)
    COMMO_INTERFACE();

    // ---- Path A: COMMO bus new command received ----
    uint8_t cmd_status = NEW_CMD_RECEIVED();
    if (cmd_status == COMMO_NEW_COMMAND || cmd_status == COMMO_SAME_COMMAND) {
        uint8_t raw_opc = GET_BUFFER(0);
        uint8_t opc     = raw_opc & COMMO_OPCODE_MASK;
        uint8_t p1 = GET_BUFFER(1);
        uint8_t p2 = GET_BUFFER(2);
        uint8_t p3 = GET_BUFFER(3);
        s_last_cmd_echo = raw_opc;

        printf("[COMMO] opc=0x%02X p1=%02X p2=%02X p3=%02X\n", opc, p1, p2, p3);
        LOG_INFO_MSG("COMMO", "opc=0x%02X p1=%02X p2=%02X p3=%02X", opc, p1, p2, p3);

        uint8_t status = _handle_opc(opc, p1, p2, p3);
        // Clear player_interface before Dispatcher/cmd_hndl writes it on
        // the next tick — prevents Path C from double-processing this command.
        player_interface.a_command = IDLE_OPC;
        player_interface.p_status  = READY;
        FREE_CMD_BUFFER();
        commo_bridge_send_status(status);
        return true;
    }

    // ---- Path B: DRQ — sector delivered, notify host data is ready ----
    if (da_drq_pending()) {
        uint8_t drq_status = _build_status() | DRIVE_STATUS_DRQ;
        commo_bridge_send_status(drq_status);
        return true;
    }

    // ---- Path C: upstream cmd_hndl.c wrote into player_interface ----
    if (player_interface.a_command != IDLE_OPC) {
        uint8_t opc = player_interface.a_command;
        printf("[COMMO] player_interface opc=0x%02X\n", opc);
        LOG_INFO_MSG("COMMO", "player_interface opc=0x%02X", opc);

        uint8_t status = _handle_opc(opc,
            player_interface.param1,
            player_interface.param2,
            player_interface.param3);

        player_interface.p_status  = READY;
        player_interface.a_command = IDLE_OPC;
        commo_bridge_send_status(status);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// _handle_opc — direct ODE action for each COMMO opcode
// ---------------------------------------------------------------------------
// Returns a status byte for the COMMO response packet.
static uint8_t _handle_opc(uint8_t opc, uint8_t p1, uint8_t p2, uint8_t p3) {
    switch (opc) {
        case TRAY_OUT_OPC:
            printf("[COMMO] TRAY_OUT — disc eject\n");
            LOG_INFO_MSG("COMMO", "TRAY_OUT");
            da_stop();
            s_drive_state = DRIVE_IDLE;
            _update_active_pin();
            return 0x00;  // DISC bit clear (tray open)

        case TRAY_IN_OPC:
            printf("[COMMO] TRAY_IN — disc inserted\n");
            LOG_INFO_MSG("COMMO", "TRAY_IN");
            sector_cache_seek(&g_cache, 0);  // reset prefetch to lead-in
            s_drive_state = DRIVE_SPINUP;
            // #ifdef FAKE_TIMING
            //     s_state_deadline_us = time_us_32() + SPINUP_DELAY_US;
            // #else
            s_state_deadline_us = 0;  // instant — uncomment FAKE_TIMING to delay
            // #endif
            _update_active_pin();
            return _build_status();

        case START_UP_OPC:
            // akiko sends START_UP repeatedly until it receives a READY
            // response — it is a poll, not a one-shot command.  We report the
            // current state (SPINUP→BUSY, READY→OK) without forcing a transition;
            // _maybe_advance_state() in the poll loop handles the timing.
            printf("[COMMO] START_UP (state=%d)\n", (int)s_drive_state);
            LOG_INFO_MSG("COMMO", "START_UP state=%d", (int)s_drive_state);
            if (s_drive_state == DRIVE_IDLE) s_drive_state = DRIVE_SPINUP;
            _update_active_pin();
            return _build_status();

        case STOP_OPC:
            printf("[COMMO] STOP\n");
            LOG_INFO_MSG("COMMO", "STOP");
            da_stop();
            s_drive_state = DRIVE_IDLE;
            _update_active_pin();
            return _build_status();

        case PLAY_TRACK_OPC: {
            // p1 = BCD track number (01-99).  If p1 is valid, resolve the track's
            // start LBA directly; otherwise fall back to the last SEEK target.
            uint8_t track_bin = (uint8_t)((p1 >> 4) * 10u + (p1 & 0x0Fu));
            if (track_bin >= g_disc.first_track && track_bin <= g_disc.last_track) {
                s_seek_lba = g_disc.tracks[track_bin - 1].start_lba;
                sector_cache_seek(&g_cache, s_seek_lba);
            }
            const track_t *trk = disc_find_track(&g_disc, s_seek_lba);
            da_set_audio_mode(trk && trk->type == TRACK_TYPE_AUDIO);
            printf("[COMMO] PLAY track=%u LBA=%lu\n", track_bin, (unsigned long)s_seek_lba);
            LOG_INFO_MSG("COMMO", "PLAY track=%u LBA=%lu", track_bin, (unsigned long)s_seek_lba);
            da_start_play(&g_cache, s_seek_lba);
            s_current_lba = s_seek_lba;
            s_drive_state = DRIVE_PLAYING;
            _update_active_pin();
            return _build_status();
        }

        case PAUSE_ON_OPC:
            printf("[COMMO] PAUSE\n");
            LOG_INFO_MSG("COMMO", "PAUSE");
            da_pause();
            s_drive_state = DRIVE_PAUSED;
            return _build_status();

        case PAUSE_OFF_OPC: {
            printf("[COMMO] RESUME\n");
            LOG_INFO_MSG("COMMO", "RESUME");
            const track_t *rtrk = disc_find_track(&g_disc, da_get_current_lba());
            da_set_audio_mode(rtrk && rtrk->type == TRACK_TYPE_AUDIO);
            da_resume();
            s_drive_state = DRIVE_PLAYING;
            return _build_status();
        }

        case SEEK_OPC: {
            // p1=MM, p2=SS, p3=FF in BCD (same encoding as Q-channel)
            msf_t m = { p1, p2, p3 };
            s_seek_lba = msf_to_lba(m);
            printf("[COMMO] SEEK → LBA=%lu (MSF %02X:%02X:%02X)\n",
                   (unsigned long)s_seek_lba, p1, p2, p3);
            LOG_INFO_MSG("COMMO", "SEEK LBA=%lu", (unsigned long)s_seek_lba);
            sector_cache_seek(&g_cache, s_seek_lba);
            s_drive_state = DRIVE_SEEKING;
            _update_active_pin();
            s_state_deadline_us = 0;
            return _build_status();
        }

        case JUMP_TRACKS_OPC: {
            // p1=high byte, p2=low byte of signed 16-bit relative track count.
            // Explicit shift preserves the big-endian wire encoding — the
            // byte_hl_t union in the original 8051 code inverts on little-endian ARM.
            int16_t delta = (int16_t)((uint16_t)p1 << 8 | p2);
            uint32_t cur_lba = da_get_current_lba();
            const track_t *cur_trk = disc_find_track(&g_disc, cur_lba);
            int target = cur_trk ? (int)cur_trk->number + (int)delta
                                 : (int)g_disc.first_track;
            if (target < (int)g_disc.first_track) target = (int)g_disc.first_track;
            if (target > (int)g_disc.last_track)  target = (int)g_disc.last_track;
            s_seek_lba = g_disc.tracks[target - 1].start_lba;
            printf("[COMMO] JUMP_TRACKS delta=%d → track=%d LBA=%lu\n",
                   (int)delta, target, (unsigned long)s_seek_lba);
            LOG_INFO_MSG("COMMO", "JUMP_TRACKS delta=%d track=%d LBA=%lu",
                         (int)delta, target, (unsigned long)s_seek_lba);
            sector_cache_seek(&g_cache, s_seek_lba);
            s_drive_state = DRIVE_SEEKING;
            _update_active_pin();
            s_state_deadline_us = 0;
            return _build_status();
        }

        case READ_TOC_OPC:
            printf("[COMMO] READ_TOC  tracks=%d-%d  lead-out=%lu\n",
                   g_disc.first_track, g_disc.last_track,
                   (unsigned long)g_disc.total_sectors);
            LOG_INFO_MSG("COMMO", "READ_TOC tracks=%d-%d",
                         g_disc.first_track, g_disc.last_track);
            _send_toc_packets();
            s_drive_state = DRIVE_READY;
            _update_active_pin();
            return _build_status();

        case READ_SUBCODE_OPC: {
            uint32_t cur_lba = da_get_current_lba();
            const track_t *trk = disc_find_track(&g_disc, cur_lba);
            uint8_t qbuf[12];
            if (trk) {
                bool is_data = (trk->type != TRACK_TYPE_AUDIO);
                subcode_build_q_position(trk->number, 1, is_data,
                                         trk->start_lba, cur_lba, qbuf);
            } else {
                memset(qbuf, 0, sizeof(qbuf));
            }
            _wait_commo_ready();
            commo_bridge_send_qchannel(qbuf);
            s_drive_state = DRIVE_READY;
            return _build_status();
        }

        case SINGLE_SPEED_OPC:
            printf("[COMMO] SINGLE_SPEED (1x)\n");
            LOG_INFO_MSG("COMMO", "speed → 1x");
            da_set_double_speed(false);
            return _build_status();

        case DOUBLE_SPEED_OPC:
            printf("[COMMO] DOUBLE_SPEED (2x)\n");
            LOG_INFO_MSG("COMMO", "speed → 2x");
            da_set_double_speed(true);
            return _build_status();

        case SET_VOLUME_OPC:
            // DA output has no analogue volume control — LC78835M DAC handles volume.
            return _build_status();

        case ENTER_SERVICE_MODE_OPC:
            printf("[COMMO] ENTER_SERVICE_MODE (no-op in ODE mode)\n");
            LOG_INFO_MSG("COMMO", "ENTER_SERVICE_MODE no-op");
            return _build_status();

        default:
            // Return current drive status rather than ERROR — Akiko treats an
            // ERROR response as a hardware fault and may halt playback entirely.
            LOG_WARN_MSG("COMMO unknown opc 0x%02X", opc);
            return _build_status();
    }
}

// ---------------------------------------------------------------------------
// commo_bridge_send_status
// ---------------------------------------------------------------------------
// Builds a 15-byte status packet and queues it for COMMO TX.
// The packet layout follows the original Philips/Commodore format.
void commo_bridge_send_status(uint8_t status_byte) {
    if (!s_active) return;

    // Build 15-byte status packet
    // Byte 0:  0x00 = status packet identifier
    // Byte 1:  player status (COMMO_READY_WITHOUT_ERROR or COMMO_BUSY)
    // Byte 2:  CXD status byte (our register emulator's status)
    // Byte 3:  echo of last received command opcode
    // Bytes 4-13: zeros
    // Byte 14: XOR checksum of bytes 0-13

    static uint8_t pkt[STATUS_PACKET_LENGTH];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = 0x00;   // Status packet type

    // Map drive state to COMMO player-status byte
    if (s_drive_state == DRIVE_SEEKING || s_drive_state == DRIVE_SPINUP) {
        pkt[1] = COMMO_BUSY;
    } else if (status_byte & DRIVE_STATUS_ERROR) {
        pkt[1] = COMMO_READY_WITH_ERROR;
    } else {
        pkt[1] = COMMO_READY_WITHOUT_ERROR;
    }

    pkt[2] = status_byte;
    pkt[3] = s_last_cmd_echo;

    // Compute checksum: XOR of all preceding bytes
    uint8_t chk = 0;
    for (int i = 0; i < STATUS_PACKET_LENGTH - 1; i++) {
        chk ^= pkt[i];
    }
    pkt[STATUS_PACKET_LENGTH - 1] = chk;

    SEND_STRING(SEND_STRING_COMPLETE, pkt, STATUS_PACKET_LENGTH);
}

// ---------------------------------------------------------------------------
// commo_bridge_send_qchannel
// ---------------------------------------------------------------------------
// Sends a Q-channel subcode packet to the CD32 host.
// Called when fresh Q-channel subcode is available (e.g. after sector delivery).
void commo_bridge_send_qchannel(const uint8_t *qbuf_12bytes) {
    if (!s_active) return;

    // Check COMMO TX is free
    if (SEND_STRING_READY() == COMMO_BUSY) return;

    static uint8_t pkt[Q_PACKET_LENGTH];
    memset(pkt, 0, sizeof(pkt));

    pkt[0] = 0x01;  // Q-channel packet type
    memcpy(pkt + 1, qbuf_12bytes, 12);  // 12 bytes of Q-channel data

    // Checksum
    uint8_t chk = 0;
    for (int i = 0; i < Q_PACKET_LENGTH - 1; i++) chk ^= pkt[i];
    pkt[Q_PACKET_LENGTH - 1] = chk;

    SEND_STRING(SEND_STRING_COMPLETE, pkt, Q_PACKET_LENGTH);
}

bool commo_bridge_is_active(void) {
    return s_active;
}

drive_state_t commo_bridge_get_drive_state(void) {
    return s_drive_state;
}

// =============================================================================
// PIO HARDWARE ABSTRACTION FUNCTIONS
// =============================================================================
// These functions are declared in upstream/include/pio_hw.h and would normally
// be defined in upstream/hal/pio_hw.c (deleted — contained real-hardware init
// for CXD2500BQ/DSIC2/QCHAN programs on PIO0 SM0–SM3, which are already owned
// by the ODE's da_output and subcode_encoder PIOs).
//
// upstream/core/commo.c calls pio_commo_rx_ready(), pio_commo_rx_get(),
// pio_commo_tx_byte() and pio_commo_release() to service the COMMO bus.
// We provide real implementations for those, and no-op stubs for the rest.
// =============================================================================

// ---------------------------------------------------------------------------
// pio_hw_init — no-op stub (the real init is split across main.c and
// commo_bridge_init())
// ---------------------------------------------------------------------------
// If anything in the upstream code calls pio_hw_init() directly it will
// silently succeed without touching our PIOs.  commo_bridge_init() already
// loaded the COMMO programs before any upstream module had a chance to run.
void pio_hw_init(void) {
    // Deliberately empty.  See header comment above.
}

// ---------------------------------------------------------------------------
// pio_commo_rx_ready — true when a received byte is waiting in the RX FIFO
// ---------------------------------------------------------------------------
// commo.c's commo_step() polls this to drive the RX state machine.
bool pio_commo_rx_ready(void) {
#if BUILD_WITH_COMMO
    if (!s_active) return false;
    return !pio_sm_is_rx_fifo_empty(PIO_COMMO_RX, SM_COMMO_RX);
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// pio_commo_rx_get — blocking fetch of one received byte
// ---------------------------------------------------------------------------
// Must only be called after pio_commo_rx_ready() has returned true, or
// after the SM was armed to wait for a new byte.  After the byte is pulled
// out of the FIFO the RX SM automatically re-arms for the next byte.
uint8_t pio_commo_rx_get(void) {
#if BUILD_WITH_COMMO
    if (!s_active) return 0;
    uint32_t raw = pio_sm_get_blocking(PIO_COMMO_RX, SM_COMMO_RX);
    // RX SM halts itself after each byte (push + irq set 0).
    // Clear IRQ flag and re-enable SM for next byte.
    if (pio_interrupt_get(PIO_COMMO_RX, 0)) {
        pio_interrupt_clear(PIO_COMMO_RX, 0);
    }
    // Re-enable RX SM for the next byte — uses the helper from commo.pio.h
    commo_rx_enable(PIO_COMMO_RX, SM_COMMO_RX, PIN_COMMO_CLK);
    return (uint8_t)(raw & 0xFFu);
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// pio_commo_tx_byte — blocking send of one byte over the COMMO bus
// ---------------------------------------------------------------------------
// Switches the bus to transmit mode, clocks out the byte, waits for the TX
// SM to complete, then returns.  The bus remains in transmit mode until
// pio_commo_release() is called — this lets commo.c send multiple bytes
// back-to-back without re-arming between each one.
void pio_commo_tx_byte(uint8_t data) {
#if BUILD_WITH_COMMO
    if (!s_active) return;

    // commo_tx_send() (from commo.pio.h) handles: stop RX SM, switch pin
    // directions, clear FIFOs, enable TX SM, and push the byte.
    commo_tx_send(PIO_COMMO_RX, SM_COMMO_RX, SM_COMMO_TX,
                  PIN_COMMO_CLK, data);

    // Wait for the TX FIFO to drain AND the SM to finish shifting the
    // last bit out.  The TX program does "irq set 1" after the last bit.
    // We poll the IRQ flag rather than rely on the handler, so that the
    // caller can guarantee the byte is on the wire before returning.
    const uint32_t TX_TIMEOUT_US = 10000;  // 10 ms per byte — generous
    uint32_t waited = 0;
    while (!pio_interrupt_get(PIO_COMMO_TX, 1) && waited < TX_TIMEOUT_US) {
        tight_loop_contents();
        sleep_us(1);
        waited++;
    }
    if (pio_interrupt_get(PIO_COMMO_TX, 1)) {
        pio_interrupt_clear(PIO_COMMO_TX, 1);
    }
    // Brief settle (half a bit period at COMMO_BIT_FREQ_HZ)
    sleep_us(5);
#else
    (void)data;
#endif
}

// ---------------------------------------------------------------------------
// pio_commo_release — release the bus back to receive mode after a transmit
// ---------------------------------------------------------------------------
// commo.c calls this after sending a complete response packet.  We disable
// the TX SM, switch pin directions back to input, and re-arm the RX SM for
// the next incoming command.
void pio_commo_release(void) {
#if BUILD_WITH_COMMO
    if (!s_active) return;
    pio_sm_set_enabled(PIO_COMMO_TX, SM_COMMO_TX, false);
    commo_rx_enable(PIO_COMMO_RX, SM_COMMO_RX, PIN_COMMO_CLK);
#endif
}

// ---------------------------------------------------------------------------
// CXD / DSIC / QCHAN stubs — real-hardware functions, not used in ODE mode
// ---------------------------------------------------------------------------
// These exist only to satisfy linker references from upstream code that
// might be pulled in accidentally.  upstream/drivers/ (driver.c, servo.c etc.)
// have been deleted — only commo.c, dispatcher.c, cmd_hndl.c, sts_q_id.c,
// maths.c, and timer.c are compiled in the ODE build.
//
// We log a warning the first time each is called so an incorrect build
// configuration is obvious at runtime.

void pio_cxd_write(uint8_t data) {
    (void)data;
    static bool warned = false;
    if (!warned) {
        printf("[PIO_HW_STUB] pio_cxd_write() called — not supported in ODE mode\n");
        LOG_WARN_MSG("[PIO] pio_cxd_write stub called (not supported in ODE)");
        warned = true;
    }
}

void pio_dsic_write(uint8_t data) {
    (void)data;
    static bool warned = false;
    if (!warned) {
        printf("[PIO_HW_STUB] pio_dsic_write() called — not supported in ODE mode\n");
        LOG_WARN_MSG("[PIO] pio_dsic_write stub called (not supported in ODE)");
        warned = true;
    }
}

uint8_t pio_dsic_read(void) {
    static bool warned = false;
    if (!warned) {
        printf("[PIO_HW_STUB] pio_dsic_read() called — not supported in ODE mode\n");
        LOG_WARN_MSG("[PIO] pio_dsic_read stub called (not supported in ODE)");
        warned = true;
    }
    return 0;
}

void pio_qchan_start(void) {
    static bool warned = false;
    if (!warned) {
        printf("[PIO_HW_STUB] pio_qchan_start() called — not supported in ODE mode\n");
        LOG_WARN_MSG("[PIO] pio_qchan_start stub called (not supported in ODE)");
        warned = true;
    }
}

void pio_qchan_drain(uint8_t *buf) {
    static bool warned = false;
    if (buf) memset(buf, 0, 10);
    if (!warned) {
        printf("[PIO_HW_STUB] pio_qchan_drain() called — not supported in ODE mode\n");
        LOG_WARN_MSG("[PIO] pio_qchan_drain stub called (not supported in ODE)");
        warned = true;
    }
}
