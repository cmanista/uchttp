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
#ifndef UCHTTP_MAX_PATH_PARAMS
#  define UCHTTP_MAX_PATH_PARAMS 8
#endif
#ifndef UCHTTP_MAX_MIDDLEWARE
#  define UCHTTP_MAX_MIDDLEWARE 8
#endif
// Connection pool size (requirement #6): max simultaneously served clients.
#ifndef UCHTTP_MAX_CONNECTIONS
#  define UCHTTP_MAX_CONNECTIONS 4
#endif
#ifndef UCHTTP_RX_BUFFER_SIZE
#  define UCHTTP_RX_BUFFER_SIZE 1024
#endif
#ifndef UCHTTP_TX_BUFFER_SIZE
#  define UCHTTP_TX_BUFFER_SIZE 1024
#endif

// --- DoS / Slowloris protection (requirement #4) ---------------------------
// Maximum bytes accepted for the request header block and the request body.
// They guard the fixed-size RX buffer against overflow / resource exhaustion.
#ifndef UCHTTP_MAX_HEADER_BYTES
#  define UCHTTP_MAX_HEADER_BYTES 2048
#endif
#ifndef UCHTTP_MAX_BODY_BYTES
#  define UCHTTP_MAX_BODY_BYTES   4096
#endif
// Idle read timeout (milliseconds) for a connection; 0 disables it. Enforced by
// sockets that implement set_read_timeout(); see LinuxSocket.
#ifndef UCHTTP_IDLE_TIMEOUT_MS
#  define UCHTTP_IDLE_TIMEOUT_MS  5000
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

enum class HttpMethod : uint8_t { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, UNKNOWN };

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
// QueryString  – zero-alloc, zero-copy iterator over "key=value&key2=value2"
//
// Holds a string_view into the RX buffer (the part after '?' in the URL) and
// lazily iterates over key/value pairs. No heap, no owning storage.
// ---------------------------------------------------------------------------

class QueryString {
public:
    struct Pair {
        std::string_view key;
        std::string_view value;
    };

    constexpr QueryString() noexcept = default;
    constexpr explicit QueryString(std::string_view raw) noexcept : raw_(raw) {}

    // Forward iterator over key/value pairs ---------------------------------
    class iterator {
    public:
        using value_type        = Pair;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        constexpr iterator() noexcept = default;
        constexpr explicit iterator(std::string_view rest) noexcept
            : rest_(rest) { advance(); }

        constexpr const Pair& operator*()  const noexcept { return current_; }
        constexpr const Pair* operator->() const noexcept { return &current_; }

        constexpr iterator& operator++() noexcept { advance(); return *this; }

        constexpr bool operator==(const iterator& o) const noexcept {
            return at_end_ == o.at_end_ &&
                   (at_end_ || rest_.data() == o.rest_.data());
        }
        constexpr bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }

    private:
        constexpr void advance() noexcept {
            if (rest_.empty()) { at_end_ = true; return; }
            at_end_ = false;

            // take one segment up to '&'
            std::string_view seg;
            auto amp = rest_.find('&');
            if (amp == std::string_view::npos) {
                seg  = rest_;
                rest_ = {};
            } else {
                seg  = rest_.substr(0, amp);
                rest_ = rest_.substr(amp + 1);
            }

            // split segment on first '='
            auto eq = seg.find('=');
            if (eq == std::string_view::npos) {
                current_ = { seg, {} };
            } else {
                current_ = { seg.substr(0, eq), seg.substr(eq + 1) };
            }
        }

        std::string_view rest_{};
        Pair             current_{};
        bool             at_end_ { true };
    };

    constexpr iterator begin() const noexcept { return iterator(raw_); }
    constexpr iterator end()   const noexcept { return iterator(); }

    constexpr bool             empty() const noexcept { return raw_.empty(); }
    constexpr std::string_view raw()   const noexcept { return raw_; }

    // Returns value for the first matching key, or {} if absent.
    constexpr std::string_view get(std::string_view key) const noexcept {
        for (auto it = begin(); it != end(); ++it) {
            if (it->key == key) return it->value;
        }
        return {};
    }

    // True if the key is present (even with an empty value, e.g. "?flag").
    constexpr bool has(std::string_view key) const noexcept {
        for (auto it = begin(); it != end(); ++it) {
            if (it->key == key) return true;
        }
        return false;
    }

private:
    std::string_view raw_{};
};

// ---------------------------------------------------------------------------
// PathParam  – one extracted dynamic URL segment, e.g. {id} -> "42"
// ---------------------------------------------------------------------------

struct PathParam {
    std::string_view name;   // template name, e.g. "id"
    std::string_view value;  // zero-copy view into RX buffer, e.g. "42"
};

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

struct HttpRequest {
    HttpMethod       method { HttpMethod::UNKNOWN };
    std::string_view path;       // path only, query string stripped
    std::string_view raw_query;  // part after '?', without the '?'
    std::string_view body;

    uint8_t    header_count { 0 };
    HttpHeader headers[UCHTTP_MAX_HEADERS];

    uint8_t    param_count { 0 };
    PathParam  path_params[UCHTTP_MAX_PATH_PARAMS];

    // Protocol metadata handled by the engine (requirement #3). Filled in by
    // the parser at on_headers_complete. These are NOT visible to middleware
    // as application state — they govern how the sockets physically talk.
    uint8_t  http_major    { 1 };      // HTTP/<major>.<minor>
    uint8_t  http_minor    { 1 };
    bool     keep_alive    { false };  // llhttp_should_keep_alive() result
    bool     chunked       { false };  // Transfer-Encoding: chunked present
    uint64_t content_length { 0 };     // declared Content-Length (0 if absent)

    std::string_view get_header(std::string_view name) const noexcept;

    // Protocol accessors (defined in uchttp.cpp) ----------------------------
    // Host header — used by the server for (virtual-host) routing.
    std::string_view host() const noexcept;

    // Query string accessors (defined in uchttp.cpp) ------------------------
    QueryString      query() const noexcept;
    std::string_view query_param(std::string_view key) const noexcept;

    // Path parameter accessor (defined in uchttp.cpp) -----------------------
    std::string_view path_param(std::string_view name) const noexcept;

    void reset() noexcept;
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
    void set_content_encoding(std::string_view enc) noexcept;
    void set_body(std::string_view body) noexcept;

    // Connection lifecycle (requirement #3). The engine sets this from the
    // request's keep-alive state before the handler runs; send() emits the
    // matching "Connection: keep-alive" / "Connection: close" header.
    void set_keep_alive(bool v) noexcept;
    bool keep_alive() const noexcept;

    // Read-back accessors (used by response middleware, e.g. ETag/304).
    // Defined in uchttp.cpp.
    int              status() const noexcept;
    std::string_view body()  const noexcept;
    std::string_view content_type() const noexcept;

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

        if (!content_encoding_.empty()) {
            append("Content-Encoding: ");
            append(content_encoding_);
            append("\r\n");
        }

        // Connection header is owned by the engine (requirement #3): tell the
        // client whether we will keep the socket open or close it.
        append(keep_alive_ ? "Connection: keep-alive\r\n"
                           : "Connection: close\r\n");

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
    std::string_view content_encoding_;
    std::string_view body_;
    bool             keep_alive_ { false };

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
// Middleware (Chain of Responsibility)
//
// A middleware is a plain function pointer:
//
//     void mw(const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain);
//
// It may inspect/modify the request and response, then call `chain.next()` to
// pass control to the next middleware (and finally the route handler). A
// middleware that does NOT call next() short-circuits the chain (e.g. an auth
// middleware returning 401). Because next() is called *inside* the middleware,
// code placed after the next() call runs on the way back out — ideal for
// response-header injection (CORS/CSP), timing and logging.
//
// Zero heap: the chain is driven iteratively over a caller-owned array of
// function pointers; no std::function, no allocation, no virtuals.
// ---------------------------------------------------------------------------

class MiddlewareChain;  // forward decl

using MiddlewareFn = void (*)(const HttpRequest&, HttpResponse&, MiddlewareChain&);

class MiddlewareChain {
public:
    MiddlewareChain(const MiddlewareFn* mws, size_t count,
                    HandlerFn handler) noexcept;

    // Advance to the next middleware, or invoke the final route handler once
    // all middleware have run. Safe to call at most once per middleware.
    // Non-template, called once per middleware/request — body in uchttp.cpp.
    void next(const HttpRequest& req, HttpResponse& res) noexcept;

    // Kick off the chain from the beginning.
    void run(const HttpRequest& req, HttpResponse& res) noexcept;

private:
    const MiddlewareFn* mws_   { nullptr };
    size_t              count_ { 0 };
    HandlerFn           handler_ { nullptr };
    size_t              index_ { 0 };
    bool                handler_called_ { false };
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

    // Exact-match lookup (kept for backward compatibility / simple cases).
    const Route* find(HttpMethod method, std::string_view path) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (routes_[i].method == method && routes_[i].path == path)
                return &routes_[i];
        }
        return nullptr;
    }

    // Pattern-aware lookup. On a match of a templated route (e.g.
    // "/api/sensors/{id}") the extracted parameters are written into `req`.
    // Static segments must match literally; "{name}" segments match any single
    // non-empty segment and capture its value as path param "name".
    const Route* find(HttpMethod method, std::string_view path,
                      HttpRequest& req) const noexcept {
        // First pass: prefer an exact literal match (fast path, no capture).
        for (size_t i = 0; i < count_; ++i) {
            if (routes_[i].method == method && routes_[i].path == path)
                return &routes_[i];
        }
        // Second pass: templated patterns.
        for (size_t i = 0; i < count_; ++i) {
            if (routes_[i].method != method) continue;
            if (!is_pattern(routes_[i].path)) continue;
            req.param_count = 0;
            if (match_pattern(routes_[i].path, path, req))
                return &routes_[i];
        }
        req.param_count = 0;
        return nullptr;
    }

    // Returns true if any route (any method) matches the path literally or by
    // pattern. Used to distinguish 404 (no such resource) from 405 (wrong
    // method) and to build the Allow header for OPTIONS.
    bool path_exists(std::string_view path) const noexcept {
        HttpRequest scratch;
        for (size_t i = 0; i < count_; ++i) {
            if (routes_[i].path == path) return true;
            if (is_pattern(routes_[i].path)) {
                scratch.param_count = 0;
                if (match_pattern(routes_[i].path, path, scratch)) return true;
            }
        }
        return false;
    }

    size_t size() const noexcept { return count_; }

    const Route& operator[](size_t i) const noexcept { return routes_[i]; }

private:
    // A pattern contains at least one "{...}" segment.
    static bool is_pattern(std::string_view p) noexcept {
        return p.find('{') != std::string_view::npos;
    }

    // Split helper: returns the segment before the next '/' and advances `s`
    // past it. Leading '/' is skipped by the caller.
    static std::string_view next_segment(std::string_view& s) noexcept {
        auto slash = s.find('/');
        if (slash == std::string_view::npos) {
            std::string_view seg = s;
            s = {};
            return seg;
        }
        std::string_view seg = s.substr(0, slash);
        s = s.substr(slash + 1);
        return seg;
    }

    // Segment-by-segment match of `pattern` against `path`, capturing any
    // "{name}" segments into req.path_params. No allocation.
    static bool match_pattern(std::string_view pattern, std::string_view path,
                              HttpRequest& req) noexcept {
        // Both must start with '/' to be comparable segment-wise.
        if (!pattern.empty() && pattern.front() == '/') pattern.remove_prefix(1);
        if (!path.empty()    && path.front()    == '/') path.remove_prefix(1);

        while (!pattern.empty() || !path.empty()) {
            if (pattern.empty() || path.empty()) return false;  // length mismatch

            std::string_view pseg = next_segment(pattern);
            std::string_view vseg = next_segment(path);

            if (pseg.size() >= 2 && pseg.front() == '{' && pseg.back() == '}') {
                if (vseg.empty()) return false;  // dynamic segment must be non-empty
                if (req.param_count >= UCHTTP_MAX_PATH_PARAMS) return false;
                std::string_view name = pseg.substr(1, pseg.size() - 2);
                req.path_params[req.param_count++] = { name, vseg };
            } else if (pseg != vseg) {
                return false;
            }
        }
        return true;
    }

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

    // DoS protection (requirement #4): byte counters and oversize flags filled
    // by the parser callbacks; the engine turns them into 431 / 413 responses.
    size_t header_bytes   { 0 };
    size_t body_bytes     { 0 };
    bool   headers_too_large { false };
    bool   body_too_large    { false };

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

    // Default-constructed, idle connection — used by ConnectionPool, which
    // pre-allocates the slots once and assigns a socket per client.
    HttpConnection() noexcept {
        llhttp_init(&parser_, HTTP_REQUEST, &make_llhttp_settings());
        parser_.data = &state_;
    }

    // Rebind this (pooled) connection to a freshly accepted socket.
    void assign(Socket socket) noexcept {
        socket_ = static_cast<Socket&&>(socket);
        llhttp_reset(&parser_);
    }

    bool is_valid() const noexcept { return socket_.is_valid(); }

    HttpConnection(const HttpConnection&)            = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
    HttpConnection(HttpConnection&&)                 = default;

    template <size_t MaxRoutes>
    bool process(const RouteTable<MaxRoutes>& routes,
                 const MiddlewareFn* middleware = nullptr,
                 size_t middleware_count = 0) noexcept {
        // One llhttp parser instance drives the whole connection; it natively
        // supports multiple messages back-to-back. on_message_complete returns
        // HPE_PAUSED so a single read() carrying the tail of one request and
        // the head of the next does not clobber the current request's state.
        llhttp_reset(&parser_);

        bool   handled_any = false;
        size_t buffered    = 0;   // valid bytes currently in rx_buf_

        // Keep-alive loop (requirement #3): serve consecutive requests on the
        // same socket until the client/engine decides to close it.
        while (true) {
            state_.reset();

            // --- read + parse exactly one complete request -----------------
            size_t consumed = 0;   // bytes of rx_buf_ consumed by this request

            while (!state_.message_complete && !state_.parse_error) {
                if (consumed < buffered) {
                    const char* at = reinterpret_cast<const char*>(
                        rx_buf_.data() + consumed);
                    size_t avail = buffered - consumed;

                    llhttp_errno_t err = llhttp_execute(&parser_, at, avail);

                    if (err == HPE_PAUSED) {
                        // message_complete paused us; compute bytes used by
                        // this request, then resume for the next one.
                        const char* stop = llhttp_get_error_pos(&parser_);
                        consumed += static_cast<size_t>(
                            reinterpret_cast<const uint8_t*>(stop)
                            - (rx_buf_.data() + consumed));
                        llhttp_resume(&parser_);
                        break;
                    }
                    if (err != HPE_OK) {
                        state_.parse_error = true;
                        break;
                    }
                    consumed = buffered;   // consumed everything buffered
                }

                // Need more data: compact any leftover to the front, then read.
                if (consumed > 0) {
                    size_t rest = buffered - consumed;
                    if (rest > 0)
                        __builtin_memmove(rx_buf_.data(),
                                          rx_buf_.data() + consumed, rest);
                    buffered = rest;
                    consumed = 0;
                }
                if (buffered >= RxSize) {
                    state_.parse_error = true;   // request exceeds static buffer
                    break;
                }
                auto buf = std::span<uint8_t>(rx_buf_.data() + buffered,
                                              RxSize - buffered);
                int n = socket_.read(buf);
                if (n <= 0) break;               // EOF / error
                buffered += static_cast<size_t>(n);
            }

            if (state_.headers_too_large) {
                // DoS protection (requirement #4): header block too large.
                send_error(431, "Request Header Fields Too Large", false);
                return handled_any;
            }
            if (state_.body_too_large) {
                send_error(413, "Content Too Large", false);
                return handled_any;
            }
            if (state_.parse_error) {
                // Malformed syntax: engine answers 400 before any middleware.
                send_error(400, "Bad Request", false);
                return handled_any;        // cannot trust the stream -> close
            }
            if (!state_.message_complete) {
                // Clean EOF (client closed) or no data -> nothing more to do.
                return handled_any;
            }

            handled_any = true;

            // --- dispatch --------------------------------------------------
            const bool keep = state_.request.keep_alive;

            response_.bind(std::span<char>(tx_buf_.data(), tx_buf_.size()));
            response_.reset();
            response_.set_keep_alive(keep);

            if (state_.request.method == HttpMethod::UNKNOWN) {
                // Parser accepted the syntax but the method is one we do not
                // implement (e.g. TRACE/CONNECT) -> 501 Not Implemented.
                send_error(501, "Not Implemented", keep);
            } else {
                const Route* route = routes.find(state_.request.method,
                                                  state_.request.path,
                                                  state_.request);
                if (route && route->handler) {
                    MiddlewareChain chain(middleware, middleware_count,
                                          route->handler);
                    chain.run(state_.request, response_);
                    (void)response_.send(socket_);
                } else if (routes.path_exists(state_.request.path)) {
                    send_error(405, "Method Not Allowed", keep);
                } else {
                    send_error(404, "Not Found", keep);
                }
            }

            // --- connection lifecycle -------------------------------------
            if (!keep) {
                break;   // Connection: close (or HTTP/1.0 without keep-alive)
            }
            // Compact any pipelined leftover to the front, then serve it next.
            if (consumed > 0) {
                size_t rest = buffered - consumed;
                if (rest > 0)
                    __builtin_memmove(rx_buf_.data(),
                                      rx_buf_.data() + consumed, rest);
                buffered = rest;
            }
        }

        return handled_any;
    }

    void close() noexcept { socket_.close(); }

private:
    Socket       socket_;
    ParseState   state_;
    HttpResponse response_;
    llhttp_t     parser_{};

    std::array<uint8_t, RxSize> rx_buf_{};
    std::array<char,    TxSize> tx_buf_{};

    void send_error(int code, std::string_view msg, bool keep) noexcept {
        response_.bind(std::span<char>(tx_buf_.data(), tx_buf_.size()));
        response_.reset();
        response_.set_keep_alive(keep);
        response_.send(socket_, code, msg, "text/plain");
    }
};

// ---------------------------------------------------------------------------
// ConnectionPool  – fixed, pre-allocated array of connection slots
// (requirement #6). No heap: all slots, their RX/TX buffers and parser state
// are allocated once up front. acquire() hands out an idle slot (or nullptr
// when the pool is exhausted), release() returns one, and close_all() performs
// a graceful shutdown of every active slot (e.g. on stack reset / reboot).
// ---------------------------------------------------------------------------

template <SocketConcept Socket,
          size_t MaxConns = UCHTTP_MAX_CONNECTIONS,
          size_t RxSize   = UCHTTP_RX_BUFFER_SIZE,
          size_t TxSize   = UCHTTP_TX_BUFFER_SIZE>
class ConnectionPool {
public:
    using Connection = HttpConnection<Socket, RxSize, TxSize>;

    ConnectionPool() = default;
    ConnectionPool(const ConnectionPool&)            = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    static constexpr size_t capacity() noexcept { return MaxConns; }
    size_t active() const noexcept { return active_; }
    bool   full()   const noexcept { return active_ >= MaxConns; }

    // Grab an idle slot and bind `sock` to it. Returns nullptr if the pool is
    // exhausted (caller should drop/refuse the connection).
    Connection* acquire(Socket&& sock) noexcept {
        for (size_t i = 0; i < MaxConns; ++i) {
            if (!in_use_[i]) {
                in_use_[i] = true;
                ++active_;
                slots_[i].assign(static_cast<Socket&&>(sock));
                return &slots_[i];
            }
        }
        return nullptr;
    }

    // Close the slot's socket and return it to the pool.
    void release(Connection* conn) noexcept {
        for (size_t i = 0; i < MaxConns; ++i) {
            if (&slots_[i] == conn) {
                if (in_use_[i]) {
                    slots_[i].close();
                    in_use_[i] = false;
                    --active_;
                }
                return;
            }
        }
    }

    // Graceful shutdown: close every active connection and free all slots.
    void close_all() noexcept {
        for (size_t i = 0; i < MaxConns; ++i) {
            if (in_use_[i]) {
                slots_[i].close();
                in_use_[i] = false;
            }
        }
        active_ = 0;
    }

private:
    Connection slots_[MaxConns] {};
    bool       in_use_[MaxConns] {};
    size_t     active_ { 0 };
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
          size_t TxSize    = UCHTTP_TX_BUFFER_SIZE,
          size_t MaxMiddleware = UCHTTP_MAX_MIDDLEWARE,
          size_t MaxConns      = UCHTTP_MAX_CONNECTIONS>
class HttpServer {
public:
    HttpServer() = default;

    // Register a global middleware. Middleware run in registration order,
    // before the matched route handler. Returns false if the table is full.
    bool use(MiddlewareFn mw) noexcept {
        if (mw_count_ >= MaxMiddleware) return false;
        middleware_[mw_count_++] = mw;
        return true;
    }

    bool add_route(HttpMethod method, std::string_view path,
                   HandlerFn handler) noexcept {
        return routes_.add(method, path, handler);
    }

    bool get (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::GET,     path, h); }
    bool post(std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::POST,    path, h); }
    bool put (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::PUT,     path, h); }
    bool del (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::DELETE,  path, h); }
    bool patch  (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::PATCH,   path, h); }
    bool head   (std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::HEAD,    path, h); }
    bool options(std::string_view path, HandlerFn h) noexcept { return add_route(HttpMethod::OPTIONS, path, h); }

    template <typename Acceptor>
        requires AcceptorConcept<Acceptor, Socket>
    void run(Acceptor& acceptor) noexcept {
        running_ = true;
        while (acceptor.is_running() && running_) {
            Socket sock = acceptor.accept();
            if (!sock.is_valid()) continue;

            // Acquire a pre-allocated slot from the fixed pool (requirement #6).
            auto* conn = pool_.acquire(static_cast<Socket&&>(sock));
            if (!conn) {
                // Pool exhausted: refuse rather than allocate. The socket is
                // dropped (closed by its destructor) so the client backs off.
                continue;
            }
            conn->process(routes_, middleware_, mw_count_);
            pool_.release(conn);
        }
        // Graceful shutdown: close anything still active on the way out.
        pool_.close_all();
    }

    template <typename Acceptor>
        requires AcceptorConcept<Acceptor, Socket>
    void poll(Acceptor& acceptor) noexcept {
        Socket sock = acceptor.accept();
        if (!sock.is_valid()) return;

        auto* conn = pool_.acquire(static_cast<Socket&&>(sock));
        if (!conn) return;                 // pool full -> drop
        conn->process(routes_, middleware_, mw_count_);
        pool_.release(conn);
    }

    // Request the run() loop to stop and gracefully close active connections.
    void stop() noexcept { running_ = false; }

    // Immediately close every active connection in the pool (graceful shutdown
    // on stack reset / reboot). Safe to call from a signal/other context.
    void shutdown() noexcept { pool_.close_all(); }

    const RouteTable<MaxRoutes>& routes() const noexcept { return routes_; }

    size_t middleware_count() const noexcept { return mw_count_; }

    // Pool introspection (requirement #6).
    size_t active_connections() const noexcept { return pool_.active(); }
    static constexpr size_t max_connections() noexcept { return MaxConns; }

private:
    RouteTable<MaxRoutes>                            routes_;
    MiddlewareFn                                     middleware_[MaxMiddleware] {};
    size_t                                           mw_count_ { 0 };
    ConnectionPool<Socket, MaxConns, RxSize, TxSize> pool_;
    bool                                             running_ { false };
};

} // namespace uchttp
