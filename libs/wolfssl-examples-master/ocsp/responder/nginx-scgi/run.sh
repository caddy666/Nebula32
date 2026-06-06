#!/bin/sh
#
# run.sh - Start an OCSP responder using wolfclu (SCGI) behind nginx.
#
# This script:
#   1. Starts wolfclu in SCGI mode on port 8081
#   2. Starts nginx on port 8080, forwarding to wolfclu via SCGI
#   3. Sends a test OCSP query using wolfssl's built-in test certs
#
# Prerequisites:
#   - wolfssl built with: --enable-ocsp --enable-ocsp-responder
#   - wolfclu built and installed
#   - nginx installed with SCGI support (default in most packages)
#
# Usage:
#   ./run.sh [options]
#
# Options:
#   --ca-cert <file>    CA certificate (default: wolfSSL test ca-cert.pem)
#   --ca-key  <file>    CA private key (default: wolfSSL test ca-key.pem)
#   --index   <file>    OpenSSL-format index.txt (optional)
#   --port    <num>     nginx listen port (default: 8080)
#   --scgi-port <num>   wolfclu SCGI port (default: 8081)

set -e

# Defaults - use wolfSSL test certificates
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CA_CERT="${SCRIPT_DIR}/../../../certs/ca-cert.pem"
CA_KEY="${SCRIPT_DIR}/../../../certs/ca-key.pem"
INDEX_FILE=""
HTTP_PORT=8080
SCGI_PORT=8081
WOLFCLU_PID=""
NGINX_PID=""

cleanup() {
    echo ""
    echo "Shutting down..."
    [ -n "$WOLFCLU_PID" ] && kill "$WOLFCLU_PID" 2>/dev/null || true
    [ -n "$NGINX_PID" ]   && kill "$NGINX_PID"   2>/dev/null || true
    wait 2>/dev/null || true
    [ -n "$WORK_DIR" ] && rm -rf "$WORK_DIR"
    echo "Done."
}
trap cleanup EXIT INT TERM

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --ca-cert)   CA_CERT="$2"; shift 2 ;;
        --ca-key)    CA_KEY="$2"; shift 2 ;;
        --index)     INDEX_FILE="$2"; shift 2 ;;
        --port)      HTTP_PORT="$2"; shift 2 ;;
        --scgi-port) SCGI_PORT="$2"; shift 2 ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Validate files exist
if [ ! -f "$CA_CERT" ]; then
    echo "Error: CA cert not found: $CA_CERT"
    exit 1
fi
if [ ! -f "$CA_KEY" ]; then
    echo "Error: CA key not found: $CA_KEY"
    exit 1
fi

# Check for required tools
if ! command -v wolfssl >/dev/null 2>&1; then
    echo "Error: 'wolfssl' (wolfCLU) not found in PATH"
    echo "Build wolfCLU from https://github.com/wolfSSL/wolfCLU"
    exit 1
fi
if ! command -v nginx >/dev/null 2>&1; then
    echo "Error: nginx not found in PATH"
    exit 1
fi

echo "=== OCSP Responder: nginx + wolfclu (SCGI) ==="
echo ""
echo "CA cert:    $CA_CERT"
echo "CA key:     $CA_KEY"
echo "HTTP port:  $HTTP_PORT (nginx)"
echo "SCGI port:  $SCGI_PORT (wolfclu)"
echo ""

# --- Step 1: Start wolfclu OCSP responder in SCGI mode ---
echo "Starting wolfclu OCSP responder (SCGI on port $SCGI_PORT)..."

set -- -scgi -port "$SCGI_PORT" -rsigner "$CA_CERT" -rkey "$CA_KEY" -CA "$CA_CERT"
if [ -n "$INDEX_FILE" ]; then
    set -- "$@" -index "$INDEX_FILE"
fi

wolfssl ocsp "$@" &
WOLFCLU_PID=$!
sleep 1

if ! kill -0 "$WOLFCLU_PID" 2>/dev/null; then
    echo "Error: wolfclu failed to start"
    exit 1
fi
echo "wolfclu started (PID $WOLFCLU_PID)"

# --- Step 2: Generate nginx config with correct ports ---
WORK_DIR="$(mktemp -d "$SCRIPT_DIR/tmp.XXXXXX")"
NGINX_CONF="$WORK_DIR/nginx-ocsp.conf"
cat > "$NGINX_CONF" <<EOF
daemon off;
pid $WORK_DIR/nginx-ocsp.pid;
error_log /dev/stderr info;

events {
    worker_connections 64;
}

http {
    client_body_temp_path $WORK_DIR/body;
    proxy_temp_path       $WORK_DIR/proxy;
    fastcgi_temp_path     $WORK_DIR/fastcgi;
    uwsgi_temp_path       $WORK_DIR/uwsgi;
    scgi_temp_path        $WORK_DIR/scgi;

    access_log /dev/stdout;

    server {
        listen $HTTP_PORT;

        location / {
            scgi_pass 127.0.0.1:$SCGI_PORT;

            scgi_param REQUEST_METHOD  \$request_method;
            scgi_param REQUEST_URI     \$request_uri;
            scgi_param QUERY_STRING    \$query_string;
            scgi_param CONTENT_TYPE    \$content_type;
            scgi_param CONTENT_LENGTH  \$content_length;
            scgi_param DOCUMENT_URI    \$document_uri;
            scgi_param DOCUMENT_ROOT   \$document_root;
            scgi_param SCGI            1;
            scgi_param SERVER_PROTOCOL \$server_protocol;
            scgi_param REQUEST_SCHEME  \$scheme;
            scgi_param HTTPS           \$https if_not_empty;
            scgi_param REMOTE_ADDR     \$remote_addr;
            scgi_param REMOTE_PORT     \$remote_port;
            scgi_param SERVER_PORT     \$server_port;
            scgi_param SERVER_NAME     \$server_name;
        }
    }
}
EOF

echo "Starting nginx (HTTP on port $HTTP_PORT)..."
nginx -c "$NGINX_CONF" &
NGINX_PID=$!
sleep 1

if ! kill -0 "$NGINX_PID" 2>/dev/null; then
    echo "Error: nginx failed to start"
    exit 1
fi
echo "nginx started (PID $NGINX_PID)"

echo ""
echo "=== OCSP responder is running ==="
echo ""
echo "Test with wolfssl:"
echo "  wolfssl ocsp -issuer $CA_CERT -cert ../../certs/server-cert.pem \\"
echo "    -url http://127.0.0.1:$HTTP_PORT/"
echo ""
echo "Test with openssl:"
echo "  openssl ocsp -issuer $CA_CERT -cert ../../certs/server-cert.pem \\"
echo "    -url http://127.0.0.1:$HTTP_PORT/ -resp_text"
echo ""
echo "Press Ctrl-C to stop."
echo ""

# Wait for either process to exit
wait
