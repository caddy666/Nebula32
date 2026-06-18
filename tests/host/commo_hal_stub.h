#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Reset all stub state — call from test setup() */
void    commo_hal_stub_reset(void);

/* Push one byte into the RX queue (models host driving a byte).
 * data_is_low() automatically returns 1 whenever the RX queue is non-empty,
 * so most tests do not need to call set_data_low() for normal packet delivery. */
void    commo_hal_stub_push(uint8_t b);

/* Override the data_is_low signal independently of queue state.
 * Use for adversarial tests: bus-glitch while TX in progress, etc. */
void    commo_hal_stub_set_data_low(int val);

/* How many bytes the SM has transmitted so far */
int     commo_hal_stub_tx_count(void);

/* The i-th byte transmitted by the SM (0-indexed) */
uint8_t commo_hal_stub_tx_byte(int i);

/* How many times commo_hal_release() was called (2 per SEND_STRING_COMPLETE TX) */
int     commo_hal_stub_release_count(void);

/* True if stub_push() was called when the RX queue was already full */
bool    commo_hal_stub_rx_overflow(void);

/* True if commo_hal_txd() was called when the TX log was already full */
bool    commo_hal_stub_tx_overflow(void);
