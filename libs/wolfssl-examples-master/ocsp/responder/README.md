# OCSP Responder Examples

Examples demonstrating the wolfSSL OCSP Responder API added in
[wolfSSL/wolfssl#9761](https://github.com/wolfSSL/wolfssl/pull/9761).

## Prerequisites

Build and install wolfSSL with OCSP responder support:

```sh
cd wolfssl
./configure --enable-ocsp --enable-ocsp-responder
make
sudo make install
sudo ldconfig
```

## Examples

### 1. Raw DER Request/Response (`ocsp-request-response.c`)

Demonstrates the core API without any networking:

- Parse a certificate and build a DER-encoded OCSP request
  (`wc_InitOcspRequest`, `wc_EncodeOcspRequest`)
- Create an `OcspResponder`, register a signer, and set certificate statuses
  (`wc_OcspResponder_new`, `wc_OcspResponder_AddSigner`,
  `wc_OcspResponder_SetCertStatus`)
- Generate a signed OCSP response from the request
  (`wc_OcspResponder_WriteResponse`)
- Verify the response against a `WOLFSSL_CERT_MANAGER`
  (`wc_CheckCertOcspResponse`)
- Show REVOKED status and error response generation

```sh
make ocsp-request-response
./ocsp-request-response
```

Uses the wolfSSL test certs in `../../certs/` by default.

### 2. Minimal HTTP Responder (`ocsp-responder-http.c`)

A tiny HTTP server that accepts POST requests containing DER OCSP requests and
returns DER OCSP responses. Kept as small as possible.

```sh
make ocsp-responder-http

# Start the responder, marking the server cert as GOOD
./ocsp-responder-http 8080 ../../certs/ca-cert.pem ../../certs/ca-key.pem \
    ../../certs/server-cert.pem

# Test with OpenSSL (in another terminal)
openssl ocsp -issuer ../../certs/ca-cert.pem -cert ../../certs/server-cert.pem \
    -url http://127.0.0.1:8080/ -no_nonce
```

Any certificate files listed after the CA key have their serial numbers
registered as CERT_GOOD. Certificates not registered will get CERT_UNKNOWN.

### 3. nginx + wolfclu SCGI (`nginx-scgi/`)

Production-style deployment: nginx handles HTTP and forwards raw OCSP request
bodies to wolfclu over SCGI. nginx provides TLS termination, access control,
logging, and load balancing while wolfclu focuses on OCSP processing.

Requirements:
- [wolfCLU](https://github.com/wolfSSL/wolfCLU) built and installed
- nginx with SCGI support (enabled by default)

```
+---------+  HTTP POST   +-------+  SCGI   +---------+
|  Client  |------------>| nginx  |-------->| wolfclu  |
|(openssl) |<------------|  :8080 |<--------|  :8081   |
+---------+  OCSP resp   +-------+         +---------+
```

Quick start:

```sh
cd nginx-scgi
./run.sh
```

Or run manually:

```sh
# Terminal 1: Start wolfclu SCGI backend
wolfssl ocsp -scgi -port 8081 \
    -rsigner ../../certs/ca-cert.pem \
    -rkey ../../certs/ca-key.pem \
    -CA ../../certs/ca-cert.pem

# Terminal 2: Start nginx
nginx -c $(pwd)/nginx-scgi/nginx-ocsp.conf

# Terminal 3: Test
openssl ocsp -issuer ../../certs/ca-cert.pem -cert ../../certs/server-cert.pem \
    -url http://127.0.0.1:8080/ -no_nonce
```

The `nginx-ocsp.conf` file can be customized for your environment. See the
comments in the file for standalone vs. installed nginx usage.

## Shared Code

`ocsp-load-certs.h` contains file loading utilities (`LoadFile`, `LoadCertDer`,
`LoadKeyDer`) shared between the C examples.
