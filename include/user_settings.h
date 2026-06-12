/* user_settings.h — wolfSSL build config for Nebula32 (RP2350 / Pico 2 W)
 *
 * Shadows libs/wolfssl-master vendor copy per the project's ffconf.h pattern.
 * include/ appears before libs/ on every include path so this file wins.
 *
 * Base: libs/wolfssl-examples-master/RPi-Pico/config/user_settings.h
 * Key Nebula32 changes from that base:
 *   WOLFSSL_LWIP_NATIVE  (not WOLFSSL_LWIP) — raw lwIP PCB integration path
 *   Inline XTIME stub   — RP2350 has no RTC; fixed epoch is fine for local HTTPS
 *   NO_RSA              — save ~40 KB flash; we use an ECDSA (P-256) cert
 *   USE_CERT_BUFFERS_256 unconditional — dev cert from <wolfssl/certs_test.h>
 *   SP Math only        — no assembly for initial integration (portable C)
 */

#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Platform — RP2350 Pico 2 W
 * ------------------------------------------------------------------------- */
#define WOLFSSL_RPIPICO          /* enables Pico SDK TRNG (wc_pico_rng_gen_block) */
#define WOLFSSL_LWIP_NATIVE      /* raw lwIP tcp_pcb integration via wolfSSL_SetIO_LwIP */

/* Inline time stub — RP2350 has no battery-backed RTC.
 * Fixed epoch (2024-01-01) is acceptable for a single-user local web UI.
 * wolfSSL uses XTIME for certificate validity window checks. */
static inline time_t nebula32_xtime(time_t *t) {
    time_t now = (time_t)1704067200; /* 2024-01-01 00:00:00 UTC */
    if (t) *t = now;
    return now;
}
#define XTIME(t) nebula32_xtime(t)

/* -------------------------------------------------------------------------
 * Embedded settings
 * ------------------------------------------------------------------------- */
#define SINGLE_THREADED          /* no mutex — poll-mode lwIP, Core 0 only */
#define WOLFSSL_SMALL_STACK      /* heap-allocate large temporaries (< 100 B on stack) */
#define WOLFSSL_USER_IO          /* disable Berkeley socket I/O; lwipCtx provides I/O */
#define WOLFSSL_NO_SOCK          /* no <sys/socket.h> on bare-metal — skips BSD includes in wolfio.h */
#define WOLFSSL_GENERAL_ALIGNMENT 4
#define SIZEOF_LONG_LONG          8

/* -------------------------------------------------------------------------
 * Math — SP Math, portable C (no assembly for initial integration)
 * ------------------------------------------------------------------------- */
#define WOLFSSL_SP_MATH_ALL      /* SP math for all key sizes and curves */
#define WOLFSSL_HAVE_SP_ECC      /* SP ECC acceleration (P-256) */
#define TFM_TIMING_RESISTANT     /* timing-resistant big-number ops */

/* -------------------------------------------------------------------------
 * Ciphers — minimal suite: ECDSA cert + AES-GCM/ChaCha20, TLS 1.3 only
 * ------------------------------------------------------------------------- */

/* ECC — P-256 for ECDSA certificate and key exchange */
#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define ECC_SHAMIR               /* slightly faster point multiply (2× heap, same flash) */

/* AES-GCM */
#undef NO_AES
#define HAVE_AES_CBC
#define HAVE_AESGCM
#define GCM_TABLE_4BIT

/* ChaCha20-Poly1305 */
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_ONE_TIME_AUTH

/* Hashing */
#undef NO_SHA256
#define WOLFSSL_SHA512
#define WOLFSSL_SHA384
#define WOLFSSL_SHA3
#define HAVE_HKDF

/* No RSA — saves ~40 KB flash; ECDSA cert is sufficient for local HTTPS */
#define NO_RSA

/* No DH — TLS 1.3 uses ECDHE; DHE not needed */
#define NO_DH

/* Disable DES3 and old ciphers */
#define NO_DES3
#define NO_OLD_TLS
#define NO_PSK
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_PWDBASED

/* -------------------------------------------------------------------------
 * TLS features
 * ------------------------------------------------------------------------- */
#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define WOLFSSL_BASE64_ENCODE

/* Session cache — disabled to save ~20 KB RAM on embedded target */
#define NO_SESSION_CACHE

/* -------------------------------------------------------------------------
 * RNG — RP2350 hardware TRNG via Pico SDK pico_rand
 * wc_pico_rng_gen_block is implemented in:
 *   libs/wolfssl-master/wolfcrypt/src/port/rpi_pico/pico.c
 * That file is added as a source in CMakeLists.txt when building for pico2_w.
 * ------------------------------------------------------------------------- */
#define WC_NO_HASHDRBG                       /* bypass SHA-256-DRBG; use TRNG directly */
#define CUSTOM_RAND_GENERATE_BLOCK wc_pico_rng_gen_block

/* -------------------------------------------------------------------------
 * Filesystem / stdio — not available in embedded firmware
 * ------------------------------------------------------------------------- */
#define NO_FILESYSTEM
#define NO_WOLFSSL_DIR           /* wc_port.h's dirent.h block is gated on this, not NO_FILESYSTEM */
#define NO_WRITEV
#define NO_MAIN_DRIVER
#define NO_DEV_RANDOM

/* -------------------------------------------------------------------------
 * Misc
 * ------------------------------------------------------------------------- */
#ifndef WOLFSSL_IGNORE_FILE_WARN  /* wolfSSL's CMake also passes this via -D */
#define WOLFSSL_IGNORE_FILE_WARN  /* suppress "file included but not needed" warnings */
#endif
#define BENCH_EMBEDDED            /* reduced benchmark sizes */

#ifdef __cplusplus
}
#endif

#endif /* WOLFSSL_USER_SETTINGS_H */
