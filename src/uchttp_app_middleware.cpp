#include "uchttp_app_middleware.h"

#include <cstring>

namespace uchttp::app {

// ---------------------------------------------------------------------------
// Fixed storage (no heap). Header values handed to res.add_header() must
// outlive send(); these buffers live for the program's lifetime. One request
// is processed at a time per connection, so a per-module buffer is safe for
// the single-threaded engine model. A threaded RTOS port would make these
// thread-local.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t CC_CAP  = 128;

struct CacheCfg {
    char   value[CC_CAP] { 'n','o','-','c','a','c','h','e' };
    size_t len { 8 };
};

CacheCfg& cache_cfg() noexcept {
    static CacheCfg c;
    return c;
}

// Buffer holding the ETag of the current response (quoted FNV-1a hash).
thread_local char  g_etag_buf[20];
thread_local char  g_reqid_buf[24];

// FNV-1a 64-bit.
uint64_t fnv1a(std::string_view s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Write v as 16 lowercase hex digits into out (exactly 16 bytes).
void hex16(uint64_t v, char* out) noexcept {
    static const char* d = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[i] = d[v & 0xF];
        v >>= 4;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Cache-Control
// ---------------------------------------------------------------------------

void set_cache_control(std::string_view value) noexcept {
    CacheCfg& c = cache_cfg();
    c.len = value.size() < CC_CAP ? value.size() : CC_CAP;
    if (c.len) __builtin_memcpy(c.value, value.data(), c.len);
}

void cache_control(const HttpRequest& req, HttpResponse& res,
                   MiddlewareChain& chain) noexcept {
    chain.next(req, res);
    const CacheCfg& c = cache_cfg();
    if (c.len) res.add_header("Cache-Control", std::string_view(c.value, c.len));
}

// ---------------------------------------------------------------------------
// ETag + If-None-Match -> 304
// ---------------------------------------------------------------------------

size_t compute_etag(std::string_view body, char* out, size_t cap) noexcept {
    // Format: "<16 hex>" (quoted) -> 18 chars.
    if (cap < 18) return 0;
    out[0] = '"';
    hex16(fnv1a(body), out + 1);
    out[17] = '"';
    return 18;
}

void etag(const HttpRequest& req, HttpResponse& res,
          MiddlewareChain& chain) noexcept {
    chain.next(req, res);

    // Only tag cacheable, bodied success responses.
    if (res.status() != 200 || res.body().empty()) return;

    size_t n = compute_etag(res.body(), g_etag_buf, sizeof(g_etag_buf));
    if (n == 0) return;
    std::string_view tag(g_etag_buf, n);

    res.add_header("ETag", tag);

    // Conditional GET: if the client already has this representation, reply 304.
    std::string_view inm = req.get_header("If-None-Match");
    if (!inm.empty() && inm == tag) {
        res.set_status(304);
        res.set_body({});          // 304 carries no body
    }
}

// ---------------------------------------------------------------------------
// Accept-Encoding
// ---------------------------------------------------------------------------

bool client_accepts_encoding(const HttpRequest& req,
                             std::string_view coding) noexcept {
    std::string_view ae = req.get_header("Accept-Encoding");
    size_t pos = 0;
    while ((pos = ae.find(coding, pos)) != std::string_view::npos) {
        // Ensure it is a whole token (bounded by start/comma/space/;).
        bool left_ok  = pos == 0 ||
                        ae[pos - 1] == ',' || ae[pos - 1] == ' ';
        size_t end = pos + coding.size();
        bool right_ok = end == ae.size() ||
                        ae[end] == ',' || ae[end] == ';' || ae[end] == ' ';
        if (left_ok && right_ok) return true;
        pos = end;
    }
    return false;
}

// ---------------------------------------------------------------------------
// X-Request-ID
// ---------------------------------------------------------------------------

void request_id(const HttpRequest& req, HttpResponse& res,
                MiddlewareChain& chain) noexcept {
    std::string_view incoming = req.get_header("X-Request-ID");

    std::string_view id;
    if (!incoming.empty() && incoming.size() < sizeof(g_reqid_buf)) {
        __builtin_memcpy(g_reqid_buf, incoming.data(), incoming.size());
        id = std::string_view(g_reqid_buf, incoming.size());
    } else {
        // Generate a 16-hex-digit id from a monotonic counter mixed via FNV.
        static thread_local uint64_t counter = 0;
        uint64_t v = fnv1a(std::string_view(
            reinterpret_cast<const char*>(&counter), sizeof(counter)));
        ++counter;
        hex16(v, g_reqid_buf);
        id = std::string_view(g_reqid_buf, 16);
    }

    // Expose on the response before the handler runs, so handlers/log hooks
    // can read it back too.
    res.add_header("X-Request-ID", id);
    chain.next(req, res);
}

} // namespace uchttp::app
