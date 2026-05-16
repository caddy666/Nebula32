#pragma once
#include <stdint.h>

struct pbuf {
    void        *payload;
    uint16_t     len;
    uint16_t     tot_len;
    struct pbuf *next;
};

static inline void pbuf_free(struct pbuf *p) { (void)p; }
