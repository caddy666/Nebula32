#pragma once
#include <stdbool.h>

static inline int dma_claim_unused_channel(bool required)
    { (void)required; return 0; }
static inline void dma_channel_abort(int ch)
    { (void)ch; }
