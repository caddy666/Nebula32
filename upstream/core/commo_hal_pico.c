/**
 * @file  commo_hal_pico.c
 * @brief Real PIO + GPIO implementation of the commo HAL (linked in firmware).
 *
 * tests/host/commo_hal_stub.c provides the test-harness version.
 */

#include "commo_hal.h"
#include "pico/stdlib.h"
#include "pio_hw.h"
#include "hardware/gpio.h"
#include "commo.pio.h"
#include "gpio_map.h"

uint8_t commo_hal_rxd(void)
{
    commo_rx_enable(PIO_COMMO_RX, SM_COMMO_RX, PIN_COMMO_CLK);

    uint32_t timeout = 500000u;
    while (!pio_commo_rx_ready() && --timeout)
        tight_loop_contents();

    if (!pio_commo_rx_ready()) return 0;

    return pio_commo_rx_get();
}

void commo_hal_txd(uint8_t b)
{
    pio_commo_tx_byte(b);
}

int commo_hal_data_is_low(void)
{
    return (int)(!gpio_get(PIN_COMMO_DATA));
}

void commo_hal_release(void)
{
    pio_commo_release();
}
