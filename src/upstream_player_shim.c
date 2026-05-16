// =============================================================================
// upstream_player_shim.c — linkage shim satisfying upstream player.h symbols
// =============================================================================
//
// The upstream cmd_hndl.c writes commands into player_interface, which is
// normally consumed by player.c in the cd32_pico project (deleted).  In our
// ODE build player.c is absent, but we still need the symbol to link.
//
// This stub provides:
//   1. The player_interface global (read by cmd_hndl.c, polled by commo_bridge.c)
//   2. player_error and process_id globals (declared in player.h)
//   3. No-op player_init() and player() stubs
//
// commo_bridge.c polls player_interface.a_command each tick and, when it
// finds a new command there, calls _handle_opc() to route it to the ODE
// pipeline (da_output, sector_cache, etc.).
//
// IMPORTANT: Always compiled in the merged build (cmd_hndl.c is always
// included).  In the original cd32_pico project, player.c provided these
// symbols — that file has been deleted from this repo.
// =============================================================================

// Bare includes resolve via upstream/include in CMakeLists include_directories
#include "defs.h"     // interface_field_t, byte, IDLE_OPC, READY
#include "player.h"   // declares player_interface, player_error etc.

// ---------------------------------------------------------------------------
// Public globals — shared with upstream cmd_hndl.c, dispatcher.c
// ---------------------------------------------------------------------------

// The command interface struct: cmd_hndl.c writes a_command + params here;
// commo_bridge.c reads and clears it each tick.
interface_field_t player_interface = {
    .p_status  = READY,     // Start ready to accept commands
    .a_command = IDLE_OPC,  // No pending command
    .param1    = 0,
    .param2    = 0,
    .param3    = 0,
};

byte player_error = 0;       // No error
byte process_id   = 0xFF;    // 0xFF = idle (matches upstream convention)
byte function_id  = 0;

// ---------------------------------------------------------------------------
// player_init — called by commo_bridge_init() when BUILD_WITH_COMMO=1
// ---------------------------------------------------------------------------
// In the full cd32_pico build, player_init() calls timer_init(), driver_init(),
// reset_dsic2_cd6(), servo_init(), and cd6_init().  In our ODE build:
//   - timer_init() is called by main.c before commo_bridge_init()
//   - driver_init() is NOT called (it would init CXD2500BQ/DSIC2/pio_hw_init)
//   - servo, cd6, and dsic2 init are all skipped (no real hardware in ODE mode)
void player_init(void)
{
    // Hardware initialisation (timer, GPIO, COMMO PIO) is handled entirely
    // by commo_bridge_init() in the ODE build, which calls timer_init() and
    // sets up the COMMO PIO directly without conflicting with our ODE's
    // da_output on PIO0 SM0 and subcode_encoder on PIO0 SM1.
    // servo_init() and cd6_init() are NOT called in ODE mode.
    player_interface.p_status  = READY;
    player_interface.a_command = IDLE_OPC;
}

// ---------------------------------------------------------------------------
// player — called once per main-loop iteration in the original cd32_pico
// ---------------------------------------------------------------------------
// In ODE mode, drive-state is tracked in commo_bridge.c (_handle_opc).
// commo_bridge_poll() reads player_interface.a_command and routes it to
// the ODE pipeline (da_output / sector_cache) when non-IDLE.
void player(void)
{
    // No-op: command dispatch happens in commo_bridge_poll().
}
