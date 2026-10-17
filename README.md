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
│   │   │   ├─ method          │  enum (GET/POST/PUT/…)                │
│   │   │   ├─ path            │  string_view → RX buf                │
│   │   │   ├─ body            │  string_view → RX buf                │
│   │   │   └─ headers[]       │  string_view pairs → RX buf          │
│   │   └─ message_complete    │                                       │
│   └──────────┬───────────────┘                                       │
│              │ RouteTable::find(method, path)                        │
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
| **Portability** | Core library (`uchttp.h`) has no OS or POSIX dependencies |

## Quick start (Linux)

```bash
cmake -B build && cmake --build build
./build/uchttp_server          # listens on http://localhost:8080

curl http://localhost:8080/api/hello
curl -X POST http://localhost:8080/api/echo -d 'hello'
curl -X PUT  http://localhost:8080/api/item -d '{"id":1}'
```

## Running tests

```bash
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./build/uchttp_tests
```

## Porting to an RTOS / bare-metal target

1. Implement `read()`, `write()`, `close()`, and `is_valid()` for your TCP stack (lwIP, Zephyr BSD sockets, etc.).
2. Implement an acceptor with `accept()` and `is_running()`.
3. Instantiate `HttpServer<YourSocket>` and call `server.run(acceptor)` from your task/thread.

No other changes are needed — the core library has no OS dependencies.

## File map

```
include/uchttp.h        Core library (header-only)
src/linux_socket.h      POSIX LinuxSocket + LinuxAcceptor
src/linux_server.cpp    Example server on localhost
tests/mock_socket.h     In-memory socket for unit tests
tests/test_uchttp.cpp   27 gtest unit tests
CMakeLists.txt          Build system
third_party/llhttp/     llhttp v9.4.3
```
