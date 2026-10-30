#include "uchttp.h"
#include <cstring>

namespace uchttp {

// ---------------------------------------------------------------------------
// HttpMethod helpers
// ---------------------------------------------------------------------------

HttpMethod llhttp_method_to_uchttp(uint8_t m) noexcept {
    switch (m) {
        case HTTP_GET:    return HttpMethod::GET;
        case HTTP_POST:   return HttpMethod::POST;
        case HTTP_PUT:    return HttpMethod::PUT;
        case HTTP_DELETE: return HttpMethod::DELETE;
        case HTTP_PATCH:  return HttpMethod::PATCH;
        case HTTP_HEAD:   return HttpMethod::HEAD;
        case HTTP_OPTIONS:return HttpMethod::OPTIONS;
        default:          return HttpMethod::UNKNOWN;
    }
}

const char* method_string(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::GET:    return "GET";
        case HttpMethod::POST:   return "POST";
        case HttpMethod::PUT:    return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::PATCH:  return "PATCH";
        case HttpMethod::HEAD:   return "HEAD";
        case HttpMethod::OPTIONS:return "OPTIONS";
        default:                 return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

std::string_view HttpRequest::get_header(std::string_view name) const noexcept {
    for (uint8_t i = 0; i < header_count; ++i) {
        if (headers[i].name == name) return headers[i].value;
    }
    return {};
}

std::string_view HttpRequest::host() const noexcept {
    return get_header("Host");
}

QueryString HttpRequest::query() const noexcept {
    return QueryString(raw_query);
}

std::string_view HttpRequest::query_param(std::string_view key) const noexcept {
    return QueryString(raw_query).get(key);
}

std::string_view HttpRequest::path_param(std::string_view name) const noexcept {
    for (uint8_t i = 0; i < param_count; ++i) {
        if (path_params[i].name == name) return path_params[i].value;
    }
    return {};
}

void HttpRequest::reset() noexcept {
    method         = HttpMethod::UNKNOWN;
    path           = {};
    raw_query      = {};
    body           = {};
    header_count   = 0;
    param_count    = 0;
    http_major     = 1;
    http_minor     = 1;
    keep_alive     = false;
    chunked        = false;
    content_length = 0;
}

// ---------------------------------------------------------------------------
// HttpResponse – non-template methods
// ---------------------------------------------------------------------------

void HttpResponse::bind(std::span<char> tx_buf) noexcept {
    buf_  = tx_buf.data();
    cap_  = tx_buf.size();
    len_  = 0;
    sent_ = false;
}

void HttpResponse::set_status(int code, std::string_view reason) noexcept {
    status_code_   = code;
    status_reason_ = reason.empty() ? default_reason(code) : reason;
}

void HttpResponse::add_header(std::string_view name,
                               std::string_view value) noexcept {
    if (hdr_count_ < UCHTTP_MAX_HEADERS) {
        resp_headers_[hdr_count_++] = { name, value };
    }
}

void HttpResponse::set_content_type(std::string_view ct) noexcept {
    content_type_ = ct;
}

void HttpResponse::set_content_encoding(std::string_view enc) noexcept {
    content_encoding_ = enc;
}

void HttpResponse::set_body(std::string_view body) noexcept {
    body_ = body;
}

void HttpResponse::set_keep_alive(bool v) noexcept {
    keep_alive_ = v;
}

bool HttpResponse::keep_alive() const noexcept {
    return keep_alive_;
}

void HttpResponse::reset() noexcept {
    len_              = 0;
    sent_             = false;
    status_code_      = 200;
    status_reason_    = "OK";
    content_type_     = {};
    content_encoding_ = {};
    body_             = {};
    keep_alive_       = false;
    hdr_count_        = 0;
}

std::string_view HttpResponse::formatted() const noexcept {
    return { buf_, len_ };
}

const char* HttpResponse::default_reason(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "Unknown";
    }
}

void HttpResponse::append(std::string_view s) noexcept {
    size_t n = (s.size() < cap_ - len_) ? s.size() : cap_ - len_;
    __builtin_memcpy(buf_ + len_, s.data(), n);
    len_ += n;
}

void HttpResponse::append(char c) noexcept {
    if (len_ < cap_) buf_[len_++] = c;
}

void HttpResponse::append_int(int v) noexcept {
    char tmp[12];
    int  i = 11;
    tmp[i] = '\0';
    if (v == 0) { append('0'); return; }
    bool     neg = v < 0;
    unsigned uv  = neg ? static_cast<unsigned>(-v) : static_cast<unsigned>(v);
    while (uv) { tmp[--i] = '0' + static_cast<char>(uv % 10); uv /= 10; }
    if (neg) tmp[--i] = '-';
    append(std::string_view(tmp + i));
}

// ---------------------------------------------------------------------------
// MiddlewareChain – non-template; invoked once per middleware per request, so
// the function-call cost is negligible next to socket I/O. Bodies live here to
// keep the header lean (product.md: implement in .cpp where no perf loss).
// ---------------------------------------------------------------------------

MiddlewareChain::MiddlewareChain(const MiddlewareFn* mws, size_t count,
                                 HandlerFn handler) noexcept
    : mws_(mws), count_(count), handler_(handler) {}

void MiddlewareChain::next(const HttpRequest& req, HttpResponse& res) noexcept {
    if (index_ < count_) {
        MiddlewareFn mw = mws_[index_++];
        if (mw) {
            mw(req, res, *this);
        } else {
            next(req, res);  // skip null slot
        }
    } else if (!handler_called_) {
        handler_called_ = true;
        if (handler_) handler_(req, res);
    }
}

void MiddlewareChain::run(const HttpRequest& req, HttpResponse& res) noexcept {
    index_ = 0;
    handler_called_ = false;
    next(req, res);
}

// ---------------------------------------------------------------------------
// ParseState
// ---------------------------------------------------------------------------

void ParseState::reset() noexcept {
    request.reset();
    hdr_field_start  = nullptr;
    hdr_field_len    = 0;
    hdr_value_start  = nullptr;
    hdr_value_len    = 0;
    in_header_value  = false;
    message_complete = false;
    parse_error      = false;
}

void ParseState::commit_header() noexcept {
    if (hdr_field_start && hdr_value_start &&
        request.header_count < UCHTTP_MAX_HEADERS) {
        request.headers[request.header_count++] = {
            { hdr_field_start, hdr_field_len },
            { hdr_value_start, hdr_value_len }
        };
    }
    hdr_field_start = nullptr; hdr_field_len = 0;
    hdr_value_start = nullptr; hdr_value_len = 0;
    in_header_value = false;
}

// ---------------------------------------------------------------------------
// llhttp C callbacks  (static – internal linkage, not exposed in any header)
// ---------------------------------------------------------------------------

static int cb_on_url(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->request.path.empty()) {
        s->request.path = { at, len };
    } else {
        s->request.path = { s->request.path.data(),
                            s->request.path.size() + len };
    }
    return HPE_OK;
}

static int cb_on_header_field(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->in_header_value) {
        s->commit_header();
    }
    if (!s->hdr_field_start) {
        s->hdr_field_start = at;
        s->hdr_field_len   = len;
    } else {
        s->hdr_field_len += len;
    }
    return HPE_OK;
}

static int cb_on_header_value(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->in_header_value = true;
    if (!s->hdr_value_start) {
        s->hdr_value_start = at;
        s->hdr_value_len   = len;
    } else {
        s->hdr_value_len += len;
    }
    return HPE_OK;
}

static int cb_on_headers_complete(llhttp_t* p) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->commit_header();
    s->request.method = llhttp_method_to_uchttp(llhttp_get_method(p));

    // Protocol metadata handled directly by the engine (requirement #3).
    s->request.http_major = llhttp_get_http_major(p);
    s->request.http_minor = llhttp_get_http_minor(p);
    s->request.content_length = p->content_length;
    s->request.chunked = (p->flags & F_CHUNKED) != 0;

    // Split the accumulated URL into path + query string (zero-copy).
    // The full URL was accumulated contiguously into request.path by cb_on_url.
    std::string_view url = s->request.path;
    auto q = url.find('?');
    if (q != std::string_view::npos) {
        s->request.path      = url.substr(0, q);
        s->request.raw_query = url.substr(q + 1);
    } else {
        s->request.raw_query = {};
    }
    return HPE_OK;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->request.body.empty()) {
        // First fragment: body starts where llhttp points into the RX buffer.
        s->request.body = { at, len };
    } else {
        // Subsequent fragment. With Content-Length the data is contiguous and
        // `at` already equals body.end() (the memmove is a no-op). With chunked
        // transfer-encoding the decoded chunks are separated by chunk framing
        // in the buffer, so we compact each fragment right after the previous
        // one to expose a single contiguous, decoded body (zero extra buffer).
        char* dst = const_cast<char*>(s->request.body.data()
                                      + s->request.body.size());
        if (dst != at) {
            __builtin_memmove(dst, at, len);
        }
        s->request.body = { s->request.body.data(),
                            s->request.body.size() + len };
    }
    return HPE_OK;
}

static int cb_on_message_complete(llhttp_t* p) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->message_complete = true;
    // Connection lifecycle (requirement #3): llhttp evaluates HTTP version +
    // Connection header to decide whether the socket may be reused.
    s->request.keep_alive = llhttp_should_keep_alive(p) != 0;
    // Pause so that a single read() carrying the tail of this request AND the
    // head of the next (keep-alive / pipelining) does not overwrite the
    // current request's state. The engine resumes after dispatching.
    return HPE_PAUSED;
}

// ---------------------------------------------------------------------------
// make_llhttp_settings  – single definition, single static instance
// ---------------------------------------------------------------------------

const llhttp_settings_t& make_llhttp_settings() noexcept {
    static llhttp_settings_t s{};
    static bool initialised = false;
    if (!initialised) {
        llhttp_settings_init(&s);
        s.on_url              = cb_on_url;
        s.on_header_field     = cb_on_header_field;
        s.on_header_value     = cb_on_header_value;
        s.on_headers_complete = cb_on_headers_complete;
        s.on_body             = cb_on_body;
        s.on_message_complete = cb_on_message_complete;
        initialised = true;
    }
    return s;
}

} // namespace uchttp
