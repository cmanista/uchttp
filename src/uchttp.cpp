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

void HttpRequest::reset() noexcept {
    method       = HttpMethod::UNKNOWN;
    path         = {};
    body         = {};
    header_count = 0;
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

void HttpResponse::set_body(std::string_view body) noexcept {
    body_ = body;
}

void HttpResponse::reset() noexcept {
    len_           = 0;
    sent_          = false;
    status_code_   = 200;
    status_reason_ = "OK";
    content_type_  = {};
    body_          = {};
    hdr_count_     = 0;
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
    return HPE_OK;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->request.body.empty()) {
        s->request.body = { at, len };
    } else {
        s->request.body = { s->request.body.data(),
                            s->request.body.size() + len };
    }
    return HPE_OK;
}

static int cb_on_message_complete(llhttp_t* p) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->message_complete = true;
    return HPE_OK;
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
