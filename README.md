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
is the minimum protocol version. See `src/linux_https_server.cpp` for a runnable
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

## File map

```
include/uchttp.h          Core library (engine, routing, middleware, pool)
include/uchttp_security.h Auth + security-headers middleware, secure_compare
include/uchttp_app_middleware.h  Cache-Control / ETag / Accept-Encoding / X-Request-ID
include/uchttp_tls.h      TLS abstraction (TlsSocket / TlsAcceptor)
include/openssl_tls.h     OpenSSL backend (host)
include/wolfssl_tls.h     wolfSSL backend (embedded)
src/linux_socket.h        POSIX LinuxSocket + LinuxAcceptor
src/linux_server.cpp      Example HTTP server on localhost
src/linux_https_server.cpp Example HTTPS server (OpenSSL)
tests/mock_socket.h       In-memory socket for unit tests
tests/test_uchttp.cpp     109 gtest unit tests
CMakeLists.txt            Build system
third_party/llhttp/       llhttp v9.4.3
```
