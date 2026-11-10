// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#include "openssl_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <cstring>

namespace uchttp {

// ---------------------------------------------------------------------------
// PSK storage (optional). One global identity/secret is enough for the server
// side; kept in fixed storage to honour the no-heap rule.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t PSK_ID_CAP  = 128;
constexpr size_t PSK_KEY_CAP = 64;

struct PskConfig {
    char          identity[PSK_ID_CAP] {};
    size_t        identity_len { 0 };
    unsigned char secret[PSK_KEY_CAP] {};
    size_t        secret_len { 0 };
    bool          enabled { false };
};

PskConfig& psk() noexcept {
    static PskConfig p;
    return p;
}

// OpenSSL server PSK callback: match the identity and copy out the secret.
unsigned int psk_server_cb(SSL* /*ssl*/, const char* identity,
                           unsigned char* psk_out, unsigned int max_psk_len) {
    PskConfig& p = psk();
    if (!p.enabled) return 0;
    if (identity == nullptr) return 0;
    if (std::strncmp(identity, p.identity, p.identity_len) != 0 ||
        identity[p.identity_len] != '\0') {
        return 0;
    }
    if (p.secret_len > max_psk_len) return 0;
    std::memcpy(psk_out, p.secret, p.secret_len);
    return static_cast<unsigned int>(p.secret_len);
}

} // namespace

// ---------------------------------------------------------------------------
// OpenSslSession
// ---------------------------------------------------------------------------

OpenSslSession::OpenSslSession(LinuxSocket&& sock, ssl_st* ssl) noexcept
    : sock_(std::move(sock)), ssl_(ssl) {
    if (ssl_ && sock_.is_valid()) {
        SSL_set_fd(ssl_, sock_.fd());
    }
}

OpenSslSession::OpenSslSession(OpenSslSession&& o) noexcept
    : sock_(std::move(o.sock_)), ssl_(o.ssl_) {
    o.ssl_ = nullptr;
}

OpenSslSession& OpenSslSession::operator=(OpenSslSession&& o) noexcept {
    if (this != &o) {
        if (ssl_) SSL_free(ssl_);
        sock_ = std::move(o.sock_);
        ssl_  = o.ssl_;
        o.ssl_ = nullptr;
    }
    return *this;
}

OpenSslSession::~OpenSslSession() {
    if (ssl_) SSL_free(ssl_);
}

bool OpenSslSession::handshake() noexcept {
    if (!ssl_) return false;
    int rc = SSL_accept(ssl_);
    return rc == 1;
}

int OpenSslSession::read(std::span<uint8_t> buf) noexcept {
    if (!ssl_) return -1;
    int n = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
    if (n > 0) return n;
    // Treat clean shutdown / want-read timeout as EOF for the engine.
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_ZERO_RETURN) return 0;
    return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) ? 0 : -1;
}

int OpenSslSession::write(std::span<const uint8_t> buf) noexcept {
    if (!ssl_) return -1;
    int n = SSL_write(ssl_, buf.data(), static_cast<int>(buf.size()));
    return n > 0 ? n : -1;
}

void OpenSslSession::shutdown() noexcept {
    if (ssl_) {
        SSL_shutdown(ssl_);
    }
    sock_.close();
}

bool OpenSslSession::is_valid() const noexcept {
    return ssl_ != nullptr && sock_.is_valid();
}

bool OpenSslSession::set_read_timeout(unsigned ms) noexcept {
    return sock_.set_read_timeout(ms);
}

// ---------------------------------------------------------------------------
// OpenSslContext
// ---------------------------------------------------------------------------

OpenSslContext::OpenSslContext() noexcept {
    // Modern OpenSSL (>=1.1) initialises itself; this is a no-op guard.
    const SSL_METHOD* method = TLS_server_method();
    ctx_ = SSL_CTX_new(method);
    if (ctx_) {
        // Require TLS 1.2+ for a sane security baseline.
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    }
}

OpenSslContext::~OpenSslContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

bool OpenSslContext::load_cert_and_key(const char* cert_path,
                                       const char* key_path) noexcept {
    if (!ctx_) return false;
    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path) != 1) return false;
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path, SSL_FILETYPE_PEM) != 1)
        return false;
    return SSL_CTX_check_private_key(ctx_) == 1;
}

bool OpenSslContext::set_psk(const char* identity,
                             std::span<const uint8_t> secret) noexcept {
    if (!ctx_ || identity == nullptr) return false;
    PskConfig& p = psk();
    size_t id_len = std::strlen(identity);
    if (id_len >= PSK_ID_CAP || secret.size() > PSK_KEY_CAP) return false;

    std::memcpy(p.identity, identity, id_len);
    p.identity[id_len] = '\0';
    p.identity_len = id_len;
    std::memcpy(p.secret, secret.data(), secret.size());
    p.secret_len = secret.size();
    p.enabled = true;

    SSL_CTX_set_psk_server_callback(ctx_, psk_server_cb);
    return true;
}

OpenSslSession OpenSslContext::make_session(LinuxSocket&& sock) noexcept {
    if (!ctx_) return OpenSslSession{};
    SSL* ssl = SSL_new(ctx_);
    return OpenSslSession(std::move(sock), ssl);
}

} // namespace uchttp
