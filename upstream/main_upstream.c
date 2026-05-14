/**
 * @file  main.c
 * @brief CD32 Pico2 firmware — entry point.
 *
 * Initialises all subsystems then runs the cooperative main loop.
 *
 * Loop order (matches the original Philips/Commodore firmware):
 *   1. command_handler()   — push a queued COMMO command to the player
 *   2. COMMO_INTERFACE()   — service the serial RX/TX state machine
 *   3. Dispatcher()        — route commands and status packets
 *   4. player()            — advance the active disc-control sequence
 *                            and run servo/subcode/shock background tasks
 *
 * Hardware: Raspberry Pi Pico 2 (RP2350)
 */

#include "pico/stdlib.h"
#include <stdio.h>

#include "defs.h"
#include "player.h"
#include "commo.h"
#include "cmd_hndl.h"
#include "driver.h"
#include "timer.h"
#include "gpio_map.h"
#include "hardware/gpio.h"

/* Forward declarations for modules in core/ */
extern void Dispatcher(void);

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    /* Pico SDK board init (clocks, USB/UART stdio) */
    stdio_init_all();

    /* -----------------------------------------------------------------
     * Subsystem initialisation
     * -------------------------------------------------------------- */

    /* player_init() calls: timer_init(), driver_init(),
     *                       reset_dsic2_cd6(), servo_init(), cd6_init()
     * and then sets the player interface to the idle/ready state.       */
    player_init();

    /* Initialise the COMMO serial interface (GPIO + state machine) */
    COMMO_INIT();

    /* Initialise the command handler */
    Init_command_handler();

    /* Enable the SCOR edge counter (was EA=1; EX0=1 on the 8051) */
    enable_scor_counter();

    /* Indicate boot complete on the status LED */
    gpio_put(PIN_LED_STATUS, 1);

    /* -----------------------------------------------------------------
     * Main loop — cooperative multitasking, no RTOS required
     * -------------------------------------------------------------- */
    for (;;) {
        command_handler();
        COMMO_INTERFACE();
        Dispatcher();
        player();
    }

    /* Unreachable */
    return 0;
}
