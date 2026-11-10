// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#include "wolfssl_tls.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <cstring>

namespace uchttp {

// ---------------------------------------------------------------------------
// PSK storage (optional) — fixed, no heap.
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

unsigned int psk_server_cb(WOLFSSL* /*ssl*/, const char* identity,
                           unsigned char* key, unsigned int key_max_len) {
    PskConfig& p = psk();
    if (!p.enabled || identity == nullptr) return 0;
    if (std::strncmp(identity, p.identity, p.identity_len) != 0 ||
        identity[p.identity_len] != '\0') {
        return 0;
    }
    if (p.secret_len > key_max_len) return 0;
    std::memcpy(key, p.secret, p.secret_len);
    return static_cast<unsigned int>(p.secret_len);
}

bool g_wolf_inited = false;

} // namespace

// ---------------------------------------------------------------------------
// WolfSslSession
// ---------------------------------------------------------------------------

WolfSslSession::WolfSslSession(LinuxSocket&& sock, WOLFSSL* ssl) noexcept
    : sock_(std::move(sock)), ssl_(ssl) {
    if (ssl_ && sock_.is_valid()) {
        wolfSSL_set_fd(ssl_, sock_.fd());
    }
}

WolfSslSession::WolfSslSession(WolfSslSession&& o) noexcept
    : sock_(std::move(o.sock_)), ssl_(o.ssl_) {
    o.ssl_ = nullptr;
}

WolfSslSession& WolfSslSession::operator=(WolfSslSession&& o) noexcept {
    if (this != &o) {
        if (ssl_) wolfSSL_free(ssl_);
        sock_ = std::move(o.sock_);
        ssl_  = o.ssl_;
        o.ssl_ = nullptr;
    }
    return *this;
}

WolfSslSession::~WolfSslSession() {
    if (ssl_) wolfSSL_free(ssl_);
}

bool WolfSslSession::handshake() noexcept {
    if (!ssl_) return false;
    return wolfSSL_accept(ssl_) == WOLFSSL_SUCCESS;
}

int WolfSslSession::read(std::span<uint8_t> buf) noexcept {
    if (!ssl_) return -1;
    int n = wolfSSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
    if (n > 0) return n;
    int err = wolfSSL_get_error(ssl_, n);
    if (err == WOLFSSL_ERROR_ZERO_RETURN) return 0;
    return (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)
               ? 0 : -1;
}

int WolfSslSession::write(std::span<const uint8_t> buf) noexcept {
    if (!ssl_) return -1;
    int n = wolfSSL_write(ssl_, buf.data(), static_cast<int>(buf.size()));
    return n > 0 ? n : -1;
}

void WolfSslSession::shutdown() noexcept {
    if (ssl_) wolfSSL_shutdown(ssl_);
    sock_.close();
}

bool WolfSslSession::is_valid() const noexcept {
    return ssl_ != nullptr && sock_.is_valid();
}

bool WolfSslSession::set_read_timeout(unsigned ms) noexcept {
    return sock_.set_read_timeout(ms);
}

// ---------------------------------------------------------------------------
// WolfSslContext
// ---------------------------------------------------------------------------

WolfSslContext::WolfSslContext() noexcept {
    if (!g_wolf_inited) {
        wolfSSL_Init();
        g_wolf_inited = true;
    }
    ctx_ = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
}

WolfSslContext::~WolfSslContext() {
    if (ctx_) wolfSSL_CTX_free(ctx_);
}

bool WolfSslContext::load_cert_and_key(const char* cert_path,
                                       const char* key_path) noexcept {
    if (!ctx_) return false;
    if (wolfSSL_CTX_use_certificate_chain_file(ctx_, cert_path)
            != WOLFSSL_SUCCESS)
        return false;
    return wolfSSL_CTX_use_PrivateKey_file(ctx_, key_path, WOLFSSL_FILETYPE_PEM)
           == WOLFSSL_SUCCESS;
}

bool WolfSslContext::set_psk(const char* identity,
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

    wolfSSL_CTX_set_psk_server_callback(ctx_, psk_server_cb);
    return true;
}

WolfSslSession WolfSslContext::make_session(LinuxSocket&& sock) noexcept {
    if (!ctx_) return WolfSslSession{};
    WOLFSSL* ssl = wolfSSL_new(ctx_);
    return WolfSslSession(std::move(sock), ssl);
}

} // namespace uchttp
