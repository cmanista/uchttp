#pragma once

#include <cstddef>
#include <cstdint>
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
// SocketConcept
// ---------------------------------------------------------------------------

template <typename T>
concept SocketConcept = requires(T& s,
                                  std::span<uint8_t>       rx,
                                  std::span<const uint8_t> tx) {
    { s.read(rx)  } -> std::same_as<int>;
    { s.write(tx) } -> std::same_as<int>;
    { s.close()   } -> std::same_as<void>;
};

// CRTP base – optional
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

// Defined in uchttp.cpp
HttpMethod       llhttp_method_to_uchttp(uint8_t m) noexcept;
const char*      method_string(HttpMethod m) noexcept;

// ---------------------------------------------------------------------------
// HttpHeader
// ---------------------------------------------------------------------------

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

struct HttpRequest {
    HttpMethod       method { HttpMethod::UNKNOWN };
    std::string_view path;
    std::string_view body;

    uint8_t    header_count { 0 };
    HttpHeader headers[UCHTTP_MAX_HEADERS];

    std::string_view get_header(std::string_view name) const noexcept;
    void             reset() noexcept;
};

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

class HttpResponse {
public:
    void bind(std::span<char> tx_buf) noexcept;
    void set_status(int code, std::string_view reason = {}) noexcept;
    void add_header(std::string_view name, std::string_view value) noexcept;
    void set_content_type(std::string_view ct) noexcept;
    void set_body(std::string_view body) noexcept;
    void reset() noexcept;

    std::string_view formatted() const noexcept;

    // send() must stay in the header: it is templated on Socket
    template <SocketConcept Socket>
    bool send(Socket& socket) noexcept {
        if (sent_) return false;
        sent_ = true;

        len_ = 0;
        append("HTTP/1.1 ");
        append_int(status_code_);
        append(' ');
        append(status_reason_);
        append("\r\n");

        if (!content_type_.empty()) {
            append("Content-Type: ");
            append(content_type_);
            append("\r\n");
        }

        if (!body_.empty()) {
            append("Content-Length: ");
            append_int(static_cast<int>(body_.size()));
            append("\r\n");
        } else {
            append("Content-Length: 0\r\n");
        }

        for (uint8_t i = 0; i < hdr_count_; ++i) {
            append(resp_headers_[i].name);
            append(": ");
            append(resp_headers_[i].value);
            append("\r\n");
        }

        append("\r\n");

        if (!body_.empty()) {
            append(body_);
        }

        auto data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(buf_), len_);
        return socket.write(data) >= 0;
    }

    template <SocketConcept Socket>
    bool send(Socket& socket, int status, std::string_view body,
              std::string_view content_type = "text/plain") noexcept {
        set_status(status);
        set_content_type(content_type);
        set_body(body);
        return send(socket);
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

    // Helpers called only from the templated send() above – must stay in header
    static const char* default_reason(int code) noexcept;
    void append(std::string_view s) noexcept;
    void append(char c) noexcept;
    void append_int(int v) noexcept;
};

// ---------------------------------------------------------------------------
// Route / HandlerFn
// ---------------------------------------------------------------------------

using HandlerFn = void (*)(const HttpRequest&, HttpResponse&);

struct Route {
    HttpMethod       method  { HttpMethod::UNKNOWN };
    std::string_view path;
    HandlerFn        handler { nullptr };
};

// ---------------------------------------------------------------------------
// RouteTable  – templated on capacity, stays in header
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
            if (routes_[i].method == method && routes_[i].path == path)
                return &routes_[i];
        }
        return nullptr;
    }

    size_t size() const noexcept { return count_; }

private:
    Route  routes_[MaxRoutes] {};
    size_t count_ { 0 };
};

// ---------------------------------------------------------------------------
// ParseState  – declared here, method bodies in uchttp.cpp
// ---------------------------------------------------------------------------

struct ParseState {
    HttpRequest request;

    const char* hdr_field_start { nullptr };
    size_t      hdr_field_len   { 0 };
    const char* hdr_value_start { nullptr };
    size_t      hdr_value_len   { 0 };
    bool        in_header_value { false };

    bool message_complete { false };
    bool parse_error      { false };

    void reset() noexcept;
    void commit_header() noexcept;
};

// ---------------------------------------------------------------------------
// llhttp settings factory – defined in uchttp.cpp (single translation unit,
// no inline-static duplication across TUs)
// ---------------------------------------------------------------------------

const llhttp_settings_t& make_llhttp_settings() noexcept;

// ---------------------------------------------------------------------------
// HttpConnection  – fully templated, stays in header
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

    HttpConnection(const HttpConnection&)            = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
    HttpConnection(HttpConnection&&)                 = default;

    template <size_t MaxRoutes>
    bool process(const RouteTable<MaxRoutes>& routes) noexcept {
        state_.reset();
        llhttp_reset(&parser_);

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

        response_.bind(std::span<char>(tx_buf_.data(), tx_buf_.size()));
        response_.reset();

        const Route* route = routes.find(state_.request.method,
                                          state_.request.path);
        if (route && route->handler) {
            route->handler(state_.request, response_);
            (void)response_.send(socket_);
        } else {
            send_error(404, "Not Found");
        }

        return true;
    }

    void close() noexcept { socket_.close(); }

private:
    Socket       socket_;
    ParseState   state_;
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
// AcceptorConcept
// ---------------------------------------------------------------------------

template <typename T, typename Socket>
concept AcceptorConcept = requires(T& a) {
    { a.accept()     } -> std::same_as<Socket>;
    { a.is_running() } -> std::same_as<bool>;
};

// ---------------------------------------------------------------------------
// HttpServer  – fully templated, stays in header
// ---------------------------------------------------------------------------

template <SocketConcept Socket,
          size_t MaxRoutes = UCHTTP_MAX_ROUTES,
          size_t RxSize    = UCHTTP_RX_BUFFER_SIZE,
          size_t TxSize    = UCHTTP_TX_BUFFER_SIZE>
class HttpServer {
public:
    HttpServer() = default;

    bool add_route(HttpMethod method, std::string_view path,
                   HandlerFn handler) noexcept {
        return routes_.add(method, path, handler);
    }

    bool get (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::GET,  path, h); }
    bool post(std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::POST, path, h); }
    bool put (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::PUT,  path, h); }

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
