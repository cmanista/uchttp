<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# WebSocket (RFC 6455)

[← Usage index](README.md)

`#include "uchttp.h"` (it pulls in `uchttp_ws.h`). A WebSocket endpoint is a
route. The upgrade request goes through the middleware chain like any other
request (auth, security headers), and only a `101` reply hands the socket over to
the WebSocket session. The sequence is shown in
[Architecture](../architecture/README.md#websocket-upgrade).

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

The `Handlers` object is stored by pointer. Keep it alive (static storage).

## Callbacks

The API is callback based: plain function pointers, no `std::function`, no
virtuals. Any member may be left null.

| Member | Called | Notes |
|---|---|---|
| `on_open(Session&, const HttpRequest&)` | After the `101` reply was sent | The request views are valid during this call only |
| `on_message(Session&, Opcode, span)` | For each complete message | Fragments are reassembled. Text is validated as UTF-8. `data` is valid during the call only. |
| `on_close(Session&, uint16_t code)` | Exactly once | The peer's close code, or `1006` if the connection was lost |
| `on_idle(Session&)` | When the read timeout expires with nothing received | Every `idle_interval_ms`. Periodic server push without an extra thread. |
| `accept(const HttpRequest&)` | Before the `101` is sent, after the middleware ran | Return `false` to refuse with `403` (for example an `Origin` check) |
| `subprotocol` | During the handshake | Selected if the client offers it in `Sec-WebSocket-Protocol` |
| `idle_interval_ms`, `ping_interval_ms` | | Per-endpoint overrides of the compile-time defaults |
| `user` | | Context returned by `Session::user()` |

The session sends with `send_text()`, `send_binary()`, `ping()` and `close()`.
Call them from the callbacks: the engine is single threaded and the send methods
never allocate.

## What the engine does

| Area | Behaviour |
|---|---|
| Handshake | `Upgrade` / `Connection` / `Sec-WebSocket-Version: 13` / `Sec-WebSocket-Key` validated (names case-insensitive). `426` with hints or `400` otherwise. `Sec-WebSocket-Accept` = base64(SHA-1(key + GUID)). |
| Framing | Masked client frames required, RSV bits and reserved opcodes rejected, minimal length encoding enforced, control frames at most 125 B and unfragmented, Ping answered with Pong, close codes validated. |
| Limits | `UCHTTP_WS_MAX_FRAME_SIZE` is checked from the frame header alone (a 4 GiB frame is refused without reading it). `UCHTTP_WS_MAX_MESSAGE_SIZE` is checked after reassembly. Both close with `1009`. |
| Keep-alive | Ping after `UCHTTP_WS_PING_INTERVAL_MS` of silence. The peer is dropped after the same time again (default 15 s / 30 s). |
| Memory | One session buffer per connection from the allocator. Frames are parsed and reassembled in place. Small outgoing frames are coalesced into the TX buffer (one write). |

An exhausted allocator refuses the upgrade with `503`.

## Platform hooks

Both are compile-time plug-ins bundled in `ws::Traits<Sha1, Allocator>` and given
to `HttpServer` as its last template parameter (default `ws::DefaultTraits`):

| Concept | Default | Zephyr port |
|---|---|---|
| `ws::Sha1Concept` (`start/update/finish`) | `ws::SoftSha1` (portable) | `ZephyrPsaSha1`: PSA Crypto, hardware accelerated where the PSA driver supports SHA-1 |
| `ws::AllocatorConcept` (`allocate/deallocate`) | `ws::StaticAllocator<N, Size>` (static storage) | `ZephyrSlabAllocator<N, Size>`: `k_mem_slab` |

```cpp
using WsTraits = uchttp::ws::Traits<MySha1, MyAllocator>;
uchttp::HttpServer<MySocket, UCHTTP_MAX_ROUTES, UCHTTP_RX_BUFFER_SIZE,
                   UCHTTP_TX_BUFFER_SIZE, UCHTTP_MAX_MIDDLEWARE,
                   UCHTTP_MAX_CONNECTIONS, WsTraits> server;
```

## Concurrent clients

`HttpServer` serves one connection at a time and a WebSocket keeps its
connection. To serve N concurrent clients, run N `HttpServer` instances, one per
thread or task, all accepting on the same acceptor. Each instance owns its
allocator, so the default `StaticAllocator` with `UCHTTP_WS_MAX_SESSIONS = 1`
(one session buffer per instance) is enough. A client that connects while all
instances are busy waits in the listen backlog. See
`samples/linux/ws_server/main.cpp`, which starts 4 worker threads, each with its
own `HttpServer` and its own `Handlers` object (`user` points at that thread's
state).

## Host demo

```bash
./build/uchttp_ws_server                  # 4 worker threads, port 8080
# browser: http://localhost:8080/  (JS client)   or:
wscat -c ws://localhost:8080/ws           # echo; "close" closes; telemetry every 1 s
```

For Zephyr, see [WebSocket on Zephyr](zephyr.md#websocket-on-zephyr).
