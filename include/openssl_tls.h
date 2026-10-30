#pragma once

// openssl_tls.h — OpenSSL TLS backend for host builds (requirement #4).
//
// Non-template: all bodies live in openssl_tls.cpp. OpenSslSession satisfies
// uchttp::TlsSessionConcept and OpenSslContext is usable as the Context of
// uchttp::TlsAcceptor. Transport is a uchttp::LinuxSocket (an accepted fd).
//
//   uchttp::OpenSslContext ctx;
//   ctx.load_cert_and_key("server.crt", "server.key");
//   uchttp::LinuxAcceptor raw(8443); raw.start();
//   uchttp::TlsAcceptor<uchttp::OpenSslContext, uchttp::LinuxAcceptor> tls(ctx, raw);
//   uchttp::HttpServer<uchttp::TlsSocket<uchttp::OpenSslSession>> server;
//   server.run(tls);

#include <cstdint>
#include <span>

#include "linux_socket.h"

// Forward-declare OpenSSL types to keep <openssl/*> out of the public header.
struct ssl_ctx_st;
struct ssl_st;

namespace uchttp {

// ---------------------------------------------------------------------------
// OpenSslSession — one TLS connection over an accepted fd. Move-only.
// ---------------------------------------------------------------------------

class OpenSslSession {
public:
    OpenSslSession() noexcept = default;
    // Takes ownership of `sock`'s fd and an SSL* created from the context.
    OpenSslSession(LinuxSocket&& sock, ssl_st* ssl) noexcept;

    OpenSslSession(const OpenSslSession&)            = delete;
    OpenSslSession& operator=(const OpenSslSession&) = delete;
    OpenSslSession(OpenSslSession&& o) noexcept;
    OpenSslSession& operator=(OpenSslSession&& o) noexcept;
    ~OpenSslSession();

    // TlsSessionConcept
    bool handshake() noexcept;
    int  read (std::span<uint8_t>       buf) noexcept;
    int  write(std::span<const uint8_t> buf) noexcept;
    void shutdown() noexcept;
    bool is_valid() const noexcept;
    bool set_read_timeout(unsigned ms) noexcept;

private:
    LinuxSocket sock_ {};
    ssl_st*     ssl_  { nullptr };
};

// ---------------------------------------------------------------------------
// OpenSslContext — owns the SSL_CTX (certificate, private key, protocol opts)
// and mints OpenSslSession instances for accepted transports.
// ---------------------------------------------------------------------------

class OpenSslContext {
public:
    using SessionType   = OpenSslSession;
    using RawSocketType = LinuxSocket;

    OpenSslContext() noexcept;
    OpenSslContext(const OpenSslContext&)            = delete;
    OpenSslContext& operator=(const OpenSslContext&) = delete;
    ~OpenSslContext();

    // Load the server certificate chain and its private key (PEM files).
    // Returns false on any OpenSSL error.
    bool load_cert_and_key(const char* cert_path, const char* key_path) noexcept;

    // Configure a pre-shared key identity/secret (PSK) instead of / in addition
    // to certificates. `secret` is copied into fixed storage (no heap).
    bool set_psk(const char* identity, std::span<const uint8_t> secret) noexcept;

    bool is_valid() const noexcept { return ctx_ != nullptr; }

    // Context (for TlsAcceptor): wrap an accepted transport in a TLS session.
    OpenSslSession make_session(LinuxSocket&& sock) noexcept;

private:
    ssl_ctx_st* ctx_ { nullptr };
};

} // namespace uchttp
