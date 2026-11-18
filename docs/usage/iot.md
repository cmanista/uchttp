<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# IoT edge capabilities

[← Usage index](README.md)

## Streaming body / OTA firmware upload

A streaming route hands the request body to the handler in chunks as it arrives.
The engine never buffers the whole payload, so a multi-megabyte firmware image
flows straight to the flash writer:

```cpp
void ota(const HttpRequest& req, HttpResponse& res,
         std::span<const uint8_t> chunk, size_t offset, bool last) {
    flash_write(partition, offset, chunk);      // no RAM buffering
    if (last) { res.set_status(201); res.set_body("flashed"); }
}
server.post_stream("/api/ota", ota);            // also put_stream(...)
```

- Chunks are views into the RX buffer (zero copy). `offset` is the number of
  bytes already delivered, and `last` is true on the final call.
- Both `Content-Length` and `Transfer-Encoding: chunked` uploads are supported.
- The body-size cap (`UCHTTP_MAX_BODY_BYTES`) does **not** apply to streaming
  routes.
- If the handler does not take over the response, the engine sends `200 OK`.

**Streaming routes bypass the middleware chain.** The handler runs while the body
is still being parsed, before dispatch, so `api_key_auth`, `bearer_auth` and
other middleware do not protect it. Authenticate inside the handler. The request
headers are already available in `req` for every chunk, including the first:

```cpp
void ota(const HttpRequest& req, HttpResponse& res,
         std::span<const uint8_t> chunk, size_t offset, bool last) {
    if (!uchttp::security::secure_compare(req.get_header("X-API-Key"), "s3cr3t")) {
        res.set_status(401);
        return;                                  // do not write to flash
    }
    flash_write(partition, offset, chunk);
    if (last) { res.set_status(201); res.set_body("flashed"); }
}
```

## Server-Sent Events (SSE)

The handler keeps the connection open and pushes `text/event-stream` frames
straight to the socket:

```cpp
server.get("/api/events", [](const HttpRequest& req, HttpResponse& res) {
    res.begin_sse();                            // 200 + text/event-stream
    res.send_event("23.5", "temperature", "42"); // data / event / id
});
```

`begin_sse()` emits the status line and headers (`200`, `Content-Type:
text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive`).
`send_event(data, event, id)` writes one frame. `event` and `id` are optional,
and multi-line data is split into several `data:` lines. Both return `false` on a
write error or when no stream is bound.

Once the handler streams its own bytes it "hijacks" the response, so the engine
does not append a buffered reply. SSE works unchanged over TLS (`TlsSocket`).
Normal routes pass through the middleware chain, so authentication middleware
applies to SSE endpoints.

Limits to know:

- `begin_sse()` writes a fixed header block straight to the socket. Headers that
  middleware adds to the response (for example `security_headers`, CORS) are
  never sent for an SSE stream.
- `send_event()` builds each frame in a 512-byte stack buffer. A frame larger than
  that is truncated.

`HttpServer` serves one connection at a time, so an SSE handler that loops until
the client disconnects occupies the server instance. Use one instance per
concurrent stream. See [Reliability](reliability.md).
