#include "uchttp_security.h"

#include <cstring>

namespace uchttp::security {

// ---------------------------------------------------------------------------
// Module configuration — fixed-size static storage, no heap (product.md).
// ---------------------------------------------------------------------------

namespace {

constexpr size_t SECRET_CAP = 128;   // max length of a key/token
constexpr size_t HDRVAL_CAP = 256;   // max length of a header value

struct FixedStr {
    char   data[HDRVAL_CAP] {};
    size_t len { 0 };

    void set(std::string_view v) noexcept {
        len = v.size() < HDRVAL_CAP ? v.size() : HDRVAL_CAP;
        if (len) __builtin_memcpy(data, v.data(), len);
    }
    std::string_view view() const noexcept { return { data, len }; }
};

struct Config {
    char   api_key[SECRET_CAP] {};
    size_t api_key_len { 0 };

    char   bearer[SECRET_CAP] {};
    size_t bearer_len { 0 };

    FixedStr cors_origin;
    FixedStr frame_options;
    FixedStr csp;
    bool     defaults_ready { false };

    void ensure_defaults() noexcept {
        if (defaults_ready) return;
        cors_origin.set("*");
        frame_options.set("DENY");
        csp.set("default-src 'self'");
        defaults_ready = true;
    }
};

Config& config() noexcept {
    static Config c;
    c.ensure_defaults();
    return c;
}

void store_secret(char* dst, size_t& dst_len, std::string_view v) noexcept {
    dst_len = v.size() < SECRET_CAP ? v.size() : SECRET_CAP;
    if (dst_len) __builtin_memcpy(dst, v.data(), dst_len);
}

} // namespace

// ---------------------------------------------------------------------------
// secure_compare — constant time w.r.t. the compared content
// ---------------------------------------------------------------------------

bool secure_compare(std::string_view a, std::string_view b) noexcept {
    // Fold the length difference into the result and always walk the longer of
    // the two ranges so the loop count does not depend on where a mismatch is.
    size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= static_cast<unsigned>(ca ^ cb);
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Auth configuration
// ---------------------------------------------------------------------------

void set_api_key(std::string_view key) noexcept {
    Config& c = config();
    store_secret(c.api_key, c.api_key_len, key);
}

void set_bearer_token(std::string_view token) noexcept {
    Config& c = config();
    store_secret(c.bearer, c.bearer_len, token);
}

// ---------------------------------------------------------------------------
// Auth middleware
// ---------------------------------------------------------------------------

void api_key_auth(const HttpRequest& req, HttpResponse& res,
                  MiddlewareChain& chain) noexcept {
    const Config& c = config();
    std::string_view expected{ c.api_key, c.api_key_len };
    std::string_view got = req.get_header("X-API-Key");

    // Fail closed: an unconfigured key rejects everything.
    if (c.api_key_len == 0 || !secure_compare(expected, got)) {
        res.set_status(401);
        res.set_content_type("text/plain");
        res.set_body("Unauthorized");
        return;                       // short-circuit: handler never runs
    }
    chain.next(req, res);
}

void bearer_auth(const HttpRequest& req, HttpResponse& res,
                 MiddlewareChain& chain) noexcept {
    const Config& c = config();
    std::string_view expected{ c.bearer, c.bearer_len };
    std::string_view auth = req.get_header("Authorization");

    // Strip the "Bearer " scheme prefix if present.
    constexpr std::string_view kPrefix = "Bearer ";
    std::string_view token;
    if (auth.size() >= kPrefix.size() &&
        auth.substr(0, kPrefix.size()) == kPrefix) {
        token = auth.substr(kPrefix.size());
    }

    if (c.bearer_len == 0 || !secure_compare(expected, token)) {
        res.set_status(401);
        res.set_content_type("text/plain");
        res.add_header("WWW-Authenticate", "Bearer");
        res.set_body("Unauthorized");
        return;
    }
    chain.next(req, res);
}

// ---------------------------------------------------------------------------
// Security-headers configuration
// ---------------------------------------------------------------------------

void set_cors_origin(std::string_view origin) noexcept {
    config().cors_origin.set(origin);
}
void set_frame_options(std::string_view v) noexcept {
    config().frame_options.set(v);
}
void set_csp(std::string_view policy) noexcept {
    config().csp.set(policy);
}

// ---------------------------------------------------------------------------
// Security-headers middleware (onion model: inject after the handler runs)
// ---------------------------------------------------------------------------

void security_headers(const HttpRequest& req, HttpResponse& res,
                      MiddlewareChain& chain) noexcept {
    chain.next(req, res);

    const Config& c = config();
    if (c.cors_origin.len)
        res.add_header("Access-Control-Allow-Origin", c.cors_origin.view());
    res.add_header("X-Content-Type-Options", "nosniff");
    if (c.frame_options.len)
        res.add_header("X-Frame-Options", c.frame_options.view());
    if (c.csp.len)
        res.add_header("Content-Security-Policy", c.csp.view());
}

} // namespace uchttp::security
