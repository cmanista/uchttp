<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Reliability and resource management

[← Usage index](README.md)

On an MCU the socket and descriptor pool is tiny, so uchttp never allocates a
connection on demand.

- **Connection pool.** `HttpServer` owns a `ConnectionPool<Socket, N, ...>`: a
  fixed, pre-allocated array of `N` connection slots (`UCHTTP_MAX_CONNECTIONS`,
  default 4), each with its own RX/TX buffers and parser state. `run()` acquires
  an idle slot per client and releases it when done. When the pool is exhausted,
  the new connection is dropped rather than growing the heap.
- **Timeouts.** The keep-alive loop plus the idle read timeout
  (`UCHTTP_IDLE_TIMEOUT_MS`, default 5000 ms) reclaim slots held by dead or
  stalled peers. See [Security](security.md#dos-and-slowloris-protection).
- **Graceful shutdown.** `server.stop()` ends the accept loop.
  `server.shutdown()` (or `ConnectionPool::close_all()`) closes every active slot
  cleanly, for example on a network-stack reset or reboot.

```cpp
uchttp::HttpServer<MySocket> server;   // pool sized by UCHTTP_MAX_CONNECTIONS
server.run(acceptor);                  // acquire/serve/release per client
// from a signal handler / supervisor task:
server.stop();                         // leave run()
server.shutdown();                     // close all active connections
```

`run()` also calls `close_all()` when it leaves the loop.

## Sizing

Every pool slot carries its own RX and TX buffers, so
`UCHTTP_MAX_CONNECTIONS * (UCHTTP_RX_BUFFER_SIZE + UCHTTP_TX_BUFFER_SIZE)` bytes
are reserved up front, plus the parser state of each slot.

`run()` accepts, processes and releases one client at a time, so a single
`HttpServer` has one busy slot at any moment. Slots beyond the first are spare
capacity that costs RAM. If you serve clients with one instance per thread (see
[WebSocket](websocket.md#concurrent-clients)), each instance has its own pool.
On a small target, reduce `UCHTTP_MAX_CONNECTIONS` to what you use.

A long-running handler (an SSE loop, a slow flash write) blocks the instance
while it runs. Use one instance per concurrent long-lived client, and rely on the
idle timeout for stalled peers.

## Introspection

| Call | Meaning |
|---|---|
| `server.active_connections()` | Slots currently in use |
| `HttpServer<...>::max_connections()` | Pool capacity (`UCHTTP_MAX_CONNECTIONS`) |
| `server.poll(acceptor)` | Handle at most one connection, then return (cooperative loops) |
