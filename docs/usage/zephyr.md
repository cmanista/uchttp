<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Zephyr OS integration

[← Usage index](README.md)

uchttp ships as a Zephyr module (Stage 1: HTTP, Stage 2: HTTPS/TLS, plus
WebSocket). The module build is kept separate from the host CMake build:

```
zephyr/module.yml      declares the module (build cmake + Kconfig)
zephyr/CMakeLists.txt  zephyr_library(): core + middleware + port + llhttp
zephyr/Kconfig         CONFIG_UCHTTP, CONFIG_UCHTTP_TLS and CONFIG_UCHTTP_* tunables
src/port/zephyr/zephyr_socket.{h,cpp}   ZephyrSocket + ZephyrAcceptor (zsock_*/net_* API)
src/port/zephyr/zephyr_tls.{h,cpp}      ZephyrTlsAcceptor + ZephyrTlsCredentials (TLS, Stage 2)
samples/zephyr/http_server/      runnable HTTP sample (HttpServer<ZephyrSocket>)
samples/zephyr/https_server/     runnable HTTPS sample (TLS, Stage 2)
samples/zephyr/ws_server/        runnable WebSocket sample
```

The core (`uchttp.h`) is unchanged. Only the socket and acceptor port differs.
`ZephyrSocket` uses Zephyr's native `zsock_*` / `net_*` socket API (not the POSIX
aliases), so it builds cleanly on both `native_sim` and real targets. The llhttp
sources are listed once in `third_party/llhttp.cmake`, shared with the host
build.

## Building the sample (west)

Once the repo is on your `west` manifest (or via `ZEPHYR_EXTRA_MODULES`):

```bash
# Quick host sanity check
west build -p always -b native_sim samples/zephyr/http_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)

# Target board (NXP FRDM-RW612)
west build -p always -b frdm_rw612 samples/zephyr/http_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)
```

Enable the module and the networking stack from your `prj.conf`:

```
CONFIG_CPP=y
CONFIG_STD_CPP23=y
CONFIG_UCHTTP=y
CONFIG_NETWORKING=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
```

Both `native_sim` and `frdm_rw612` builds are verified for Zephyr 4.5
(SDK 0.17/1.0).

The `CONFIG_UCHTTP_*` symbols map to the compile-time limits described in
[Architecture](../architecture/README.md#compile-time-configuration).

## HTTPS / TLS

On Zephyr, TLS is handled transparently at the socket layer by the native
secure-sockets stack (backed by **Mbed TLS / PSA Crypto**). A listening socket is
created with `IPPROTO_TLS_1_2` and bound to a credential tag, and every accepted
socket encrypts and decrypts inside `recv()` / `send()`. Because of that, the
plaintext `ZephyrSocket` works unchanged over TLS. Stage 2 only adds a TLS-aware
acceptor (`ZephyrTlsAcceptor`) and a credential helper (`ZephyrTlsCredentials`).
The engine (`HttpServer<ZephyrSocket>`) is identical.

```cpp
#include "zephyr_tls.h"

// Register the server certificate + key (PEM/DER, NUL-terminated for PEM).
// The credential store keeps pointers to the buffers, so keep them alive
// (e.g. `static const` in flash).
uchttp::ZephyrTlsCredentials::add_server_cert(
    UCHTTP_TLS_SERVER_CERT_TAG,
    server_certificate, server_certificate_len,
    private_key,        private_key_len);

uchttp::ZephyrTlsAcceptor acceptor(8443, UCHTTP_TLS_SERVER_CERT_TAG);
uchttp::HttpServer<uchttp::ZephyrSocket> server;   // same engine, now over TLS
server.get("/api/hello", handler);
server.run(acceptor);
```

Pre-shared keys (`ZephyrTlsCredentials::add_psk`) and mutual TLS
(`acceptor.set_peer_verify(...)` + `add_ca_cert`) are also supported. Enable the
TLS port with `CONFIG_UCHTTP_TLS=y`, which selects
`CONFIG_NET_SOCKETS_SOCKOPT_TLS`, `CONFIG_TLS_CREDENTIALS` and `CONFIG_MBEDTLS`.
Size the Mbed TLS heap and content buffers in your `prj.conf`
(`CONFIG_MBEDTLS_HEAP_SIZE`, `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` /
`..._OUT_CONTENT_LEN`).

```bash
# HTTPS sample: host sanity check (WiFi is swapped for native TAP Ethernet)
west build -p always -b native_sim samples/zephyr/https_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)

# Target board (NXP FRDM-RW612): uses the on-SoC WiFi + ELS/PKC PSA driver
west build -p always -b frdm_rw612 samples/zephyr/https_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)
```

On FRDM-RW612, connect WiFi from the shell, then:

```bash
curl -k https://<device-ip>:8443/api/hello   # -k: accept the demo self-signed cert
```

The sample embeds a self-signed demo certificate in
`samples/zephyr/https_server/src/certificate.h` (regenerate it with the `openssl`
command noted in that header). Do not ship that certificate in a product. Both
`native_sim` and `frdm_rw612` HTTPS builds are verified against Zephyr 4.5 /
SDK 1.0.

## WebSocket on Zephyr

The WebSocket core is always part of the module. The Zephyr plug-ins live in
`src/port/zephyr/zephyr_ws.h` (`ZephyrSlabAllocator`) and
`src/port/zephyr/zephyr_psa_sha1.{h,cpp}` (`ZephyrPsaSha1`, enabled by
`CONFIG_UCHTTP_WS_SHA1_PSA=y`). Tunables: `CONFIG_UCHTTP_WS_MAX_MESSAGE_SIZE`,
`..._MAX_FRAME_SIZE`, `..._IDLE_INTERVAL_MS`, `..._PING_INTERVAL_MS`,
`..._MAX_SESSIONS`.

`samples/zephyr/ws_server` serves the demo page at `/` and `/ws` (echo, a
telemetry message every second, `close` to close) with
`CONFIG_APP_SERVER_THREADS` (default 2) simultaneous clients:

```bash
west build -p always -b frdm_rw612 samples/zephyr/ws_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)
west flash
# console:  wifi connect -s <SSID> -k 1 -p <PASSWORD>
wscat -c ws://<device-ip>:8080/ws
```

To join WiFi at boot instead, put `CONFIG_APP_WIFI_SSID="..."` and
`CONFIG_APP_WIFI_PSK="..."` into `samples/zephyr/ws_server/local.conf`
(git-ignored) and add `-DEXTRA_CONF_FILE=local.conf` to the build.

## Notes from bring-up on FRDM-RW612

- `CONFIG_UCHTTP` selects `CONFIG_NET_CONTEXT_RCVTIMEO`. Without it, Zephyr
  silently ignores `SO_RCVTIMEO`, which disables both the HTTP idle timeout
  (Slowloris guard) and the WebSocket idle tick and keep-alive.
- Enabling PSA Crypto also enables `CONFIG_NET_TCP_ISN_RFC6528` (a SHA-256 per
  incoming SYN, computed in the network RX thread). The default 1500 B `rx_q`
  stack overflows. The sample sets `CONFIG_NET_RX_STACK_SIZE=4096`.

Verified on the board: python `websockets` and `wscat` clients (echo of
text/binary/UTF-8/fragmented/1000 B messages, ping/pong, 1 s push, close
handshake), protocol-violation tests (1002/1007/1009), dead-peer drop after Ping
(15 s / 30 s), two parallel sessions with a third queued until a slot frees. Peak
stack use of a server thread: about 2.2 KB of 6 KB.
