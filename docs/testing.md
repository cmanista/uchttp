<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Testing

[← Documentation index](README.md)

The unit tests use [GoogleTest](https://github.com/google/googletest). They run
on the host without a network: sockets are replaced by in-memory test doubles,
which is possible because the engine takes its socket as a template parameter.

## Running the tests

```bash
cmake -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Each `TEST` appears as its own `ctest` entry (`gtest_discover_tests`). To run the
test binary directly or select tests:

```bash
./build/uchttp_tests                                  # everything
./build/uchttp_tests --gtest_filter='WsSessionTest.*' # one suite
./build/uchttp_tests --gtest_list_tests               # list all tests
```

CMake uses an installed GTest (`find_package(GTest)`). If none is found it
downloads googletest v1.14.0 with `FetchContent`, which needs network access
during configure.

Current state: 215 tests in 24 suites (109 in `tests/test_uchttp.cpp`, 106 in
`tests/test_uchttp_ws.cpp`), all passing.

### Troubleshooting

`gtest_discover_tests` runs the test binary at the end of the build to list its
tests. If the binary cannot start, the build itself fails. A common cause on
machines with conda is a `libstdc++` that is older than the one the compiler
used:

```
libstdc++.so.6: version `GLIBCXX_3.4.32' not found
```

Put the system library first and rebuild:

```bash
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu
cmake --build build
```

## Test doubles

| Type | Location | Use |
|---|---|---|
| `MockSocket` | `tests/mock_socket.h` | Replays a fixed request and records everything written (`tx_view()`, `tx_status_line()`, `tx_body()`). Satisfies `SocketConcept`. |
| `MockAcceptor` | `tests/mock_socket.h` | Hands out one `MockSocket` and then reports `is_running() == false`, so `HttpServer::run()` returns. |
| `RefMockSocket` | `tests/test_uchttp.cpp` | Like `MockSocket`, but writes to a string owned by the test. Needed because `HttpConnection` takes ownership of the socket and destroys it. |
| Scripted transport | `tests/test_uchttp_ws.cpp` | Feeds WebSocket frames to `ws::serve()` in chosen chunks, including partial reads, and captures the frames sent back. |

## Test suites

`tests/test_uchttp.cpp` (HTTP engine):

| Suite | Tests | Covers |
|---|---|---|
| `HttpResponseTest` | 11 | Status line, headers, body, buffer formatting |
| `HttpMethodTest` | 1 | Method mapping |
| `QueryStringTest` | 7 | Query iteration, `get`, `has`, empty values |
| `RouteTableTest` | 8 | Add / find, path-parameter patterns, literal-before-pattern |
| `HttpConnectionTest` | 22 | GET / POST / PUT / DELETE / PATCH / HEAD / OPTIONS dispatch, `404`, `405`, `400`, header parsing, query and path parameters |
| `CoreHeadersTest` | 12 | `Host`, `Content-Length`, chunked, HTTP version, keep-alive and `Connection: close`, pipelined requests, `501` |
| `MiddlewareChainTest`, `MiddlewareTest` | 1 + 7 | Order, short-circuit, response-header injection, empty and null slots |
| `AppMiddlewareTest` | 7 | `Cache-Control`, `ETag` / `304`, `X-Request-ID`, `Accept-Encoding` |
| `SecurityTest` | 14 | `secure_compare`, API key and bearer auth (including fail-closed), security headers, `431` and `413` |
| `EdgeStreamTest` | 3 | Streaming body with `Content-Length` and chunked, no body cap |
| `EdgeSseTest` | 4 | SSE headers, event framing, multi-line data, response hijack |
| `ConnectionPoolTest` | 4 | Acquire / release, exhaustion, `close_all`, serving through a slot |
| `HttpServerTest` | 6 | Run loop via `MockAcceptor`, method helpers, `use()` capacity, pool introspection |
| `TlsTest` | 2 | `TlsSocket` satisfies `SocketConcept` (compile-time) |

`tests/test_uchttp_ws.cpp` (WebSocket):

| Suite | Tests | Covers |
|---|---|---|
| `WsSha1`, `WsBase64`, `WsUtf8` | 3 + 2 + 6 | RFC test vectors, encoding, strict UTF-8 |
| `WsCodec` | 11 | Frame header parse / encode, masking, length forms |
| `WsAllocator` | 3 | `StaticAllocator` behaviour |
| `WsHandshake`, `WsHandshakeValidate` | 5 + 6 | `Sec-WebSocket-Accept`, upgrade validation (`426`, `400`) |
| `WsSessionTest` | 48 | Fragmentation, control frames, protocol errors and close codes, close handshake, keep-alive, size limits, partial reads |
| `WsEndToEnd` | 22 | Upgrade through `HttpConnection::process()` with middleware, allocator and SHA-1 plug-ins |

## Writing a test

The usual pattern is a route table, a socket that replays raw request bytes, and
an assertion on the bytes written back. This example is self-contained:

```cpp
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "uchttp.h"

using namespace uchttp;

// Replays `rx` and appends everything written to an external string.
class CaptureSocket {
public:
    CaptureSocket() = default;
    CaptureSocket(std::string_view rx, std::string& tx) : rx_(rx), tx_(&tx) {}

    int read(std::span<uint8_t> buf) noexcept {
        size_t n = std::min(rx_.size() - pos_, buf.size());
        std::memcpy(buf.data(), rx_.data() + pos_, n);
        pos_ += n;
        return static_cast<int>(n);               // 0 = EOF ends the connection
    }
    int write(std::span<const uint8_t> buf) noexcept {
        if (tx_) tx_->append(reinterpret_cast<const char*>(buf.data()), buf.size());
        return static_cast<int>(buf.size());
    }
    void close() noexcept { valid_ = false; }
    bool is_valid() const noexcept { return valid_; }

private:
    std::string  rx_;
    size_t       pos_ { 0 };
    std::string* tx_ { nullptr };
    bool         valid_ { true };
};

TEST(ExampleTest, PathParamIsPassedToHandler) {
    RouteTable<4> routes;
    routes.add(HttpMethod::GET, "/api/sensors/{id}",
               [](const HttpRequest& req, HttpResponse& res) {
                   res.set_status(200);
                   res.set_body(req.path_param("id"));   // echo the parameter
               });

    std::string tx;
    CaptureSocket sock("GET /api/sensors/42 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", tx);
    HttpConnection<CaptureSocket> conn(std::move(sock));
    conn.process(routes);

    EXPECT_TRUE(tx.starts_with("HTTP/1.1 200 OK"));
    EXPECT_TRUE(tx.ends_with("\r\n\r\n42"));
}
```

Add new tests to `tests/test_uchttp.cpp` (HTTP) or `tests/test_uchttp_ws.cpp`
(WebSocket). Both are linked into the single `uchttp_tests` executable.
Use `RouteTable<N>` with a small `N`, and pass a larger `RxSize` as a template
argument (`HttpConnection<Sock, 8192>`) when a test needs requests above the
default 1 KiB buffer, as the `431` / `413` tests do.

Tests that register middleware or configure `uchttp::security` / `uchttp::app`
change process-wide state. Reset it (for example `set_api_key({})`) at the end of
the test, or set it at the start of each test that depends on it.

## What the unit tests do not cover

- **TLS handshakes.** `TlsTest` only checks at compile time that `TlsSocket`
  models `SocketConcept`. The OpenSSL and wolfSSL backends and the Zephyr TLS port
  are exercised by running the samples.
- **Real sockets and timeouts.** `LinuxSocket`, `LinuxAcceptor` and the idle
  read timeout need a network and are not part of `uchttp_tests`.
- **Zephyr.** The module and the samples are built and run with `west`, not by the
  host test suite.

## Manual checks

Build and run the Linux sample, then exercise it:

```bash
./build/uchttp_server                                   # http://localhost:8080
curl http://localhost:8080/api/hello
curl -X POST http://localhost:8080/api/echo -d 'hello'
curl http://localhost:8080/api/sensors/42               # path parameter
curl 'http://localhost:8080/api/search?q=temp&limit=5'  # query string
curl -X OPTIONS http://localhost:8080/api/data -i       # CORS preflight
curl -N http://localhost:8080/api/events                # SSE stream
```

HTTPS with a throw-away certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key \
    -out server.crt -days 1 -subj '/CN=localhost'
./build/uchttp_https_server server.crt server.key
curl -k https://localhost:8443/api/hello
```

WebSocket (the demo page is served at `/`):

```bash
./build/uchttp_ws_server
wscat -c ws://localhost:8080/ws
```

Zephyr builds (see [Zephyr](usage/zephyr.md)):

```bash
west build -p always -b native_sim samples/zephyr/http_server \
    -- -DZEPHYR_EXTRA_MODULES=$(pwd)
```

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request to `master`. It
builds and runs `ctest` on Ubuntu 24.04 with GCC and Clang, each in Debug and
Release (four jobs). The release workflow repeats the build and the tests before
publishing a release; see [RELEASING.md](RELEASING.md). Zephyr builds are not part
of CI.
