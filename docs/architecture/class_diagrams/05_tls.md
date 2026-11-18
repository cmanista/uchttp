# TLS / HTTPS

Source: `include/uchttp_tls.h`, `include/openssl_tls.h`, `include/wolfssl_tls.h`.
TLS on Zephyr (at the socket level) is described in [06_platform_ports.md](06_platform_ports.md).
[← index](README.md)

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

## Notes

- The crypto library is replaceable behind `TlsSessionConcept`. `TlsSocket<Session>` satisfies `SocketConcept`, so `HttpServer<TlsSocket<...>>` works without any engine change (including SSE and WebSocket).
- The TLS handshake runs lazily on the first `read` / `write`, or explicitly through `handshake()`.
- `TlsSocket::set_read_timeout` carries Slowloris protection over to TLS connections. `HttpConnection` also uses it for the WebSocket timeout (detected with `requires`).
- The OpenSSL backend is built whenever OpenSSL is available. The wolfSSL backend is built when `UCHTTP_WITH_WOLFSSL=ON`.
- The OpenSSL and wolfSSL types are only forward-declared in the headers, so `<openssl/*>` does not leak into the public API.
- A `Context` must provide `SessionType`, `RawSocketType` and `make_session(RawSocket&&)`.
