<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Usage

How to build an application on top of uchttp. For the build and a first request,
see the [Quick start](../../README.md#quick-start-linux). For how the pieces fit
together, see [Architecture](../architecture/README.md).

[← Documentation index](../README.md)

| Page | Topics |
|---|---|
| [Routing and request parsing](routing.md) | Path parameters, query string, HTTP methods, handlers |
| [Middleware](middleware.md) | Chain of Responsibility, bundled application middleware (Cache-Control, ETag, X-Request-ID) |
| [Protocol headers](protocol.md) | Headers owned by the engine, keep-alive, protocol error responses |
| [Security](security.md) | DoS / Slowloris limits, API key and bearer auth, security headers, TLS / HTTPS on hosts |
| [IoT edge capabilities](iot.md) | Streaming body / OTA upload, Server-Sent Events |
| [WebSocket](websocket.md) | RFC 6455 endpoints, callbacks, platform hooks, concurrent clients |
| [Reliability](reliability.md) | Connection pool, timeouts, graceful shutdown, sizing |
| [Porting](porting.md) | Socket and acceptor contracts for an RTOS or bare-metal target |
| [Zephyr](zephyr.md) | Module build, Kconfig, west, HTTPS / TLS, WebSocket, board notes |

## A minimal server

The pieces from the pages above combine like this on Linux (see
`samples/linux/http_server/main.cpp` for the full program with all routes):

```cpp
#include "uchttp.h"
#include "linux_socket.h"

static void hello(const uchttp::HttpRequest&, uchttp::HttpResponse& res) {
    res.set_status(200);
    res.set_content_type("text/plain");
    res.set_body("hello");
}

int main() {
    uchttp::LinuxAcceptor acceptor(8080);
    static uchttp::HttpServer<uchttp::LinuxSocket> server;   // static: keeps the buffers off the stack

    server.get("/api/hello", hello);
    if (!acceptor.start()) return 1;
    server.run(acceptor);
}
```
