# TLS / HTTPS

Źródło: `include/uchttp_tls.h`, `include/openssl_tls.h`, `include/wolfssl_tls.h`.
Zephyr (TLS na poziomie gniazda) opisany w [06_platform_ports.md](06_platform_ports.md).
[← indeks](../../class_diagram.md)

```mermaid
classDiagram
    class SocketConcept {
        <<concept>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
    }

    class TlsSessionConcept {
        <<concept>>
        +handshake() bool
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +shutdown() void
        +is_valid() bool
        +set_read_timeout(ms) bool
    }

    class TlsSocket~Session~ {
        +TlsSocket(session)
        +read(buf) int
        +write(buf) int
        +close() void
        +is_valid() bool
        +set_read_timeout(ms) bool
        +handshake() bool
        -session_ : Session
        -handshaken_ : bool
        -do_handshake() bool
    }

    class TlsAcceptor~Context_RawAcceptor~ {
        +TlsAcceptor(ctx, raw)
        +accept() TlsSocket
        +is_running() bool
        -ctx_ : Context*
        -raw_ : RawAcceptor*
    }

    class OpenSslContext {
        +SessionType : OpenSslSession
        +RawSocketType : LinuxSocket
        +load_cert_and_key(cert_path, key_path) bool
        +set_psk(identity, secret) bool
        +is_valid() bool
        +make_session(sock) OpenSslSession
        -ctx_ : ssl_ctx_st*
    }

    class OpenSslSession {
        +handshake() bool
        +read(buf) int
        +write(buf) int
        +shutdown() void
        +is_valid() bool
        +set_read_timeout(ms) bool
        -sock_ : LinuxSocket
        -ssl_ : ssl_st*
    }

    class WolfSslContext {
        +SessionType : WolfSslSession
        +RawSocketType : LinuxSocket
        +load_cert_and_key(cert_path, key_path) bool
        +set_psk(identity, secret) bool
        +is_valid() bool
        +make_session(sock) WolfSslSession
        -ctx_ : WOLFSSL_CTX*
    }

    class WolfSslSession {
        +handshake() bool
        +read(buf) int
        +write(buf) int
        +shutdown() void
        +is_valid() bool
        +set_read_timeout(ms) bool
        -sock_ : LinuxSocket
        -ssl_ : WOLFSSL*
    }

    class LinuxSocket {
        <<see 06_platform_ports>>
    }
    class LinuxAcceptor {
        <<see 06_platform_ports>>
    }
    class AcceptorConcept {
        <<concept>>
    }

    TlsSessionConcept <|.. OpenSslSession : satisfies
    TlsSessionConcept <|.. WolfSslSession : satisfies
    SocketConcept <|.. TlsSocket : satisfies
    TlsSocket *-- TlsSessionConcept : wraps Session
    AcceptorConcept <|.. TlsAcceptor : satisfies
    TlsAcceptor ..> TlsSocket : produces
    TlsAcceptor o-- OpenSslContext : Context (or WolfSslContext)
    TlsAcceptor o-- LinuxAcceptor : RawAcceptor
    OpenSslContext ..> OpenSslSession : make_session
    WolfSslContext ..> WolfSslSession : make_session
    OpenSslSession *-- LinuxSocket : owns fd
    WolfSslSession *-- LinuxSocket : owns fd
```

## Uwagi

- Biblioteka kryptograficzna jest podmienialna za `TlsSessionConcept`; `TlsSocket<Session>` spełnia `SocketConcept`, więc `HttpServer<TlsSocket<...>>` działa bez zmian silnika (w tym SSE i WebSocket).
- Handshake TLS wykonywany jest leniwie przy pierwszym `read`/`write` (lub jawnie przez `handshake()`).
- `TlsSocket::set_read_timeout` przenosi ochronę Slowloris na połączenia TLS; `HttpConnection` używa tego także dla timeoutu WS (wykrywane przez `requires`).
- Backend OpenSSL jest budowany zawsze, gdy OpenSSL jest dostępny; wolfSSL — gdy `UCHTTP_WITH_WOLFSSL=ON`.
- Typy OpenSSL/wolfSSL są tylko zadeklarowane w nagłówkach (forward declaration) — `<openssl/*>` nie wycieka do publicznego API.
- `Context` musi udostępniać `SessionType`, `RawSocketType` i `make_session(RawSocket&&)`.
