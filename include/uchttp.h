#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <concepts>
#include <string_view>
#include <array>

#include "llhttp.h"

// ---------------------------------------------------------------------------
// Configuration – tune for your target MCU
// ---------------------------------------------------------------------------
#ifndef UCHTTP_MAX_HEADERS
#  define UCHTTP_MAX_HEADERS  16
#endif
#ifndef UCHTTP_MAX_ROUTES
#  define UCHTTP_MAX_ROUTES   16
#endif
#ifndef UCHTTP_RX_BUFFER_SIZE
#  define UCHTTP_RX_BUFFER_SIZE 1024
#endif
#ifndef UCHTTP_TX_BUFFER_SIZE
#  define UCHTTP_TX_BUFFER_SIZE 1024
#endif

namespace uchttp {

// ---------------------------------------------------------------------------
// SocketConcept  (CRTP base lives in user code, only the concept is needed)
// ---------------------------------------------------------------------------

template <typename T>
concept SocketConcept = requires(T& s,
                                  std::span<uint8_t>       rx,
                                  std::span<const uint8_t> tx) {
    { s.read(rx)  } -> std::same_as<int>;   // returns bytes read, <=0 on error/close
    { s.write(tx) } -> std::same_as<int>;   // returns bytes written, <0 on error
    { s.close()   } -> std::same_as<void>;
};

// CRTP base – optional, lets implementations inherit default no-ops.
template <typename Derived>
class SocketBase {
public:
    int  read (std::span<uint8_t>       buf) { return static_cast<Derived*>(this)->read_impl(buf);  }
    int  write(std::span<const uint8_t> buf) { return static_cast<Derived*>(this)->write_impl(buf); }
    void close()                             { static_cast<Derived*>(this)->close_impl(); }
protected:
    SocketBase() = default;
};

// ---------------------------------------------------------------------------
// HttpMethod
// ---------------------------------------------------------------------------

enum class HttpMethod : uint8_t { GET, POST, PUT, DELETE, PATCH, HEAD, UNKNOWN };

inline HttpMethod llhttp_method_to_uchttp(uint8_t m) noexcept {
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

inline const char* method_string(HttpMethod m) noexcept {
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
// HttpHeader  – zero-copy view into the raw RX buffer
// ---------------------------------------------------------------------------

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

// ---------------------------------------------------------------------------
// HttpRequest  – populated during parsing, all views into the RX buffer
// ---------------------------------------------------------------------------

struct HttpRequest {
    HttpMethod   method  { HttpMethod::UNKNOWN };
    std::string_view path;
    std::string_view body;

    uint8_t  header_count { 0 };
    HttpHeader headers[UCHTTP_MAX_HEADERS];

    std::string_view get_header(std::string_view name) const noexcept {
        for (uint8_t i = 0; i < header_count; ++i) {
            if (headers[i].name == name) return headers[i].value;
        }
        return {};
    }

    void reset() noexcept {
        method       = HttpMethod::UNKNOWN;
        path         = {};
        body         = {};
        header_count = 0;
    }
};

// ---------------------------------------------------------------------------
// HttpResponse  – writes into a caller-supplied TX buffer, zero heap
// ---------------------------------------------------------------------------

class HttpResponse {
public:
    // Bind to the TX buffer owned by HttpConnection (or a test stack buffer).
    void bind(std::span<char> tx_buf) noexcept {
        buf_   = tx_buf.data();
        cap_   = tx_buf.size();
        len_   = 0;
        sent_  = false;
    }

    void set_status(int code, std::string_view reason = {}) noexcept {
        status_code_   = code;
        status_reason_ = reason.empty() ? default_reason(code) : reason;
    }

    // Add a response header (values must remain valid until send())
    void add_header(std::string_view name, std::string_view value) noexcept {
        if (hdr_count_ < UCHTTP_MAX_HEADERS) {
            resp_headers_[hdr_count_++] = { name, value };
        }
    }

    void set_content_type(std::string_view ct) noexcept { content_type_ = ct; }

    // Set body from a compile-time / static string_view (zero-copy).
    void set_body(std::string_view body) noexcept { body_ = body; }

    // Build the HTTP/1.1 response into the TX buffer and send it via socket.
    // Returns true on success.
    template <SocketConcept Socket>
    bool send(Socket& socket) noexcept {
        if (sent_) return false;
        sent_ = true;

        // Format status line + headers into tx buffer
        len_ = 0;
        append("HTTP/1.1 ");
        append_int(status_code_);
        append(' ');
        append(status_reason_);
        append("\r\n");

        // Content-Type
        if (!content_type_.empty()) {
            append("Content-Type: ");
            append(content_type_);
            append("\r\n");
        }

        // Content-Length from body
        if (!body_.empty()) {
            append("Content-Length: ");
            append_int(static_cast<int>(body_.size()));
            append("\r\n");
        } else {
            append("Content-Length: 0\r\n");
        }

        // User-supplied headers
        for (uint8_t i = 0; i < hdr_count_; ++i) {
            append(resp_headers_[i].name);
            append(": ");
            append(resp_headers_[i].value);
            append("\r\n");
        }

        append("\r\n");

        // Body
        if (!body_.empty()) {
            append(body_);
        }

        // Write to socket
        auto data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf_), len_);
        return socket.write(data) >= 0;
    }

    // Convenience: send a complete response in one call
    template <SocketConcept Socket>
    bool send(Socket& socket, int status, std::string_view body,
              std::string_view content_type = "text/plain") noexcept {
        set_status(status);
        set_content_type(content_type);
        set_body(body);
        return send(socket);
    }

    // Expose the formatted buffer (useful in tests)
    std::string_view formatted() const noexcept { return { buf_, len_ }; }

    void reset() noexcept {
        len_        = 0;
        sent_       = false;
        status_code_ = 200;
        status_reason_ = "OK";
        content_type_  = {};
        body_          = {};
        hdr_count_     = 0;
    }

private:
    char*  buf_  { nullptr };
    size_t cap_  { 0 };
    size_t len_  { 0 };
    bool   sent_ { false };

    int              status_code_   { 200 };
    std::string_view status_reason_ { "OK" };
    std::string_view content_type_;
    std::string_view body_;

    uint8_t    hdr_count_ { 0 };
    HttpHeader resp_headers_[UCHTTP_MAX_HEADERS];

    static const char* default_reason(int code) noexcept {
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

    void append(std::string_view s) noexcept {
        size_t copy = (s.size() < cap_ - len_) ? s.size() : cap_ - len_;
        __builtin_memcpy(buf_ + len_, s.data(), copy);
        len_ += copy;
    }

    void append(char c) noexcept {
        if (len_ < cap_) buf_[len_++] = c;
    }

    void append_int(int v) noexcept {
        // itoa into a tiny local buffer – no heap
        char tmp[12];
        int  i = 11;
        tmp[i] = '\0';
        if (v == 0) { append('0'); return; }
        bool neg = v < 0;
        unsigned uv = neg ? static_cast<unsigned>(-v) : static_cast<unsigned>(v);
        while (uv) { tmp[--i] = '0' + (uv % 10); uv /= 10; }
        if (neg) tmp[--i] = '-';
        append(std::string_view(tmp + i));
    }
};

// ---------------------------------------------------------------------------
// Route  – static table entry
// ---------------------------------------------------------------------------

using HandlerFn = void (*)(const HttpRequest&, HttpResponse&);

struct Route {
    HttpMethod   method  { HttpMethod::UNKNOWN };
    std::string_view path;
    HandlerFn    handler { nullptr };
};

// ---------------------------------------------------------------------------
// RouteTable  – fixed-size, no heap, linear search (MCU friendly)
// ---------------------------------------------------------------------------

template <size_t MaxRoutes = UCHTTP_MAX_ROUTES>
class RouteTable {
public:
    bool add(HttpMethod method, std::string_view path, HandlerFn handler) noexcept {
        if (count_ >= MaxRoutes) return false;
        routes_[count_++] = { method, path, handler };
        return true;
    }

    const Route* find(HttpMethod method, std::string_view path) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (routes_[i].method == method && routes_[i].path == path) {
                return &routes_[i];
            }
        }
        return nullptr;
    }

    size_t size() const noexcept { return count_; }

private:
    Route  routes_[MaxRoutes] {};
    size_t count_ { 0 };
};

// ---------------------------------------------------------------------------
// ParseState  – per-connection state machine fed to llhttp callbacks
// ---------------------------------------------------------------------------

struct ParseState {
    HttpRequest  request;

    // Accumulation cursors for zero-copy header key/value spans
    // (llhttp may call on_header_field/value in multiple chunks on some
    //  implementations; we track the start pointer and accumulate length)
    const char* hdr_field_start { nullptr };
    size_t      hdr_field_len   { 0 };
    const char* hdr_value_start { nullptr };
    size_t      hdr_value_len   { 0 };
    bool        in_header_value { false };  // false = collecting field name

    bool        message_complete { false };
    bool        parse_error      { false };

    void reset() noexcept {
        request.reset();
        hdr_field_start  = nullptr;
        hdr_field_len    = 0;
        hdr_value_start  = nullptr;
        hdr_value_len    = 0;
        in_header_value  = false;
        message_complete = false;
        parse_error      = false;
    }

    // Finalise the current header field+value pair into request.headers[]
    void commit_header() noexcept {
        if (hdr_field_start && hdr_value_start &&
            request.header_count < UCHTTP_MAX_HEADERS) {
            request.headers[request.header_count++] = {
                { hdr_field_start, hdr_field_len  },
                { hdr_value_start, hdr_value_len  }
            };
        }
        hdr_field_start = nullptr; hdr_field_len = 0;
        hdr_value_start = nullptr; hdr_value_len = 0;
        in_header_value = false;
    }
};

// ---------------------------------------------------------------------------
// llhttp C callbacks (free functions, access ParseState via parser->data)
// ---------------------------------------------------------------------------

inline int cb_on_url(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    // Extend existing span or set start (URL may arrive in multiple chunks)
    if (s->request.path.empty()) {
        s->request.path = { at, len };
    } else {
        // Extend: the new chunk is contiguous in the RX buffer
        s->request.path = { s->request.path.data(),
                            s->request.path.size() + len };
    }
    return HPE_OK;
}

inline int cb_on_header_field(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->in_header_value) {
        // We were collecting a value; commit the previous pair and start fresh
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

inline int cb_on_header_value(llhttp_t* p, const char* at, size_t len) noexcept {
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

inline int cb_on_headers_complete(llhttp_t* p) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->commit_header();  // commit the last header pair
    s->request.method = llhttp_method_to_uchttp(llhttp_get_method(p));
    return HPE_OK;
}

inline int cb_on_body(llhttp_t* p, const char* at, size_t len) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    if (s->request.body.empty()) {
        s->request.body = { at, len };
    } else {
        s->request.body = { s->request.body.data(),
                            s->request.body.size() + len };
    }
    return HPE_OK;
}

inline int cb_on_message_complete(llhttp_t* p) noexcept {
    auto* s = static_cast<ParseState*>(p->data);
    s->message_complete = true;
    return HPE_OK;
}

// Build a static (no-alloc) llhttp_settings_t
inline const llhttp_settings_t& make_llhttp_settings() noexcept {
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

// ---------------------------------------------------------------------------
// HttpConnection  – one per accepted TCP connection, no heap
//
//  Template params:
//    Socket   – must satisfy SocketConcept
//    RxSize   – RX buffer size  (default UCHTTP_RX_BUFFER_SIZE)
//    TxSize   – TX buffer size  (default UCHTTP_TX_BUFFER_SIZE)
// ---------------------------------------------------------------------------

template <SocketConcept Socket,
          size_t RxSize = UCHTTP_RX_BUFFER_SIZE,
          size_t TxSize = UCHTTP_TX_BUFFER_SIZE>
class HttpConnection {
public:
    explicit HttpConnection(Socket socket) noexcept
        : socket_(static_cast<Socket&&>(socket))
    {
        llhttp_init(&parser_, HTTP_REQUEST, &make_llhttp_settings());
        parser_.data = &state_;
    }

    // Non-copyable, movable
    HttpConnection(const HttpConnection&)            = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
    HttpConnection(HttpConnection&&)                 = default;

    // Process one request/response cycle.
    // Returns true if a complete request was handled.
    template <size_t MaxRoutes>
    bool process(const RouteTable<MaxRoutes>& routes) noexcept {
        state_.reset();
        llhttp_reset(&parser_);

        // Read until we have a full message or error
        size_t total = 0;
        while (total < RxSize && !state_.message_complete && !state_.parse_error) {
            auto buf = std::span<uint8_t>(rx_buf_.data() + total,
                                          rx_buf_.size()  - total);
            int n = socket_.read(buf);
            if (n <= 0) break;

            llhttp_errno_t err = llhttp_execute(
                &parser_,
                reinterpret_cast<const char*>(rx_buf_.data() + total),
                static_cast<size_t>(n));

            if (err != HPE_OK && err != HPE_PAUSED) {
                state_.parse_error = true;
                break;
            }
            total += static_cast<size_t>(n);
        }

        if (state_.parse_error) {
            send_error(400, "Bad Request");
            return false;
        }
        if (!state_.message_complete) {
            return false;
        }

        // Route dispatch
        response_.bind(std::span<char>(tx_buf_.data(), tx_buf_.size()));
        response_.reset();

        const Route* route = routes.find(state_.request.method,
                                          state_.request.path);
        if (route && route->handler) {
            route->handler(state_.request, response_);
            // If the handler didn't call send(), send a 200 with empty body
            (void)response_.send(socket_);
        } else {
            send_error(404, "Not Found");
        }

        return true;
    }

    void close() noexcept { socket_.close(); }

private:
    Socket      socket_;
    ParseState  state_;
    HttpResponse response_;
    llhttp_t     parser_{};

    std::array<uint8_t, RxSize> rx_buf_{};
    std::array<char,    TxSize> tx_buf_{};

    void send_error(int code, std::string_view msg) noexcept {
        response_.bind(std::span<char>(tx_buf_.data(), tx_buf_.size()));
        response_.reset();
        response_.send(socket_, code, msg, "text/plain");
    }
};

// ---------------------------------------------------------------------------
// HttpServer  – accepts connections and dispatches them
//
//  The server itself is platform-agnostic. Platform-specific accept()
//  is provided by the Acceptor concept below.
// ---------------------------------------------------------------------------

template <typename T, typename Socket>
concept AcceptorConcept = requires(T& a) {
    // Returns a new socket (or invalid socket) for each accepted connection.
    // The returned type must satisfy SocketConcept.
    { a.accept() } -> std::same_as<Socket>;
    // Returns true while the server should keep running.
    { a.is_running() } -> std::same_as<bool>;
};

template <SocketConcept Socket,
          size_t MaxRoutes   = UCHTTP_MAX_ROUTES,
          size_t RxSize      = UCHTTP_RX_BUFFER_SIZE,
          size_t TxSize      = UCHTTP_TX_BUFFER_SIZE>
class HttpServer {
public:
    HttpServer() = default;

    bool add_route(HttpMethod method, std::string_view path,
                   HandlerFn handler) noexcept {
        return routes_.add(method, path, handler);
    }

    // Convenience helpers
    bool get (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::GET,  path, h); }
    bool post(std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::POST, path, h); }
    bool put (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::PUT,  path, h); }

    // Run loop – blocks until acceptor.is_running() returns false
    template <typename Acceptor>
        requires AcceptorConcept<Acceptor, Socket>
    void run(Acceptor& acceptor) noexcept {
        while (acceptor.is_running()) {
            Socket sock = acceptor.accept();
            if (!sock.is_valid()) continue;

            HttpConnection<Socket, RxSize, TxSize> conn(static_cast<Socket&&>(sock));
            conn.process(routes_);
            conn.close();
        }
    }

    // Single-step version for cooperative RTOS schedulers or poll loops
    template <typename Acceptor>
        requires AcceptorConcept<Acceptor, Socket>
    void poll(Acceptor& acceptor) noexcept {
        Socket sock = acceptor.accept();
        if (!sock.is_valid()) return;

        HttpConnection<Socket, RxSize, TxSize> conn(static_cast<Socket&&>(sock));
        conn.process(routes_);
        conn.close();
    }

    const RouteTable<MaxRoutes>& routes() const noexcept { return routes_; }

private:
    RouteTable<MaxRoutes> routes_;
};

} // namespace uchttp
