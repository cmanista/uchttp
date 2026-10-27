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
tests/test_uchttp.cpp   61 gtest unit tests
CMakeLists.txt          Build system
third_party/llhttp/     llhttp v9.4.3
```
