#pragma once

// uchttp_tls.h — TLS/HTTPS abstraction (requirement #4).
//
// The crypto library is pluggable. A "TLS session" backend is any type that
// satisfies TlsSessionConcept; uchttp ships two:
//   * OpenSslSession  (host builds, src/openssl_tls.*)
//   * WolfSslSession   (embedded builds, src/wolfssl_tls.*, conditional)
//
// TlsSocket<Session> wraps a session and itself satisfies uchttp::SocketConcept,
// so the whole engine runs over TLS unchanged:
//
//   using TlsSock = uchttp::TlsSocket<uchttp::OpenSslSession>;
//   uchttp::HttpServer<TlsSock> server;
//
// Everything here is a template (CRTP/concepts, no virtuals) and therefore
// must live in the header. The concrete backends are non-template and live in
// their own .cpp files.

#include <cstdint>
#include <span>
#include <utility>
#include <concepts>

#include "uchttp.h"

namespace uchttp {

// ---------------------------------------------------------------------------
// TlsSessionConcept — the minimal surface a crypto backend must expose. A
// session owns one established transport (e.g. an accepted fd) plus its TLS
// state. It is move-only and performs the handshake lazily on first I/O or
// explicitly via handshake().
// ---------------------------------------------------------------------------

template <typename T>
concept TlsSessionConcept = requires(T& s, std::span<uint8_t> rx,
                                     std::span<const uint8_t> tx, unsigned ms) {
    { s.handshake()        } -> std::same_as<bool>;
    { s.read(rx)           } -> std::same_as<int>;
    { s.write(tx)          } -> std::same_as<int>;
    { s.shutdown()         } -> std::same_as<void>;
    { s.is_valid()         } -> std::same_as<bool>;
    { s.set_read_timeout(ms) } -> std::same_as<bool>;
};

// ---------------------------------------------------------------------------
// TlsSocket<Session> — adapts a TLS session to uchttp::SocketConcept.
// ---------------------------------------------------------------------------

template <TlsSessionConcept Session>
class TlsSocket {
public:
    TlsSocket() noexcept = default;
    explicit TlsSocket(Session&& session) noexcept
        : session_(std::move(session)) {}

    // Move-only (mirrors the session's ownership of the transport).
    TlsSocket(const TlsSocket&)            = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;
    TlsSocket(TlsSocket&&)                 = default;
    TlsSocket& operator=(TlsSocket&&)      = default;

    // --- SocketConcept ------------------------------------------------------
    int read(std::span<uint8_t> buf) noexcept {
        if (!handshaken_ && !do_handshake()) return -1;
        return session_.read(buf);
    }

    int write(std::span<const uint8_t> buf) noexcept {
        if (!handshaken_ && !do_handshake()) return -1;
        return session_.write(buf);
    }

    void close() noexcept {
        if (session_.is_valid()) session_.shutdown();
    }

    bool is_valid() const noexcept { return session_.is_valid(); }

    // Slowloris protection also applies to TLS connections.
    bool set_read_timeout(unsigned ms) noexcept {
        return session_.set_read_timeout(ms);
    }

    // Force the TLS handshake up front (optional; read/write do it lazily).
    bool handshake() noexcept { return do_handshake(); }

private:
    bool do_handshake() noexcept {
        if (handshaken_) return true;
        handshaken_ = session_.handshake();
        return handshaken_;
    }

    Session session_ {};
    bool    handshaken_ { false };
};

// ---------------------------------------------------------------------------
// TlsAcceptor<Context, RawAcceptor> — wraps a plaintext acceptor, wrapping
// every accepted transport in a TLS session produced by the crypto Context.
//
// Context must expose:
//   Session make_session(RawSocket&&);   // bind fd/transport to a TLS session
// and RawAcceptor must satisfy AcceptorConcept for its RawSocket.
// ---------------------------------------------------------------------------

template <typename Context, typename RawAcceptor>
class TlsAcceptor {
public:
    using Session   = typename Context::SessionType;
    using RawSocket = typename Context::RawSocketType;
    using Socket    = TlsSocket<Session>;

    TlsAcceptor(Context& ctx, RawAcceptor& raw) noexcept
        : ctx_(&ctx), raw_(&raw) {}

    // AcceptorConcept
    Socket accept() noexcept {
        RawSocket raw = raw_->accept();
        if (!raw.is_valid()) return Socket{};        // invalid -> skipped
        return Socket(ctx_->make_session(std::move(raw)));
    }

    bool is_running() const noexcept { return raw_->is_running(); }

private:
    Context*     ctx_;
    RawAcceptor* raw_;
};

} // namespace uchttp
