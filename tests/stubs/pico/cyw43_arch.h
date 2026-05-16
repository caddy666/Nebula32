#pragma once
#include "lwip/ip4_addr.h"

#define CYW43_AUTH_WPA2_AES_PSK 0

// Minimal cyw43_state shape — only netif[0].ip_addr is accessed by webserver.c.
typedef struct {
    struct { ip4_addr_t ip_addr; } netif[2];
} cyw43_state_t;

// Per-TU static instance; webserver_init() writes to it, but is never called
// in test builds so the initial zero value is always seen.
static cyw43_state_t cyw43_state;

static inline int  cyw43_arch_init(void)              { return 0; }
static inline void cyw43_arch_enable_sta_mode(void)   {}
static inline int  cyw43_arch_wifi_connect_timeout_ms(
    const char *s, const char *p, int a, int ms)
    { (void)s; (void)p; (void)a; (void)ms; return 0; }
static inline void cyw43_arch_poll(void) {}
