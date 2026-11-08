# uchttp — Microcontroller HTTP Server

A lightweight, header-only HTTP/1.1 server for bare-metal, FreeRTOS, and Zephyr targets.
Zero heap allocations, zero-copy parsing, CRTP-based socket abstraction.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          User Application                           │
│                                                                     │
│   server.get("/api/data", handler);                                 │
│   server.post("/api/item", handler);                                │
│   server.run(acceptor);                                             │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ add_route / run / poll
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     HttpServer<Socket>                              │
│                                                                     │
│   ┌─────────────────────────────┐                                   │
│   │   RouteTable<MaxRoutes>     │  fixed-size array, no heap        │
│   │                             │                                   │
│   │  [GET  /         handler]   │                                   │
│   │  [POST /api/item handler]   │  linear search                    │
│   │  [PUT  /api/item handler]   │                                   │
│   │  ...                        │                                   │
│   └─────────────────────────────┘                                   │
│                                                                     │
│   loop: accept → create HttpConnection → process → close            │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ accept()
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│              Acceptor  (satisfies AcceptorConcept)                  │
│                                                                     │
│   Linux:  LinuxAcceptor  (POSIX accept())                           │
│   RTOS:   user-supplied (e.g. lwIP netconn_accept)                  │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ Socket (move)
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│           HttpConnection<Socket, RxSize, TxSize>                    │
│                                                                     │
│   ┌──────────────────────────┐   ┌──────────────────────────┐       │
│   │  RX buffer [RxSize]      │   │  TX buffer [TxSize]      │       │
│   │  (stack / static)        │   │  (stack / static)        │       │
│   └──────────┬───────────────┘   └──────────────────────────┘       │
│              │ raw bytes (zero-copy)                                 │
│              ▼                                                       │
│   ┌──────────────────────────┐                                       │
│   │   llhttp parser          │  incremental, no alloc               │
│   │                          │                                       │
│   │  on_url ──────────────►  │  string_view into RX buf             │
│   │  on_header_field/value ► │  string_view into RX buf             │
│   │  on_body ─────────────►  │  string_view into RX buf             │
│   │  on_message_complete ──► │  sets ParseState.complete            │
│   └──────────┬───────────────┘                                       │
│              │ populates                                             │
│              ▼                                                       │
│   ┌──────────────────────────┐                                       │
│   │   ParseState             │                                       │
│   │   ├─ HttpRequest         │                                       │
│   │   ├─ HttpRequest         │                                       │
│   │   │   ├─ method          │  enum (GET/POST/PUT/DELETE/…)         │
│   │   │   ├─ path            │  string_view → RX buf (query stripped)│
│   │   │   ├─ raw_query       │  string_view → RX buf (after '?')     │
│   │   │   ├─ path_params[]   │  {name,value} views → RX buf          │
│   │   │   ├─ body            │  string_view → RX buf                │
│   │   │   └─ headers[]       │  string_view pairs → RX buf          │
│   │   └─ message_complete    │                                       │
│   └──────────┬───────────────┘                                       │
│              │ RouteTable::find(method, path, request)               │
│              ▼                                                       │
│   ┌──────────────────────────┐   ┌──────────────────────────┐       │
│   │   Route handler (fn ptr) │──►│   HttpResponse           │       │
│   │                          │   │   ├─ set_status(code)    │       │
│   └──────────────────────────┘   │   ├─ set_body(sv)        │       │
│                                  │   ├─ add_header(k,v)     │       │
│                                  │   └─ send(socket)        │       │
│                                  │     writes to TX buf,    │       │
│                                  │     then socket.write()  │       │
│                                  └──────────────────────────┘       │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ read() / write()
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│              Socket  (satisfies SocketConcept)                      │
│                                                                     │
│   int  read (span<uint8_t>)        ◄── TCP recv                     │
│   int  write(span<const uint8_t>)  ──► TCP send                     │
│   void close()                                                      │
│                                                                     │
│   Linux:  LinuxSocket   (POSIX recv/send)                           │
│   Tests:  MockSocket    (in-memory buffers)                         │
│   RTOS:   user-supplied (lwIP / Zephyr BSD sockets / netconn)       │
└─────────────────────────────────────────────────────────────────────┘
```

### Key properties

| Property | Detail |
|---|---|
| **Zero heap** | All buffers are stack or statically allocated; no `new` / `malloc` |
| **Zero-copy parsing** | `HttpRequest` fields are `string_view`s into the RX buffer |
| **CRTP / Concepts** | Socket and Acceptor are template parameters — fully inlined, no vtable |
| **llhttp** | Battle-tested incremental parser from Node.js (v9.4.3) |
| **Static routing** | `RouteTable<N>` is a fixed array; size is a compile-time constant |
| **Handler signature** | `void(const HttpRequest&, HttpResponse&)` — plain function pointer |
| **Middleware** | Chain of Responsibility; `void(req, res, MiddlewareChain&)` fn ptrs, fixed array |
| **WebSocket** | RFC 6455 endpoints as routes; pluggable SHA-1 and buffer allocator |
| **Portability** | Core library (`uchttp.h`) has no OS or POSIX dependencies |

## Quick start (Linux)

```bash
cmake -B build && cmake --build build
./build/uchttp_server          # listens on http://localhost:8080

curl http://localhost:8080/api/hello
curl -X POST http://localhost:8080/api/echo -d 'hello'
curl -X PUT  http://localhost:8080/api/item -d '{"id":1}'
curl http://localhost:8080/api/sensors/42              # path parameter
curl 'http://localhost:8080/api/search?q=temp&limit=5' # query string
curl -X DELETE http://localhost:8080/api/sensors/42    # DELETE method
curl -X OPTIONS http://localhost:8080/api/data -i      # CORS preflight
```

## Advanced routing & request parsing

### Path parameters

Register dynamic segments with `{name}` and read them zero-copy from the
request — no heap allocation, the value is a `string_view` into the RX buffer:

```cpp
server.get("/api/sensors/{id}", [](const HttpRequest& req, HttpResponse& res) {
    std::string_view id = req.path_param("id");   // e.g. "42"
    // ... look up sensor by id ...
    res.set_status(200);
});
```

Literal routes always win over patterns, so `/api/sensors/all` can be
registered alongside `/api/sensors/{id}`.

### Query string

The query string (everything after `?`) is parsed lazily by an allocation-free
iterator:

```cpp
server.get("/api/search", [](const HttpRequest& req, HttpResponse& res) {
    std::string_view q     = req.query_param("q");      // ?q=temp
    std::string_view limit = req.query_param("limit");  // &limit=5

    // Or iterate over every pair:
    for (auto [key, value] : req.query()) { /* ... */ }

    res.set_status(200);
});
```

`req.path` always contains the path only; the query string is kept separately
in `req.raw_query`.

### HTTP methods

Full support for `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD` and `OPTIONS`
(the last is required for CORS preflight):

```cpp
server.get    ("/api/data", handler);
server.post   ("/api/data", handler);
server.put    ("/api/data", handler);
server.del    ("/api/data", handler);
server.patch  ("/api/data", handler);
server.head   ("/api/data", handler);
server.options("/api/data", handler);  // CORS preflight
```

A request to a known path with an unregistered method returns
`405 Method Not Allowed`; an unknown path returns `404 Not Found`.

## Middleware (Chain of Responsibility)

Register global middleware with `server.use(...)`. A middleware is a plain
function pointer — no `std::function`, no heap:

```cpp
void my_mw(const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain);
```

Each middleware may inspect or modify the request/response, then call
`chain.next(req, res)` to pass control to the next middleware and, finally, the
route handler. Because `next()` is called *inside* the middleware, code placed
**after** the call runs on the way back out — the "onion" model. This is ideal
for timing, logging and response-header injection (CORS / security headers).

```cpp
// Security headers injected into every response (runs after the handler):
server.use([](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
    chain.next(req, res);
    res.add_header("X-Content-Type-Options", "nosniff");
    res.add_header("X-Frame-Options", "DENY");
});

// Auth middleware that short-circuits the chain (handler never runs):
server.use([](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
    if (req.get_header("X-API-Key") != "secret") {
        res.set_status(401);
        return;               // do NOT call next() -> handler is skipped
    }
    chain.next(req, res);
});
```

Middleware run in registration order; two registered as `A` then `B` wrap the
handler as `A → B → handler → B → A`. The table is a fixed-size array sized by
the `UCHTTP_MAX_MIDDLEWARE` compile-time constant (default 8).

### Bundled application middleware

`#include "uchttp_app_middleware.h"` — optional, opt-in middleware for the
headers that belong to the application layer (requirement #5):

```cpp
server.use(uchttp::app::request_id);          // echo/generate X-Request-ID
uchttp::app::set_cache_control("public, max-age=60");
server.use(uchttp::app::cache_control);       // Cache-Control on every response
server.use(uchttp::app::etag);                // ETag + If-None-Match -> 304
```

| Middleware | Header(s) | Behaviour |
|---|---|---|
| `cache_control` | `Cache-Control` | Injects the configured value (`set_cache_control`) |
| `etag` | `ETag`, `If-None-Match` | FNV-1a of the body; matching `If-None-Match` ⇒ `304 Not Modified` with an empty body |
| `request_id` | `X-Request-ID` | Echoes an incoming id or generates one for log correlation |
| `client_accepts_encoding()` | `Accept-Encoding` | Helper to negotiate compression in a handler |

(Auth and security-header middleware live in `uchttp_security.h`; see below.)

## Protocol headers handled by the engine

Some headers are owned by the server core, never by middleware, because they
decide how the sockets physically talk to each other (requirement #3):

| Header | Handling |
|---|---|
| **Host** | Parsed; exposed via `req.host()` for (virtual-host) routing |
| **Content-Length** | Exposed via `req.content_length`; drives body framing |
| **Transfer-Encoding: chunked** | Exposed via `req.chunked`; body is decoded and compacted in-place (zero extra buffer) |
| **Connection** | Drives the keep-alive loop; the engine emits `Connection: keep-alive` or `Connection: close` in the response |
| **Content-Type / Content-Encoding** | Set on the response via `res.set_content_type()` / `res.set_content_encoding()` |

### Keep-alive

A single `HttpConnection` serves consecutive requests on one socket. The engine
reads `llhttp_should_keep_alive()` (which accounts for the HTTP version and the
`Connection` header): HTTP/1.1 defaults to keep-alive, HTTP/1.0 and explicit
`Connection: close` end the loop after the current response. The parser is
paused on each message boundary so a single `read()` carrying more than one
pipelined request never mixes request state; leftover bytes are compacted to
the front of the RX buffer for the next round.

### Protocol error responses

Generated by the engine before any middleware or handler runs:

- **400 Bad Request** — malformed HTTP syntax (llhttp parse error)
- **405 Method Not Allowed** — path exists but not for this method
- **501 Not Implemented** — a method uchttp does not support (e.g. TRACE/CONNECT)
- **404 Not Found** — unknown path

## Security & access control

### DoS / Slowloris protection

The engine caps the request header block and body against the fixed RX buffer,
and bounds how long a peer may stall:

| Limit | Default | On breach |
|---|---|---|
| `UCHTTP_MAX_HEADER_BYTES` | 2048 | `431 Request Header Fields Too Large` |
| `UCHTTP_MAX_BODY_BYTES` | 4096 | `413 Content Too Large` |
| `UCHTTP_IDLE_TIMEOUT_MS` | 5000 | connection dropped (Slowloris) |

The idle timeout is applied by `LinuxAcceptor` via `LinuxSocket::set_read_timeout()`
(`SO_RCVTIMEO`); a stalled `recv` is treated as EOF and the socket is closed.

### Authentication middleware

`#include "uchttp_security.h"`. Constant-time `secure_compare()` guards against
timing side-channels; both middleware **fail closed** when unconfigured:

```cpp
uchttp::security::set_api_key("s3cr3t");
server.use(uchttp::security::api_key_auth);     // checks X-API-Key, 401 on miss

uchttp::security::set_bearer_token("tok-123");
server.use(uchttp::security::bearer_auth);      // Authorization: Bearer <token>
```

### Security-headers middleware

Injects CORS + hardening headers into every response (configurable):

```cpp
server.use(uchttp::security::security_headers);
// Access-Control-Allow-Origin: *      (set_cors_origin)
// X-Content-Type-Options: nosniff
// X-Frame-Options: DENY               (set_frame_options)
// Content-Security-Policy: default-src 'self'   (set_csp)
```

### TLS / HTTPS

The crypto library is pluggable behind `TlsSessionConcept`. `TlsSocket<Session>`
itself satisfies `SocketConcept`, so the engine runs over TLS unchanged — no
virtuals, fully inlined. Two backends ship:

- **OpenSSL** (`OpenSslContext` / `OpenSslSession`) — host builds, always built
  when OpenSSL is found.
- **wolfSSL** (`WolfSslContext` / `WolfSslSession`) — embedded builds, built
  when wolfSSL is present (`-DUCHTTP_WITH_WOLFSSL=ON`, default).

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

Both certificate and pre-shared-key (PSK) authentication are supported; TLS 1.2
is the minimum protocol version. See `samples/linux/https_server/main.cpp` for a runnable
example:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key \
    -out server.crt -days 1 -subj '/CN=localhost'
./build/uchttp_https_server server.crt server.key
curl -k https://localhost:8443/api/hello
```


## Running tests

```bash
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/uchttp_tests
```

## IoT edge capabilities

### Streaming body / OTA firmware upload

A streaming route hands the request body to the handler in chunks as it
arrives — the engine never buffers the whole payload, so a multi-megabyte
firmware image flows straight to the flash writer (requirement #7):

```cpp
void ota(const HttpRequest& req, HttpResponse& res,
         std::span<const uint8_t> chunk, size_t offset, bool last) {
    flash_write(partition, offset, chunk);      // no RAM buffering
    if (last) { res.set_status(201); res.set_body("flashed"); }
}
server.post_stream("/api/ota", ota);            // also put_stream(...)
```

Chunks are views into the RX buffer (zero copy); both `Content-Length` and
`Transfer-Encoding: chunked` uploads are supported, and the body-size cap
(`UCHTTP_MAX_BODY_BYTES`) does **not** apply to streaming routes.

### Server-Sent Events (SSE)

The handler keeps the connection open and pushes `text/event-stream` frames
straight to the socket:

```cpp
server.get("/api/events", [](const HttpRequest& req, HttpResponse& res) {
    res.begin_sse();                            // 200 + text/event-stream
    res.send_event("23.5", "temperature", "42"); // data / event / id
});
```

`begin_sse()` emits the status line and headers; `send_event()` writes one
frame (multi-line data is split into several `data:` lines). Once the handler
streams its own bytes it "hijacks" the response, so the engine does not append
a buffered reply. Both work unchanged over TLS (`TlsSocket`).

## WebSocket (RFC 6455)

`#include "uchttp.h"` (pulls in `uchttp_ws.h`). A WebSocket endpoint is a
route; the upgrade request goes through the middleware chain like any other
request (auth, security headers), and only a `101` reply hands the socket
over to the WebSocket session (requirement #8).

```cpp
void on_message(uchttp::ws::Session& s, uchttp::ws::Opcode type,
                std::span<const uint8_t> data) {
    if (type == uchttp::ws::Opcode::Text)
        s.send_text({reinterpret_cast<const char*>(data.data()), data.size()});
    else
        s.send_binary(data);
}
void on_idle(uchttp::ws::Session& s) { s.send_text(R"({"temp":23.5})"); } // every idle tick

static uchttp::ws::Handlers ws_handlers {
    .on_message = on_message,
    .on_idle    = on_idle,
};
server.websocket("/ws", ws_handlers);       // path parameters work too: "/ws/{id}"
```

API is callback based (plain function pointers, no `std::function`, no
virtuals): `on_open`, `on_message` (whole message, fragments reassembled,
text validated as UTF-8), `on_close` (exactly once, with the peer's code or
1006), `on_idle` (read timeout tick - periodic server push without an extra
thread), `accept` (gate before the `101`, e.g. an `Origin` check -> 403),
`subprotocol`, and `user` (context returned by `Session::user()`). The
session sends with `send_text()` / `send_binary()` / `ping()` / `close()`.

What the engine does:

| Area | Behaviour |
|---|---|
| Handshake | `Upgrade` / `Connection` / `Sec-WebSocket-Version: 13` / `Sec-WebSocket-Key` validated (names case-insensitive); `426` with hints or `400` otherwise; `Sec-WebSocket-Accept` = base64(SHA-1(key + GUID)) |
| Framing | masked client frames required, RSV bits and reserved opcodes rejected, minimal length encoding enforced, control frames <= 125 B and unfragmented, Ping -> Pong, close codes validated |
| Limits | `UCHTTP_WS_MAX_FRAME_SIZE` checked from the header alone (a 4 GiB frame is refused without reading it), `UCHTTP_WS_MAX_MESSAGE_SIZE` after reassembly -> close 1009 |
| Keep-alive | Ping after `UCHTTP_WS_PING_INTERVAL_MS` of silence, peer dropped after the same time again (default 15 s / 30 s) |
| Memory | one session buffer per connection from the allocator, frames parsed and reassembled in place; small outgoing frames coalesced into the TX buffer (one write) |

### Platform hooks

Both are compile-time plug-ins bundled in `ws::Traits<Sha1, Allocator>` and
given to `HttpServer` as its last template parameter (default
`ws::DefaultTraits`):

| Concept | Default | Zephyr port |
|---|---|---|
| `ws::Sha1Concept` (`start/update/finish`) | `ws::SoftSha1` (portable) | `ZephyrPsaSha1` - PSA Crypto, HW-accelerated where the PSA driver supports SHA-1 |
| `ws::AllocatorConcept` (`allocate/deallocate`) | `ws::StaticAllocator<N, Size>` (static storage) | `ZephyrSlabAllocator<N, Size>` - `k_mem_slab` |

```cpp
using WsTraits = uchttp::ws::Traits<MySha1, MyAllocator>;
uchttp::HttpServer<MySocket, UCHTTP_MAX_ROUTES, UCHTTP_RX_BUFFER_SIZE,
                   UCHTTP_TX_BUFFER_SIZE, UCHTTP_MAX_MIDDLEWARE,
                   UCHTTP_MAX_CONNECTIONS, WsTraits> server;
```

An exhausted allocator refuses the upgrade with `503`. `HttpServer` serves one
connection at a time and a WebSocket keeps its connection, so serve N
concurrent clients with N server instances (threads) sharing one acceptor -
see the examples.

### Host demo

```bash
./build/uchttp_ws_server                  # 4 worker threads, port 8080
# browser: http://localhost:8080/  (JS client)   or:
wscat -c ws://localhost:8080/ws           # echo; "close" closes; telemetry every 1 s
```

## Reliability & resource management

On an MCU the socket/descriptor pool is tiny, so uchttp never allocates a
connection on demand (requirement #6):

- **Connection pool** — `HttpServer` owns a `ConnectionPool<Socket, N, ...>`: a
  fixed, pre-allocated array of `N` connection slots (`UCHTTP_MAX_CONNECTIONS`,
  default 4), each with its own RX/TX buffers and parser state. `run()` acquires
  an idle slot per client and releases it when done. When the pool is exhausted
  the new connection is dropped rather than growing the heap.
- **Timeouts** — the keep-alive loop (requirement #3) plus the idle read timeout
  (`UCHTTP_IDLE_TIMEOUT_MS`, requirement #4) reclaim slots held by dead/stalled
  peers.
- **Graceful shutdown** — `server.stop()` ends the accept loop and
  `server.shutdown()` (or `ConnectionPool::close_all()`) closes every active
  slot cleanly, e.g. on a network-stack reset or reboot.

```cpp
uchttp::HttpServer<MySocket> server;   // pool sized by UCHTTP_MAX_CONNECTIONS
server.run(acceptor);                  // acquire/serve/release per client
// from a signal handler / supervisor task:
server.stop();                         // leave run()
server.shutdown();                     // close all active connections
```

## Porting to an RTOS / bare-metal target

1. Implement `read()`, `write()`, `close()`, and `is_valid()` for your TCP stack (lwIP, Zephyr BSD sockets, etc.).
2. Implement an acceptor with `accept()` and `is_running()`.
3. Instantiate `HttpServer<YourSocket>` and call `server.run(acceptor)` from your task/thread.

No other changes are needed — the core library has no OS dependencies.

## Zephyr OS integration

uchttp ships as a Zephyr module (Stage 1: HTTP; Stage 2: HTTPS/TLS). The module
build is kept separate from the host CMake build:

```
zephyr/module.yml      declares the module (build cmake + Kconfig)
zephyr/CMakeLists.txt  zephyr_library() — core + middleware + port + llhttp
zephyr/Kconfig         CONFIG_UCHTTP, CONFIG_UCHTTP_TLS and CONFIG_UCHTTP_* tunables
src/port/zephyr/zephyr_socket.{h,cpp}   ZephyrSocket + ZephyrAcceptor (zsock_*/net_* API)
src/port/zephyr/zephyr_tls.{h,cpp}      ZephyrTlsAcceptor + ZephyrTlsCredentials (TLS, Stage 2)
samples/zephyr/http_server/      runnable HTTP sample (HttpServer<ZephyrSocket>)
samples/zephyr/https_server/     runnable HTTPS sample (TLS, Stage 2)
samples/zephyr/ws_server/        runnable WebSocket sample
```

The core (`uchttp.h`) is unchanged — only the socket/acceptor port differs.
`ZephyrSocket` uses Zephyr's native `zsock_*` / `net_*` socket API (not the
POSIX aliases), so it builds cleanly on both `native_sim` and real targets.

### Building the sample (west)

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

### HTTPS / TLS (Stage 2)

On Zephyr, TLS is handled transparently at the socket layer by the native
secure-sockets stack (backed by **Mbed TLS / PSA Crypto**): a listening socket
is created with `IPPROTO_TLS_1_2` and bound to a credential tag, and every
accepted socket encrypts/decrypts inside `recv()`/`send()`. Because of that the
plaintext `ZephyrSocket` works unchanged over TLS — Stage 2 only adds a
TLS-aware acceptor (`ZephyrTlsAcceptor`) plus a credential helper
(`ZephyrTlsCredentials`). The engine (`HttpServer<ZephyrSocket>`) is identical.

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
`CONFIG_NET_SOCKETS_SOCKOPT_TLS`, `CONFIG_TLS_CREDENTIALS` and `CONFIG_MBEDTLS`;
size the Mbed TLS heap and content buffers in your `prj.conf`
(`CONFIG_MBEDTLS_HEAP_SIZE`, `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` /
`..._OUT_CONTENT_LEN`).

```bash
# HTTPS sample — host sanity check (WiFi is swapped for native TAP Ethernet)
west build -p always -b native_sim samples/zephyr/https_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)

# Target board (NXP FRDM-RW612) — uses the on-SoC WiFi + ELS/PKC PSA driver
west build -p always -b frdm_rw612 samples/zephyr/https_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)
```

On FRDM-RW612, connect WiFi from the shell, then:

```bash
curl -k https://<device-ip>:8443/api/hello   # -k: accept the demo self-signed cert
```

The sample embeds a self-signed demo certificate in
`samples/zephyr/https_server/src/certificate.h` (regenerate with the `openssl`
command noted in that header). Both `native_sim` and `frdm_rw612` HTTPS builds
are verified against Zephyr 4.5 / SDK 1.0.

### WebSocket on Zephyr

The WebSocket core is always part of the module; the Zephyr plug-ins live in
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

Notes from bring-up on FRDM-RW612:

- `CONFIG_UCHTTP` selects `CONFIG_NET_CONTEXT_RCVTIMEO`: without it Zephyr
  silently ignores `SO_RCVTIMEO`, which disables both the HTTP idle timeout
  (Slowloris guard) and the WebSocket idle tick / keep-alive.
- Enabling PSA Crypto also enables `CONFIG_NET_TCP_ISN_RFC6528` (a SHA-256 per
  incoming SYN, computed in the network RX thread). The default 1500 B `rx_q`
  stack overflows; the sample sets `CONFIG_NET_RX_STACK_SIZE=4096`.

Verified on the board: python `websockets` and `wscat` clients (echo of
text/binary/UTF-8/fragmented/1000 B messages, ping/pong, 1 s push, close
handshake), protocol-violation tests (1002/1007/1009), dead-peer drop after
Ping (15 s / 30 s), two parallel sessions with a third queued until a slot
frees. Peak stack use of a server thread: ~2.2 KB of 6 KB.

## File map

```
include/uchttp.h          Core library (engine, routing, middleware, pool)
include/uchttp_security.h Auth + security-headers middleware, secure_compare
include/uchttp_app_middleware.h  Cache-Control / ETag / Accept-Encoding / X-Request-ID
include/uchttp_ws.h       WebSocket core: codec, Session, Handlers, Sha1/Allocator concepts
include/uchttp_tls.h      TLS abstraction (TlsSocket / TlsAcceptor)
include/openssl_tls.h     OpenSSL backend (host)
include/wolfssl_tls.h     wolfSSL backend (embedded)
src/uchttp_ws*.cpp        WebSocket codec + SHA-1, session loop, handshake
src/port/linux/linux_socket.{h,cpp}  POSIX LinuxSocket + LinuxAcceptor
src/port/zephyr/zephyr_socket.{h,cpp}  Zephyr ZephyrSocket + ZephyrAcceptor
src/port/zephyr/zephyr_tls.{h,cpp}     Zephyr ZephyrTlsAcceptor + ZephyrTlsCredentials (TLS)
src/port/zephyr/zephyr_ws.h            Zephyr ZephyrSlabAllocator (k_mem_slab) for WebSocket
src/port/zephyr/zephyr_psa_sha1.{h,cpp} Zephyr ZephyrPsaSha1 (PSA Crypto SHA-1)
zephyr/                   Zephyr module (module.yml, CMakeLists.txt, Kconfig)
samples/common/ws_demo.h        Demo page + telemetry shared by the WS examples
samples/linux/http_server/      Example HTTP server on localhost
samples/linux/https_server/     Example HTTPS server (OpenSSL)
samples/linux/ws_server/        Example WebSocket server (4 worker threads)
samples/zephyr/http_server/     Zephyr HTTP sample (HttpServer<ZephyrSocket>)
samples/zephyr/https_server/    Zephyr HTTPS sample (TLS, Mbed TLS / PSA Crypto)
samples/zephyr/ws_server/       Zephyr WebSocket sample (echo + telemetry)
tests/mock_socket.h       In-memory socket for unit tests
tests/test_uchttp.cpp     109 gtest unit tests (HTTP engine)
tests/test_uchttp_ws.cpp  106 gtest unit tests (WebSocket)
CMakeLists.txt            Build system
third_party/llhttp/       llhttp v9.4.3
```
