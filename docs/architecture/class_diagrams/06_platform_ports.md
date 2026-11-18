# Platform ports and test doubles

Source: `src/port/linux/linux_socket.h`, `src/port/zephyr/zephyr_*.h`, `tests/mock_socket.h`.
[← index](README.md)

```mermaid
classDiagram
    class SocketConcept {
        <<concept>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
    }

    class AcceptorConcept {
        <<concept>>
        +accept() Socket
        +is_running() bool
    }

    %% ---- Linux ----
    class LinuxSocket {
        +LinuxSocket(fd)
        +read(buf) int
        +write(buf) int
        +close() void
        +set_read_timeout(ms) bool
        +is_valid() bool
        +fd() int
        -fd_ : int
    }

    class LinuxAcceptor {
        +LinuxAcceptor(port, backlog)
        +start() bool
        +stop() void
        +accept() LinuxSocket
        +is_running() bool
        -port_ : uint16_t
        -backlog_ : int
        -listen_fd_ : int
        -running_ : bool
    }

    %% ---- Zephyr ----
    class ZephyrSocket {
        +ZephyrSocket(fd)
        +read(buf) int
        +write(buf) int
        +close() void
        +set_read_timeout(ms) bool
        +is_valid() bool
        +fd() int
        -fd_ : int
    }

    class ZephyrAcceptor {
        +ZephyrAcceptor(port, backlog)
        +start() bool
        +stop() void
        +accept() ZephyrSocket
        +is_running() bool
        -port_ : uint16_t
        -backlog_ : int
        -listen_fd_ : int
        -running_ : bool
    }

    class ZephyrTlsCredentials {
        +add_server_cert(tag, cert, cert_len, key, key_len) bool$
        +add_ca_cert(tag, ca, ca_len) bool$
        +add_psk(tag, psk, psk_len, identity, identity_len) bool$
    }

    class ZephyrTlsAcceptor {
        +ZephyrTlsAcceptor(port, sec_tag, backlog)
        +set_peer_verify(mode) void
        +start() bool
        +stop() void
        +accept() ZephyrSocket
        +is_running() bool
        -port_ : uint16_t
        -sec_tag_ : int
        -backlog_ : int
        -peer_verify_ : int
        -listen_fd_ : int
        -running_ : bool
    }

    class ZephyrPsaSha1 {
        +start() bool
        +update(data) bool
        +finish(digest) bool
        -op_ : psa_hash_operation_t
        -active_ : bool
    }

    class ZephyrSlabAllocator~Blocks_BlockSize~ {
        +init() void
        +allocate(n) void*
        +deallocate(p, n) void
        +used() size_t
        -slab_ : k_mem_slab
        -buf_ : uint8_t[]
        -inited_ : bool
    }

    class Sha1Concept {
        <<concept, see 04_websocket>>
    }
    class AllocatorConcept {
        <<concept, see 04_websocket>>
    }

    %% ---- Test doubles ----
    class MockSocket {
        +MockSocket(rx_data)
        +read(buf) int
        +write(buf) int
        +close() void
        +is_valid() bool
        +was_closed() bool
        +tx_view() string_view
        +tx_string() string
        +tx_status_line() string_view
        +tx_body() string_view
        +reset_tx() void
        +push_rx(more) void
        -rx_ : string
        -rx_pos_ : size_t
        -tx_ : string
        -closed_ : bool
    }

    class MockAcceptor {
        +MockAcceptor(rx_data)
        +accept() MockSocket
        +is_running() bool
        +was_accepted() bool
        -rx_data_ : string
        -accepted_ : bool
    }

    SocketConcept <|.. LinuxSocket
    SocketConcept <|.. ZephyrSocket
    SocketConcept <|.. MockSocket
    AcceptorConcept <|.. LinuxAcceptor
    AcceptorConcept <|.. ZephyrAcceptor
    AcceptorConcept <|.. ZephyrTlsAcceptor
    AcceptorConcept <|.. MockAcceptor

    LinuxAcceptor ..> LinuxSocket : produces
    ZephyrAcceptor ..> ZephyrSocket : produces
    ZephyrTlsAcceptor ..> ZephyrSocket : produces (TLS transparent)
    ZephyrTlsAcceptor ..> ZephyrTlsCredentials : sec_tag
    MockAcceptor ..> MockSocket : produces

    Sha1Concept <|.. ZephyrPsaSha1
    AllocatorConcept <|.. ZephyrSlabAllocator
```

## Notes

- **Linux** (`LinuxSocket`, `LinuxAcceptor`): POSIX `recv` / `send`, with `SO_RCVTIMEO` as the idle timeout (`UCHTTP_IDLE_TIMEOUT_MS`). Used for tests on a host. It is also the transport of the OpenSSL and wolfSSL backends (see [05_tls.md](05_tls.md)).
- **Zephyr** (`ZephyrSocket`, `ZephyrAcceptor`): the native `zsock_*` / `net_*` API. The core (`uchttp.h`) is unchanged, only the port differs.
- **Zephyr TLS**: TLS (Mbed TLS / PSA Crypto) works transparently at the socket level (`IPPROTO_TLS_1_2` + `sec_tag`), so there is no separate session type. `ZephyrTlsAcceptor` returns a plain `ZephyrSocket`. `ZephyrTlsCredentials` registers the certificate and key, a CA, or a PSK. Keep the buffers alive, because the credential store does not copy them.
- **Zephyr WebSocket**: `ZephyrPsaSha1` (PSA Crypto, hardware acceleration where the PSA driver supports SHA-1) and `ZephyrSlabAllocator` (`k_mem_slab`) are plugged in through `ws::Traits`.
- **Porting to another RTOS**: you need your own `Socket` (`read`, `write`, `close`, `is_valid`, optionally `set_read_timeout`) and `Acceptor` (`accept`, `is_running`).
- **Tests**: `MockSocket` / `MockAcceptor` (`namespace uchttp_test`) are in-memory buffers. `MockAcceptor` serves exactly one connection.
