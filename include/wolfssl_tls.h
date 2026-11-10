// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

// wolfssl_tls.h — wolfSSL TLS backend for embedded builds (requirement #4).
//
// Mirrors the OpenSSL backend's surface so TlsSocket/TlsAcceptor work with
// either. Non-template: bodies live in wolfssl_tls.cpp. Only built when wolfSSL
// is available (see CMake option UCHTTP_WITH_WOLFSSL).
//
//   uchttp::WolfSslContext ctx;
//   ctx.load_cert_and_key("server.crt", "server.key");
//   using TlsSock = uchttp::TlsSocket<uchttp::WolfSslSession>;

#include <cstdint>
#include <span>

#include "linux_socket.h"

// Forward-declare wolfSSL opaque types.
struct WOLFSSL_CTX;
struct WOLFSSL;

namespace uchttp {

// ---------------------------------------------------------------------------
// WolfSslSession — one TLS connection over an accepted fd. Move-only.
// ---------------------------------------------------------------------------

class WolfSslSession {
public:
    WolfSslSession() noexcept = default;
    WolfSslSession(LinuxSocket&& sock, WOLFSSL* ssl) noexcept;

    WolfSslSession(const WolfSslSession&)            = delete;
    WolfSslSession& operator=(const WolfSslSession&) = delete;
    WolfSslSession(WolfSslSession&& o) noexcept;
    WolfSslSession& operator=(WolfSslSession&& o) noexcept;
    ~WolfSslSession();

    // TlsSessionConcept
    bool handshake() noexcept;
    int  read (std::span<uint8_t>       buf) noexcept;
    int  write(std::span<const uint8_t> buf) noexcept;
    void shutdown() noexcept;
    bool is_valid() const noexcept;
    bool set_read_timeout(unsigned ms) noexcept;

private:
    LinuxSocket sock_ {};
    WOLFSSL*    ssl_  { nullptr };
};

// ---------------------------------------------------------------------------
// WolfSslContext — owns the WOLFSSL_CTX and mints sessions.
// ---------------------------------------------------------------------------

class WolfSslContext {
public:
    using SessionType   = WolfSslSession;
    using RawSocketType = LinuxSocket;

    WolfSslContext() noexcept;
    WolfSslContext(const WolfSslContext&)            = delete;
    WolfSslContext& operator=(const WolfSslContext&) = delete;
    ~WolfSslContext();

    bool load_cert_and_key(const char* cert_path, const char* key_path) noexcept;
    bool set_psk(const char* identity, std::span<const uint8_t> secret) noexcept;

    bool is_valid() const noexcept { return ctx_ != nullptr; }

    WolfSslSession make_session(LinuxSocket&& sock) noexcept;

private:
    WOLFSSL_CTX* ctx_ { nullptr };
};

} // namespace uchttp
