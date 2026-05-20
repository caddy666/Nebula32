#pragma once
#include <stdint.h>

/* Reset all stub state — call from test setup() */
void    commo_hal_stub_reset(void);

/* Push one byte into the RX queue (models host driving a byte) */
void    commo_hal_stub_push(uint8_t b);

/* Control whether commo_hal_data_is_low() returns 1 or 0 */
void    commo_hal_stub_set_data_low(int val);

/* How many bytes the SM has transmitted so far */
int     commo_hal_stub_tx_count(void);

/* The i-th byte transmitted by the SM (0-indexed) */
uint8_t commo_hal_stub_tx_byte(int i);
