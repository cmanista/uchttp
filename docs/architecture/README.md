<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Architecture

uchttp is a C++23 HTTP/1.1 server for small targets. Everything that depends on
the platform (sockets, TLS library, SHA-1, session memory) is a template
parameter that satisfies a C++ concept, so the engine contains no virtual
functions and no heap allocation.

[← Documentation index](../README.md)

Contents:

- [Layers](#layers)
- [Request lifecycle](#request-lifecycle)
- [Request dispatch](#request-dispatch)
- [WebSocket upgrade](#websocket-upgrade)
- [TLS](#tls)
- [Memory model](#memory-model)
- [Concurrency model](#concurrency-model)
- [Compile-time configuration](#compile-time-configuration)
- [Source layout](#source-layout)
- [Class diagrams](#class-diagrams)

## Layers

```mermaid
flowchart TB
    app["<b>User application</b><br/>server.get / post / use / websocket<br/>server.run(acceptor)"]

    server["<b>HttpServer</b> (Socket, MaxRoutes, RxSize, TxSize,<br/>MaxMiddleware, MaxConns, WsTraits)<br/><br/>RouteTable (MaxRoutes): fixed array, linear search,<br/>literal routes before {name} patterns<br/>MiddlewareFn array (MaxMiddleware): function pointers<br/>ConnectionPool (MaxConns): pre-allocated slots"]

    acceptor["<b>Acceptor</b> (AcceptorConcept)<br/>LinuxAcceptor, ZephyrAcceptor,<br/>TlsAcceptor, user-supplied"]

    subgraph conn ["HttpConnection (Socket, RxSize, TxSize), one per pool slot"]
        direction TB
        rx["RX buffer [RxSize]<br/>stack / static"]
        parser["llhttp parser<br/>incremental, no allocation"]
        state["ParseState + HttpRequest<br/>string_views into the RX buffer"]
        chain["MiddlewareChain<br/>runs the middleware, then the handler"]
        handler["Route handler<br/>function pointer"]
        resp["HttpResponse<br/>formats status line, headers, body"]
        tx["TX buffer [TxSize]<br/>stack / static"]
        rx -- "raw bytes" --> parser
        parser -- "callbacks fill" --> state
        state --> chain
        chain --> handler
        handler --> resp
        resp --> tx
    end

    socket["<b>Socket</b> (SocketConcept)<br/>read / write / close<br/>LinuxSocket, ZephyrSocket, TlsSocket, MockSocket"]

    app -- "add_route / use / run / poll" --> server
    server -- "accept()" --> acceptor
    server -- "acquire(socket), process(routes, middleware), release(slot)" --> conn
    acceptor -. "produces" .-> socket
    conn -- "read() / write()" --> socket
```

| Layer | Responsibility | Satisfies / owns |
|---|---|---|
| `HttpServer` | Registers routes and middleware, runs the accept loop, owns the pool. | `RouteTable`, `ConnectionPool`, middleware array, WebSocket allocator |
| Acceptor | Produces one `Socket` per client. Applies the idle read timeout. | `AcceptorConcept` (`accept()`, `is_running()`) |
| `ConnectionPool` | Fixed array of `HttpConnection` slots. A full pool refuses the client. | `HttpConnection` (N slots) |
| `HttpConnection` | Keep-alive loop: read, parse, dispatch, respond. Owns the RX/TX buffers and the parser. | `ParseState`, `HttpResponse`, `llhttp_t` |
| Socket | Byte transport. The only part that talks to the network stack. | `SocketConcept` (`read`, `write`, `close`) |

Design rules the code follows:

- **No virtual functions.** Platform parts are template parameters checked by
  concepts (`SocketConcept`, `AcceptorConcept`, `TlsSessionConcept`,
  `ws::Sha1Concept`, `ws::AllocatorConcept`). Handlers and middleware are plain
  function pointers.
- **No heap.** Buffers, route tables, middleware tables, connection slots and
  WebSocket sessions are fixed-size arrays sized at compile time.
- **Zero-copy.** `HttpRequest` holds `string_view`s into the RX buffer. They stay
  valid until the next request is read on the same connection.
- **Parser.** [llhttp](https://github.com/nodejs/llhttp) v9.4.3 is vendored in
  `third_party/` (see
  [UPSTREAM.md](../../third_party/llhttp/UPSTREAM.md) for its origin).

## Request lifecycle

One client connection from `accept()` to close. The inner loop repeats for every
request on a keep-alive connection.

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant S as HttpServer
    participant A as Acceptor
    participant P as ConnectionPool
    participant H as HttpConnection
    participant L as llhttp
    participant R as RouteTable
    participant M as MiddlewareChain
    participant F as Route handler

    S->>A: accept()
    A-->>S: Socket
    S->>P: acquire(socket)
    alt pool exhausted
        P-->>S: nullptr
        Note over S: socket is dropped (closed by its destructor)
    else slot available
        P-->>S: HttpConnection*
        S->>H: process(routes, middleware, count, ws_alloc)
        loop keep-alive loop
            C->>H: request bytes (socket.read into RX buffer)
            H->>L: llhttp_execute(RX buffer)
            L-->>H: callbacks fill ParseState, parser pauses at message end
            alt protocol error (431, 413, 400, 501)
                H-->>C: error response, then close
            else complete request
                H->>R: find(method, path, request)
                alt route found
                    H->>M: run(request, response)
                    M->>F: next(...) ends in the handler
                    F-->>M: fills HttpResponse
                    M-->>H: unwinds (middleware code after next() runs)
                    H-->>C: response (socket.write from TX buffer)
                else known path, other method
                    H-->>C: 405 Method Not Allowed
                else unknown path
                    H-->>C: 404 Not Found
                end
            end
            Note over H: Connection: close or HTTP/1.0 ends the loop.<br/>Pipelined bytes are moved to the front of the RX buffer.
        end
        S->>P: release(slot) closes the socket
    end
```

Keep-alive details:

- The parser is paused at every message boundary, so one `read()` that carries
  the tail of one request and the head of the next never mixes request state.
- `request.keep_alive` comes from `llhttp_should_keep_alive()`. It accounts for
  the HTTP version and the `Connection` header.
- If a request does not fit in the RX buffer, the engine answers `400` and closes
  the connection (see [limits](#compile-time-configuration)).

## Request dispatch

Decision order inside `HttpConnection::process` once a request has been read.

```mermaid
flowchart TD
    start(["request read"]) --> rej{"streaming route refused<br/>by the middleware?"}
    rej -- yes --> rrej["middleware's own response, for example 401<br/>upload body is not read<br/>close"]
    rej -- no --> e431{"header block<br/>too large?"}
    e431 -- yes --> r431["431 Request Header Fields Too Large<br/>close"]
    e431 -- no --> e413{"body too large?"}
    e413 -- yes --> r413["413 Content Too Large<br/>close"]
    e413 -- no --> e400{"parse error, or request<br/>larger than RX buffer?"}
    e400 -- yes --> r400["400 Bad Request<br/>close"]
    e400 -- no --> done{"message<br/>complete?"}
    done -- "no (EOF)" --> endc(["close, nothing to send"])
    done -- yes --> stream{"streaming route<br/>matched during parsing?"}
    stream -- yes --> shandler["middleware chain ran before the first chunk<br/>handler already received the body chunks<br/>engine sends 200 OK unless the handler took over"]
    stream -- no --> method{"method supported?"}
    method -- no --> r501["501 Not Implemented"]
    method -- yes --> find{"RouteTable.find"}
    find -- "WebSocket route" --> ws["middleware chain ending in upgrade_handler<br/>status 101: run the WebSocket session<br/>otherwise plain HTTP reply"]
    find -- "normal route" --> normal["middleware chain, then handler<br/>send buffered response"]
    find -- "no match" --> path{"path registered<br/>for another method?"}
    path -- yes --> r405["405 Method Not Allowed"]
    path -- no --> r404["404 Not Found"]
```

Notes:

- `400`, `413`, `431`, `404`, `405` and `501` are produced by the engine. The
  middleware chain does not run for them, so headers added by middleware (for
  example `security_headers`) are absent from these replies.
- **Streaming routes run the middleware chain before the first body chunk.** The
  streaming handler is called while the body is still being parsed, so the engine
  runs the chain as soon as the headers are complete, with a no-op as its final
  target. If a middleware does not call `next()` (for example `api_key_auth`
  answering `401`), the parser is stopped, no byte of the body reaches the
  handler, and the engine sends the middleware's response and closes the
  connection. See
  [Streaming body / OTA](../usage/iot.md#streaming-body--ota-firmware-upload).
- Literal routes are matched before `{name}` patterns. `path_exists` is what
  separates `405` from `404`.

## WebSocket upgrade

A WebSocket endpoint is an ordinary route. The upgrade request goes through the
middleware chain, so authentication and security headers apply.

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant H as HttpConnection
    participant M as MiddlewareChain
    participant U as ws::upgrade_handler (Sha1)
    participant G as Handlers.accept
    participant AL as ws Allocator
    participant SV as ws::serve / Session
    participant CB as Handlers callbacks

    C->>H: GET /ws, Upgrade: websocket, Sec-WebSocket-Key
    H->>M: run(request, response)
    M->>U: next(...)
    U->>U: validate_handshake (426 / 400 on failure)
    U-->>M: 101 + Sec-WebSocket-Accept = base64(SHA-1(key + GUID))
    M-->>H: response (middleware may reject, for example 401)
    alt status is not 101
        H-->>C: plain HTTP reply
    else status 101
        H->>G: accept(request) if set
        G-->>H: false means 403
        H->>AL: allocate(UCHTTP_WS_BUFFER_SIZE)
        AL-->>H: nullptr means 503
        H-->>C: 101 Switching Protocols
        H->>SV: serve(transport, handlers, request, rx, tx, leftover)
        SV->>CB: on_open
        loop until closed
            C->>SV: frame
            SV->>CB: on_message (fragments reassembled, text is valid UTF-8)
            SV->>CB: on_idle on read timeout
            SV-->>C: Ping after UCHTTP_WS_PING_INTERVAL_MS of silence
        end
        SV->>CB: on_close (exactly once)
        H->>AL: deallocate(buffer)
        Note over H: socket is closed afterwards
    end
```

The WebSocket core (`uchttp_ws.h`) does not know about sockets, the OS or a
crypto library. `HttpConnection` binds a `ws::Transport` (function pointers and a
context pointer) to the concrete socket. SHA-1 and the session buffer come from
`ws::Traits<Sha1, Allocator>`, the last template parameter of `HttpServer`.

## TLS

TLS is a layer around the socket, so the engine is unchanged. On hosts, a TLS
session wraps an accepted file descriptor. On Zephyr, the secure-socket layer of
the network stack encrypts inside `recv()` / `send()`.

```mermaid
flowchart LR
    subgraph host ["Host (OpenSSL / wolfSSL)"]
        direction TB
        raw["LinuxAcceptor<br/>produces LinuxSocket"]
        tlsacc["TlsAcceptor (Context, RawAcceptor)<br/>Context.make_session(raw socket)"]
        tlssock["TlsSocket (Session)<br/>satisfies SocketConcept<br/>handshake on first read/write"]
        sess["OpenSslSession / WolfSslSession<br/>satisfies TlsSessionConcept"]
        raw --> tlsacc --> tlssock
        tlssock --- sess
    end

    subgraph zephyr ["Zephyr (Mbed TLS / PSA Crypto)"]
        direction TB
        ztls["ZephyrTlsAcceptor<br/>listening socket with IPPROTO_TLS_1_2 and sec_tag"]
        zsock["ZephyrSocket<br/>plaintext API, TLS inside the kernel socket"]
        zcred["ZephyrTlsCredentials<br/>certificate, key, CA, PSK"]
        ztls --> zsock
        zcred -. "sec_tag" .-> ztls
    end

    engine["HttpServer (Socket)<br/>identical engine in both cases"]
    tlssock --> engine
    zsock --> engine
```

Both certificates and pre-shared keys are supported. TLS 1.2 is the minimum
version. `TlsSocket::set_read_timeout` forwards to the session, so the idle
timeout and the WebSocket idle tick work over TLS as well.

## Memory model

| What | Where | Size |
|---|---|---|
| RX / TX buffers | member arrays of each `HttpConnection` | `RxSize` + `TxSize` per slot |
| Connection slots | array in `ConnectionPool`, inside `HttpServer` | `MaxConns` slots |
| Route table | array in `RouteTable` | `MaxRoutes` entries |
| Middleware table | array of function pointers in `HttpServer` | `MaxMiddleware` entries |
| Request headers / path params | arrays in `HttpRequest` | `UCHTTP_MAX_HEADERS`, `UCHTTP_MAX_PATH_PARAMS` |
| Response scratch | `char[32]` in `HttpResponse` (for run-time header values such as `Sec-WebSocket-Accept`) | 32 bytes |
| WebSocket session buffer | one block from the `ws::Allocator` per session (`StaticAllocator` by default) | `UCHTTP_WS_BUFFER_SIZE` |

The server object itself decides where all of this lives. Declare it `static` (or
as a global) for large configurations so it does not land on a small task stack.
`HttpResponse::add_header` keeps views, not copies: header values must outlive
the response (string literals, statics, or `HttpResponse::scratch()`).

## Concurrency model

`HttpServer::run` serves one connection at a time: accept, acquire a slot,
process until the connection ends, release. To serve N clients at once, run N
`HttpServer` instances (threads or RTOS tasks) that share one acceptor. This also
applies to WebSocket, because a session keeps its connection until it closes. The
samples `samples/linux/ws_server` (4 worker threads) and
`samples/zephyr/ws_server` (`CONFIG_APP_SERVER_THREADS`) do this.

`HttpServer::poll` handles at most one connection per call, for cooperative
schedulers. `stop()` ends the accept loop and `shutdown()` closes every active
slot.

## Compile-time configuration

All limits are macros with `#ifndef` defaults. Set them with `-D` on the compiler
command line or, on Zephyr, with the Kconfig symbols (`CONFIG_UCHTTP_*`).

| Macro | Default | Kconfig | Meaning |
|---|---|---|---|
| `UCHTTP_RX_BUFFER_SIZE` | 1024 | yes | RX buffer per connection slot |
| `UCHTTP_TX_BUFFER_SIZE` | 1024 | yes | TX buffer per connection slot |
| `UCHTTP_MAX_ROUTES` | 16 | yes | Entries in the route table |
| `UCHTTP_MAX_MIDDLEWARE` | 8 | no | Entries in the middleware table |
| `UCHTTP_MAX_CONNECTIONS` | 4 | yes | Slots in the connection pool |
| `UCHTTP_MAX_HEADERS` | 16 | no | Request / response headers kept per message |
| `UCHTTP_MAX_PATH_PARAMS` | 8 | no | `{name}` segments per route |
| `UCHTTP_MAX_HEADER_BYTES` | 2048 | yes | Header block limit, answered with `431` |
| `UCHTTP_MAX_BODY_BYTES` | 4096 | yes | Body limit for normal routes, answered with `413` |
| `UCHTTP_IDLE_TIMEOUT_MS` | 5000 | yes | Idle read timeout, `0` disables it |
| `UCHTTP_WS_MAX_MESSAGE_SIZE` | 1024 | yes | Largest message after reassembly (close `1009`) |
| `UCHTTP_WS_MAX_FRAME_SIZE` | = max message size | yes | Largest single frame, checked from the frame header |
| `UCHTTP_WS_CONTROL_RESERVE` | 144 | no | Space kept free for control frames between fragments |
| `UCHTTP_WS_BUFFER_SIZE` | max message + reserve | no | Session buffer, one allocator block |
| `UCHTTP_WS_MAX_SESSIONS` | 1 | yes | Blocks in the default `StaticAllocator` |
| `UCHTTP_WS_IDLE_INTERVAL_MS` | 1000 | yes | Read timeout used as the session tick (`on_idle`) |
| `UCHTTP_WS_PING_INTERVAL_MS` | 15000 | yes | Silence before a Ping; `0` disables the keep-alive |

The limits interact with the RX buffer. A request is read into the RX buffer in
one piece, so the effective cap is the smaller of the buffer and the macro.
With the default 1 KiB RX buffer, a request larger than the buffer is answered
with `400 Bad Request` before `UCHTTP_MAX_HEADER_BYTES` or
`UCHTTP_MAX_BODY_BYTES` can trigger `431` / `413`. To get those status codes,
make `UCHTTP_RX_BUFFER_SIZE` larger than the limits (the unit tests use an
8 KiB buffer for this). Streaming routes are not limited by
`UCHTTP_MAX_BODY_BYTES`.

## Source layout

```
include/uchttp.h                 Core: concepts, HttpRequest/Response, RouteTable, middleware chain,
                                 HttpConnection, ConnectionPool, HttpServer
include/uchttp_security.h        Auth and security-headers middleware, secure_compare
include/uchttp_app_middleware.h  Cache-Control, ETag, Accept-Encoding, X-Request-ID
include/uchttp_ws.h              WebSocket core: codec, Session, Handlers, Sha1/Allocator concepts
include/uchttp_tls.h             TlsSessionConcept, TlsSocket, TlsAcceptor
include/openssl_tls.h            OpenSSL backend (host)
include/wolfssl_tls.h            wolfSSL backend (embedded)
include/uchttp_version.h.in      Template for the generated uchttp_version.h
src/uchttp.cpp                   Non-template engine code (parser callbacks, request/response helpers)
src/uchttp_security.cpp          Security middleware
src/uchttp_app_middleware.cpp    Application middleware
src/uchttp_ws.cpp                WebSocket codec and SHA-1
src/uchttp_ws_session.cpp        WebSocket session loop
src/uchttp_ws_handshake.cpp      WebSocket handshake validation
src/openssl_tls.cpp              OpenSSL backend
src/wolfssl_tls.cpp              wolfSSL backend
src/port/linux/                  LinuxSocket, LinuxAcceptor
src/port/zephyr/                 ZephyrSocket, ZephyrAcceptor, ZephyrTlsAcceptor, ZephyrTlsCredentials,
                                 ZephyrSlabAllocator, ZephyrPsaSha1
zephyr/                          Zephyr module (module.yml, CMakeLists.txt, Kconfig)
samples/common/                  Demo page and telemetry shared by the WebSocket samples
samples/linux/                   http_server, https_server, ws_server
samples/zephyr/                  http_server, https_server, ws_server
tests/                           MockSocket and the gtest suites (see ../testing.md)
third_party/llhttp/              llhttp v9.4.3 (symlink to the versioned directory)
third_party/llhttp.cmake         Single definition of the llhttp sources, used by both builds
scripts/release.sh               Release helper (see ../RELEASING.md)
CMakeLists.txt                   Host build
```

## Class diagrams

Class diagrams per module, verified against the headers:

| Diagram | Contents |
|---|---|
| [01_core_types.md](class_diagrams/01_core_types.md) | Concepts, `HttpRequest`, `HttpResponse`, `QueryString`, `Route` |
| [02_engine.md](class_diagrams/02_engine.md) | `HttpServer`, `ConnectionPool`, `HttpConnection`, `ParseState`, `RouteTable` |
| [03_middleware.md](class_diagrams/03_middleware.md) | `MiddlewareChain`, `uchttp::security`, `uchttp::app` |
| [04_websocket.md](class_diagrams/04_websocket.md) | `Session`, `Handlers`, `Transport`, SHA-1 and allocator concepts |
| [05_tls.md](class_diagrams/05_tls.md) | `TlsSocket`, `TlsAcceptor`, OpenSSL and wolfSSL backends |
| [06_platform_ports.md](class_diagrams/06_platform_ports.md) | Linux, Zephyr and test doubles |

The [index](class_diagrams/README.md) also has a module dependency overview.
