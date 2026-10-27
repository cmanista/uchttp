# uchttp — Class Diagram

```mermaid
classDiagram
    %% -----------------------------------------------------------------------
    %% Concepts (shown as interfaces)
    %% -----------------------------------------------------------------------

    class SocketConcept {
        <<concept>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
    }

    class AcceptorConcept {
        <<concept>>
        +accept() Socket
        +is_running() bool
    }

    %% -----------------------------------------------------------------------
    %% CRTP base
    %% -----------------------------------------------------------------------

    class SocketBase~Derived~ {
        <<CRTP base>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
    }

    %% -----------------------------------------------------------------------
    %% Core domain types
    %% -----------------------------------------------------------------------

    class HttpMethod {
        <<enumeration>>
        GET
        POST
        PUT
        DELETE
        PATCH
        HEAD
        OPTIONS
        UNKNOWN
    }

    class HttpHeader {
        <<struct>>
        +name : string_view
        +value : string_view
    }

    class PathParam {
        <<struct>>
        +name : string_view
        +value : string_view
    }

    class QueryString {
        +QueryString(raw : string_view)
        +begin() iterator
        +end() iterator
        +get(key) string_view
        +has(key) bool
        +empty() bool
        +raw() string_view
        -raw_ : string_view
    }

    class HttpRequest {
        <<struct>>
        +method : HttpMethod
        +path : string_view
        +raw_query : string_view
        +body : string_view
        +header_count : uint8_t
        +headers : HttpHeader[UCHTTP_MAX_HEADERS]
        +param_count : uint8_t
        +path_params : PathParam[UCHTTP_MAX_PATH_PARAMS]
        +get_header(name) string_view
        +query() QueryString
        +query_param(key) string_view
        +path_param(name) string_view
        +reset() void
    }

    class HttpResponse {
        +bind(tx_buf : span~char~) void
        +set_status(code, reason) void
        +add_header(name, value) void
        +set_content_type(ct) void
        +set_body(body) void
        +reset() void
        +formatted() string_view
        +send~Socket~(socket) bool
        +send~Socket~(socket, status, body, ct) bool
        -buf_ : char*
        -cap_ : size_t
        -len_ : size_t
        -sent_ : bool
        -status_code_ : int
        -status_reason_ : string_view
        -content_type_ : string_view
        -body_ : string_view
        -hdr_count_ : uint8_t
        -resp_headers_ : HttpHeader[UCHTTP_MAX_HEADERS]
    }

    class Route {
        <<struct>>
        +method : HttpMethod
        +path : string_view
        +handler : HandlerFn
    }

    class MiddlewareChain {
        +MiddlewareChain(mws, count, handler)
        +next(req, res) void
        +run(req, res) void
        -mws_ : const MiddlewareFn*
        -count_ : size_t
        -handler_ : HandlerFn
        -index_ : size_t
        -handler_called_ : bool
    }

    class RouteTable~MaxRoutes~ {
        +add(method, path, handler) bool
        +find(method, path) const Route*
        +find(method, path, req) const Route*
        +path_exists(path) bool
        +size() size_t
        +operator[](i) const Route&
        -routes_ : Route[MaxRoutes]
        -count_ : size_t
    }

    class ParseState {
        <<struct>>
        +request : HttpRequest
        +hdr_field_start : const char*
        +hdr_field_len : size_t
        +hdr_value_start : const char*
        +hdr_value_len : size_t
        +in_header_value : bool
        +message_complete : bool
        +parse_error : bool
        +reset() void
        +commit_header() void
    }

    class HttpConnection~Socket_RxSize_TxSize~ {
        +HttpConnection(socket)
        +process(routes, middleware, mw_count) bool
        +close() void
        -socket_ : Socket
        -state_ : ParseState
        -response_ : HttpResponse
        -parser_ : llhttp_t
        -rx_buf_ : array~uint8_t_RxSize~
        -tx_buf_ : array~char_TxSize~
        -send_error(code, msg) void
    }

    class HttpServer~Socket_MaxRoutes_RxSize_TxSize_MaxMiddleware~ {
        +add_route(method, path, handler) bool
        +get(path, handler) bool
        +post(path, handler) bool
        +put(path, handler) bool
        +del(path, handler) bool
        +patch(path, handler) bool
        +head(path, handler) bool
        +options(path, handler) bool
        +use(mw) bool
        +run(acceptor) void
        +poll(acceptor) void
        +routes() const RouteTable~MaxRoutes~&
        +middleware_count() size_t
        -routes_ : RouteTable~MaxRoutes~
        -middleware_ : MiddlewareFn[MaxMiddleware]
        -mw_count_ : size_t
    }

    %% -----------------------------------------------------------------------
    %% Linux (POSIX) implementations
    %% -----------------------------------------------------------------------

    class LinuxSocket {
        +LinuxSocket(fd)
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
        +is_valid() bool
        +fd() int
        -fd_ : int
    }

    class LinuxAcceptor {
        +LinuxAcceptor(port, backlog)
        +start() bool
        +stop() void
        +accept() LinuxSocket
        +is_running() bool
        -port_ : uint16_t
        -backlog_ : int
        -listen_fd_ : int
        -running_ : bool
    }

    %% -----------------------------------------------------------------------
    %% Test doubles
    %% -----------------------------------------------------------------------

    class MockSocket {
        +MockSocket(rx_data)
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
        +is_valid() bool
        +was_closed() bool
        +tx_view() string_view
        +tx_string() string
        +tx_status_line() string_view
        +tx_body() string_view
        +reset_tx() void
        +push_rx(more) void
        -rx_ : string
        -rx_pos_ : size_t
        -tx_ : string
        -closed_ : bool
    }

    class MockAcceptor {
        +MockAcceptor(rx_data)
        +accept() MockSocket
        +is_running() bool
        +was_accepted() bool
        -rx_data_ : string
        -accepted_ : bool
    }

    %% -----------------------------------------------------------------------
    %% External dependency
    %% -----------------------------------------------------------------------

    class llhttp_t {
        <<external>>
    }

    %% -----------------------------------------------------------------------
    %% Relationships
    %% -----------------------------------------------------------------------

    %% Concept satisfaction (realization)
    SocketConcept <|.. LinuxSocket : satisfies
    SocketConcept <|.. MockSocket  : satisfies
    AcceptorConcept <|.. LinuxAcceptor : satisfies
    AcceptorConcept <|.. MockAcceptor  : satisfies

    %% CRTP inheritance
    SocketBase~Derived~ <|-- LinuxSocket : extends (CRTP)

    %% Composition / aggregation
    HttpServer~Socket_MaxRoutes_RxSize_TxSize_MaxMiddleware~ *-- RouteTable~MaxRoutes~ : contains
    HttpServer~Socket_MaxRoutes_RxSize_TxSize_MaxMiddleware~ *-- MiddlewareFn : holds 0..*
    HttpConnection~Socket_RxSize_TxSize~ *-- ParseState  : contains
    HttpConnection~Socket_RxSize_TxSize~ *-- HttpResponse : contains
    HttpConnection~Socket_RxSize_TxSize~ *-- llhttp_t     : uses
    ParseState *-- HttpRequest : contains
    HttpRequest *-- HttpHeader : contains 0..*
    HttpRequest *-- PathParam : contains 0..*
    HttpRequest ..> QueryString : creates (lazy)
    HttpResponse *-- HttpHeader : contains 0..*
    RouteTable~MaxRoutes~ *-- Route : contains 0..*
    Route --> HttpMethod : uses
    HttpRequest --> HttpMethod : uses

    %% Usage relationships
    HttpServer~Socket_MaxRoutes_RxSize_TxSize_MaxMiddleware~ ..> HttpConnection~Socket_RxSize_TxSize~ : creates per request
    HttpConnection~Socket_RxSize_TxSize~ ..> RouteTable~MaxRoutes~ : looks up route
    HttpConnection~Socket_RxSize_TxSize~ ..> MiddlewareChain : builds & runs
    MiddlewareChain ..> HandlerFn : invokes final
    HttpResponse ..> SocketConcept : sends via
    LinuxAcceptor ..> LinuxSocket : produces
    MockAcceptor  ..> MockSocket  : produces
```

## Notes

- **Template parameters** are abbreviated in class names where Mermaid would make the name too long (e.g. `Socket_RxSize_TxSize` instead of `Socket, RxSize, TxSize`).
- `SocketConcept` and `AcceptorConcept` are C++20 concepts, shown as interfaces for diagram clarity.
- `SocketBase<Derived>` is an optional CRTP helper; `LinuxSocket` currently inherits from it.
- `HandlerFn` is a plain function pointer `void(*)(const HttpRequest&, HttpResponse&)` and is embedded in `Route` rather than shown as a separate class.
- `MiddlewareFn` is a plain function pointer `void(*)(const HttpRequest&, HttpResponse&, MiddlewareChain&)`. Middleware form a Chain of Responsibility: `MiddlewareChain::next()` advances to the next middleware or invokes the final route handler ("onion" model — code after `next()` runs on the way out). `HttpServer` holds a fixed array of up to `MaxMiddleware` (`UCHTTP_MAX_MIDDLEWARE`) function pointers registered via `use()`.
- `QueryString` is an allocation-free, lazy iterator over `key=value&...` pairs from `HttpRequest::raw_query`; it is created on demand by `HttpRequest::query()` and owns no storage beyond a `string_view`.
- `PathParam` holds a captured `{name}` URL segment as zero-copy `string_view`s into the RX buffer; `RouteTable::find(method, path, req)` fills `HttpRequest::path_params` during pattern matching.
- `llhttp_t` is the opaque parser struct from the third-party llhttp library.
- All buffers (`rx_buf_`, `tx_buf_`) and tables (routes, middleware, headers, path params) are stack/static — no heap allocation anywhere in the library.
