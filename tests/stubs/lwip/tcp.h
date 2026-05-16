#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef int err_t;
#define ERR_OK  0
#define ERR_VAL (-3)
#define ERR_MEM (-1)

#define TCP_WRITE_FLAG_COPY 0x01
#define TCP_PRIO_MIN        0
#define IPADDR_TYPE_V4      0
#define IP_ANY_TYPE         0

struct tcp_pcb { int _d; };

static inline err_t tcp_write(struct tcp_pcb *p, const void *d, uint16_t l, uint8_t f)
    { (void)p; (void)d; (void)l; (void)f; return ERR_OK; }
static inline err_t tcp_output(struct tcp_pcb *p)
    { (void)p; return ERR_OK; }
static inline err_t tcp_close(struct tcp_pcb *p)
    { (void)p; return ERR_OK; }
static inline struct tcp_pcb *tcp_new_ip_type(int t)
    { (void)t; return (struct tcp_pcb *)0; }
static inline err_t tcp_bind(struct tcp_pcb *p, void *a, uint16_t port)
    { (void)p; (void)a; (void)port; return ERR_OK; }
static inline struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p, uint8_t b)
    { (void)p; (void)b; return (struct tcp_pcb *)0; }
static inline void tcp_accept(struct tcp_pcb *p, void *cb)
    { (void)p; (void)cb; }
static inline void tcp_arg(struct tcp_pcb *p, void *a)
    { (void)p; (void)a; }
static inline void tcp_recv(struct tcp_pcb *p, void *cb)
    { (void)p; (void)cb; }
static inline void tcp_setprio(struct tcp_pcb *p, uint8_t prio)
    { (void)p; (void)prio; }
static inline void tcp_recved(struct tcp_pcb *p, uint16_t len)
    { (void)p; (void)len; }
