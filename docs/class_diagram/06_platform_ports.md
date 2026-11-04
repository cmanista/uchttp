# Platform ports and test doubles

Źródło: `src/linux_socket.h`, `src/port/zephyr_*.h`, `tests/mock_socket.h`.
[← indeks](../../class_diagram.md)

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

    class SocketBase~Derived~ {
        <<CRTP base>>
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
    SocketBase <|-- LinuxSocket : optional CRTP

    LinuxAcceptor ..> LinuxSocket : produces
    ZephyrAcceptor ..> ZephyrSocket : produces
    ZephyrTlsAcceptor ..> ZephyrSocket : produces (TLS transparent)
    ZephyrTlsAcceptor ..> ZephyrTlsCredentials : sec_tag
    MockAcceptor ..> MockSocket : produces

    Sha1Concept <|.. ZephyrPsaSha1
    AllocatorConcept <|.. ZephyrSlabAllocator
```

## Uwagi

- **Linux** (`LinuxSocket`, `LinuxAcceptor`): POSIX `recv`/`send`, `SO_RCVTIMEO` jako idle timeout (`UCHTTP_IDLE_TIMEOUT_MS`). Służy do testów na hoście; jest też transportem backendów OpenSSL/wolfSSL (zob. [05_tls.md](05_tls.md)).
- **Zephyr** (`ZephyrSocket`, `ZephyrAcceptor`): natywne API `zsock_*`/`net_*`. Rdzeń (`uchttp.h`) jest niezmieniony — różni się tylko port.
- **Zephyr TLS**: TLS (Mbed TLS / PSA Crypto) działa transparentnie na poziomie gniazda (`IPPROTO_TLS_1_2` + `sec_tag`), więc nie ma osobnej sesji — `ZephyrTlsAcceptor` zwraca zwykły `ZephyrSocket`. `ZephyrTlsCredentials` rejestruje certyfikat/klucz, CA lub PSK; bufory należy trzymać przy życiu (store nie kopiuje).
- **Zephyr WS**: `ZephyrPsaSha1` (PSA Crypto, akceleracja sprzętowa tam gdzie sterownik PSA wspiera SHA-1) i `ZephyrSlabAllocator` (`k_mem_slab`) podłączane przez `ws::Traits`.
- **Porting na inny RTOS**: wystarczy własny `Socket` (`read`, `write`, `close`, `is_valid`, opcjonalnie `set_read_timeout`) i `Acceptor` (`accept`, `is_running`).
- **Testy**: `MockSocket`/`MockAcceptor` (`namespace uchttp_test`) to bufory w pamięci; `MockAcceptor` obsługuje dokładnie jedno połączenie.
