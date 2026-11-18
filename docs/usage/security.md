<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Security and access control

[← Usage index](README.md)

## DoS and Slowloris protection

The engine caps the request header block and body against the fixed RX buffer,
and bounds how long a peer may stall:

| Limit | Default | On breach |
|---|---|---|
| `UCHTTP_MAX_HEADER_BYTES` | 2048 | `431 Request Header Fields Too Large` |
| `UCHTTP_MAX_BODY_BYTES` | 4096 | `413 Content Too Large` |
| `UCHTTP_IDLE_TIMEOUT_MS` | 5000 | connection dropped (Slowloris) |

The idle timeout is applied by the acceptor (`LinuxAcceptor`, `ZephyrAcceptor`,
`ZephyrTlsAcceptor`) through `set_read_timeout()` (`SO_RCVTIMEO`). A stalled
`recv` is treated as end of stream and the socket is closed. A value of `0`
disables the timeout.

The size limits work together with the RX buffer. A request is read into the RX
buffer in one piece. With the default 1 KiB buffer (`UCHTTP_RX_BUFFER_SIZE`), a
larger request is answered with `400 Bad Request` before the `431` / `413`
limits are reached. Set the RX buffer larger than the limits if you want those
status codes (the unit tests use 8 KiB). Streaming routes are not subject to
`UCHTTP_MAX_BODY_BYTES`.

## Authentication middleware

`#include "uchttp_security.h"`. A constant-time `secure_compare()` guards against
timing side-channels. Both middleware **fail closed**: with no key or token
configured, every request is rejected with `401`.

```cpp
uchttp::security::set_api_key("s3cr3t");
server.use(uchttp::security::api_key_auth);     // checks X-API-Key, 401 on miss

uchttp::security::set_bearer_token("tok-123");
server.use(uchttp::security::bearer_auth);      // Authorization: Bearer <token>
```

`secure_compare(a, b)` is also available for your own secrets (for example in a
streaming handler, which the middleware chain does not cover):

```cpp
if (!uchttp::security::secure_compare(req.get_header("X-API-Key"), "s3cr3t")) {
    res.set_status(401);
    return;
}
```

Header names are matched exactly, including letter case (`X-API-Key`,
`Authorization`). A client that sends them in another case is rejected.

## Security-headers middleware

Injects CORS and hardening headers into every response that passes through the
middleware chain (configurable):

```cpp
server.use(uchttp::security::security_headers);
// Access-Control-Allow-Origin: *      (set_cors_origin)
// X-Content-Type-Options: nosniff
// X-Frame-Options: DENY               (set_frame_options)
// Content-Security-Policy: default-src 'self'   (set_csp)
```

Pass an empty `string_view` to a setter to disable that header.

## TLS / HTTPS

The crypto library is pluggable behind `TlsSessionConcept`. `TlsSocket<Session>`
itself satisfies `SocketConcept`, so the engine runs over TLS unchanged, with no
virtuals and fully inlined. Two backends ship:

- **OpenSSL** (`OpenSslContext` / `OpenSslSession`): host builds, always built
  when OpenSSL is found.
- **wolfSSL** (`WolfSslContext` / `WolfSslSession`): embedded builds, built when
  wolfSSL is present (`-DUCHTTP_WITH_WOLFSSL=ON`, default).

```cpp
uchttp::OpenSslContext ctx;
ctx.load_cert_and_key("server.crt", "server.key");   // or ctx.set_psk(...)

uchttp::LinuxAcceptor raw(8443); raw.start();
uchttp::TlsAcceptor<uchttp::OpenSslContext, uchttp::LinuxAcceptor> tls(ctx, raw);

using TlsSock = uchttp::TlsSocket<uchttp::OpenSslSession>;
uchttp::HttpServer<TlsSock> server;
server.get("/api/hello", handler);
server.run(tls);
```

Both certificate and pre-shared-key (PSK) authentication are supported. TLS 1.2
is the minimum protocol version. See `samples/linux/https_server/main.cpp` for a
runnable example:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key \
    -out server.crt -days 1 -subj '/CN=localhost'
./build/uchttp_https_server server.crt server.key
curl -k https://localhost:8443/api/hello
```

TLS on Zephyr uses the native secure sockets instead. See
[Zephyr](zephyr.md#https--tls).
