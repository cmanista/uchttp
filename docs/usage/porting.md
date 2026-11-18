<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Porting to an RTOS or bare-metal target

[← Usage index](README.md)

1. Implement `read()`, `write()`, `close()`, and `is_valid()` for your TCP stack
   (lwIP, Zephyr BSD sockets, etc.). The socket must be movable.
2. Implement an acceptor with `accept()` and `is_running()`.
3. Instantiate `HttpServer<YourSocket>` and call `server.run(acceptor)` from your
   task or thread.

No other changes are needed. The core library has no OS dependencies.

## Contracts

`SocketConcept` (checked by the compiler):

```cpp
int  read (std::span<uint8_t>       buf);   // >0 bytes, 0 = peer closed, <0 = error/timeout
int  write(std::span<const uint8_t> buf);   // bytes written, <0 = error
void close();
```

Also needed by the engine and the pool: `bool is_valid() const`, a default
constructor that produces an invalid socket, and move construction / assignment.

`AcceptorConcept<Acceptor, Socket>`:

```cpp
Socket accept();          // blocks; returns an invalid socket on failure
bool   is_running();
```

Optional, detected with a `requires` expression:

- `bool set_read_timeout(unsigned ms)` on the socket. When present, the WebSocket
  session uses it for its idle tick. Apply `UCHTTP_IDLE_TIMEOUT_MS` to accepted
  sockets in your acceptor to get Slowloris protection for plain HTTP.

## Minimal skeleton

```cpp
class MySocket {
public:
    MySocket() = default;
    explicit MySocket(int handle) : h_(handle) {}
    MySocket(MySocket&&) noexcept;              // transfer ownership
    MySocket& operator=(MySocket&&) noexcept;
    ~MySocket() { close(); }

    int  read (std::span<uint8_t>       buf) noexcept;
    int  write(std::span<const uint8_t> buf) noexcept;
    void close() noexcept;
    bool is_valid() const noexcept { return h_ >= 0; }
private:
    int h_ { -1 };
};

class MyAcceptor {
public:
    MySocket accept() noexcept;        // return MySocket{} when nothing was accepted
    bool is_running() const noexcept;
};

uchttp::HttpServer<MySocket> server;
MyAcceptor acceptor;
server.get("/api/hello", handler);
server.run(acceptor);
```

`LinuxSocket` / `LinuxAcceptor` (`src/port/linux/`) and `ZephyrSocket` /
`ZephyrAcceptor` (`src/port/zephyr/`) are complete reference implementations.
`tests/mock_socket.h` is the smallest one.

## Other pluggable parts

| Part | Concept | Provided |
|---|---|---|
| TLS library | `TlsSessionConcept` | OpenSSL, wolfSSL. See [Security](security.md#tls--https). |
| SHA-1 for the WebSocket handshake | `ws::Sha1Concept` | `ws::SoftSha1`, `ZephyrPsaSha1` |
| WebSocket session memory | `ws::AllocatorConcept` | `ws::StaticAllocator`, `ZephyrSlabAllocator` |
