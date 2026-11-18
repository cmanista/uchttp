// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

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

int HttpResponse::status() const noexcept {
    return status_code_;
}

std::string_view HttpResponse::body() const noexcept {
    return body_;
}

std::string_view HttpResponse::content_type() const noexcept {
    return content_type_;
}

// --- Streaming / SSE (requirement #7) --------------------------------------

void HttpResponse::bind_stream(RawWriteFn fn, void* ctx) noexcept {
    stream_write_ = fn;
    stream_ctx_   = ctx;
}

int HttpResponse::write_raw(std::span<const uint8_t> data) noexcept {
    if (!stream_write_) return -1;
    hijacked_ = true;              // handler owns the response from now on
    sent_     = true;              // suppress the engine's buffered send()
    return stream_write_(stream_ctx_, data.data(), data.size());
}

int HttpResponse::write_raw(std::string_view s) noexcept {
    return write_raw(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

bool HttpResponse::hijacked() const noexcept {
    return hijacked_;
}

bool HttpResponse::begin_sse() noexcept {
    if (!stream_write_) return false;
    static constexpr std::string_view head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    return write_raw(head) >= 0;
}

bool HttpResponse::send_event(std::string_view data,
                              std::string_view event,
                              std::string_view id) noexcept {
    if (!stream_write_) return false;

    // Build the frame in a small stack buffer. Fields:
    //   [id: <id>\n] [event: <event>\n] data: <line>\n (repeated) \n
    char   frame[512];
    size_t n = 0;
    auto put = [&](std::string_view s) noexcept {
        size_t c = s.size();
        if (n + c > sizeof(frame)) c = sizeof(frame) - n;
        __builtin_memcpy(frame + n, s.data(), c);
        n += c;
    };

    if (!id.empty())    { put("id: ");    put(id);    put("\n"); }
    if (!event.empty()) { put("event: "); put(event); put("\n"); }

    // Split data on '\n' into multiple "data:" lines per the SSE spec.
    size_t start = 0;
    while (start <= data.size()) {
        size_t nl = data.find('\n', start);
        std::string_view line = (nl == std::string_view::npos)
            ? data.substr(start)
            : data.substr(start, nl - start);
        put("data: "); put(line); put("\n");
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    put("\n");   // blank line terminates the event

    return write_raw(std::string_view(frame, n)) >= 0;
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
    hijacked_         = false;
    // stream_write_/stream_ctx_ are bound per-connection and intentionally
    // preserved across keep-alive requests.
}

std::string_view HttpResponse::formatted() const noexcept {
    return { buf_, len_ };
}

const char* HttpResponse::default_reason(int code) noexcept {
    switch (code) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
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
    header_bytes      = 0;
    body_bytes        = 0;
    headers_too_large = false;
    body_too_large    = false;
    stream_handler    = nullptr;
    stream_offset     = 0;
    stream_rejected   = false;
    // stream_resolve/stream_gate/stream_ctx/stream_response are bound
    // per-connection and preserved across keep-alive requests.
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
    s->header_bytes += len;
    if (s->header_bytes > UCHTTP_MAX_HEADER_BYTES) {
        s->headers_too_large = true;
        return HPE_USER;   // stop parsing; engine replies 431
    }
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
    s->header_bytes += len;
    if (s->header_bytes > UCHTTP_MAX_HEADER_BYTES) {
        s->headers_too_large = true;
        return HPE_USER;
    }
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
    s->header_bytes += len;
    if (s->header_bytes > UCHTTP_MAX_HEADER_BYTES) {
        s->headers_too_large = true;
        return HPE_USER;
    }
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

    // Streaming body / OTA (requirement #7): resolve the route now so the
    // engine can push body chunks to a streaming handler instead of buffering.
    if (s->stream_resolve) {
        s->stream_handler = s->stream_resolve(s->stream_ctx, s->request);

        // The streaming handler is driven from the parser, so the middleware
        // chain (auth, ...) has to run here, before the first body chunk. If a
        // middleware refuses, stop parsing: no byte of the body reaches the
        // handler and the engine replies with the middleware's response.
        if (s->stream_handler && s->stream_gate && s->stream_response &&
            !s->stream_gate(s->stream_ctx, s->request, *s->stream_response)) {
            s->stream_handler  = nullptr;
            s->stream_rejected = true;
            return HPE_USER;
        }
    }
    return HPE_OK;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);

    // Streaming body / OTA (requirement #7): push this chunk straight to the
    // handler (e.g. a flash writer) without buffering or size-capping — the
    // payload may be far larger than the RX buffer or UCHTTP_MAX_BODY_BYTES.
    if (s->stream_handler) {
        std::span<const uint8_t> chunk(
            reinterpret_cast<const uint8_t*>(at), len);
        s->stream_handler(s->request, *s->stream_response, chunk,
                          s->stream_offset, /*last=*/false);
        s->stream_offset += len;
        return HPE_OK;
    }

    s->body_bytes += len;
    if (s->body_bytes > UCHTTP_MAX_BODY_BYTES) {
        s->body_too_large = true;
        return HPE_USER;   // stop parsing; engine replies 413
    }
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

    // Streaming body / OTA (requirement #7): signal end-of-body to the handler
    // with an empty final chunk so it can finalise (flush flash, verify CRC).
    if (s->stream_handler) {
        s->stream_handler(s->request, *s->stream_response,
                          std::span<const uint8_t>{}, s->stream_offset,
                          /*last=*/true);
    }

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
