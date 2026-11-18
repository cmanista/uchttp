<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Middleware (Chain of Responsibility)

[← Usage index](README.md)

Register global middleware with `server.use(...)`. A middleware is a plain
function pointer. There is no `std::function` and no heap:

```cpp
void my_mw(const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain);
```

Each middleware may inspect or modify the request and response, then call
`chain.next(req, res)` to pass control to the next middleware and, finally, the
route handler. Because `next()` is called inside the middleware, code placed
after the call runs on the way back out (the "onion" model). This suits timing,
logging and response-header injection (CORS, security headers).

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

Middleware run in registration order. Two registered as `A` then `B` wrap the
handler as `A → B → handler → B → A`. The table is a fixed-size array sized by
`UCHTTP_MAX_MIDDLEWARE` (default 8). `server.use()` returns `false` when it is
full.

## What the chain covers

| Request | Middleware chain runs? |
|---|---|
| Normal route | Yes |
| WebSocket upgrade | Yes. A middleware can reject the upgrade (for example `401`). |
| Streaming route (`post_stream`, `put_stream`) | Yes, once, right after the headers and before the first body chunk. A middleware that does not call `next()` refuses the upload: the handler is not called and the connection is closed after the middleware's reply. |
| `404`, `405`, `501` and protocol errors (`400`, `413`, `431`) | No. They are produced by the engine. |

Because of the last row, headers added by middleware such as `security_headers`
are not present on `404` / `405` replies.

For streaming routes, code after `chain.next()` runs before the body is streamed,
so a middleware that reads the response body (such as `etag`) sees it empty. See
[Streaming body / OTA](iot.md#streaming-body--ota-firmware-upload).

## Bundled application middleware

`#include "uchttp_app_middleware.h"`. Optional, opt-in middleware for headers
that belong to the application layer:

```cpp
server.use(uchttp::app::request_id);          // echo/generate X-Request-ID
uchttp::app::set_cache_control("public, max-age=60");
server.use(uchttp::app::cache_control);       // Cache-Control on every response
server.use(uchttp::app::etag);                // ETag + If-None-Match -> 304
```

| Middleware | Header(s) | Behaviour |
|---|---|---|
| `cache_control` | `Cache-Control` | Injects the configured value (`set_cache_control`, default `no-cache`) |
| `etag` | `ETag`, `If-None-Match` | FNV-1a of the body. A matching `If-None-Match` gives `304 Not Modified` with an empty body. |
| `request_id` | `X-Request-ID` | Echoes an incoming id or generates one for log correlation |
| `client_accepts_encoding()` | `Accept-Encoding` | Helper to negotiate compression in a handler |

These middleware read request headers with `get_header`, which is case-sensitive.
A client that sends `if-none-match` or `x-request-id` in lower case is not
matched. Header names in the examples use the usual capitalisation.

Auth and security-header middleware live in `uchttp_security.h`. See
[Security](security.md).
