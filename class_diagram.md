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
        UNKNOWN
    }

    class HttpHeader {
        <<struct>>
        +name : string_view
        +value : string_view
    }

    class HttpRequest {
        <<struct>>
        +method : HttpMethod
        +path : string_view
        +body : string_view
        +header_count : uint8_t
        +headers : HttpHeader[UCHTTP_MAX_HEADERS]
        +get_header(name) string_view
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

    class RouteTable~MaxRoutes~ {
        +add(method, path, handler) bool
        +find(method, path) const Route*
        +size() size_t
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
        +process(routes) bool
        +close() void
        -socket_ : Socket
        -state_ : ParseState
        -response_ : HttpResponse
        -parser_ : llhttp_t
        -rx_buf_ : array~uint8_t_RxSize~
        -tx_buf_ : array~char_TxSize~
        -send_error(code, msg) void
    }

    class HttpServer~Socket_MaxRoutes_RxSize_TxSize~ {
        +add_route(method, path, handler) bool
        +get(path, handler) bool
        +post(path, handler) bool
        +put(path, handler) bool
        +run(acceptor) void
        +poll(acceptor) void
        +routes() const RouteTable~MaxRoutes~&
        -routes_ : RouteTable~MaxRoutes~
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
    HttpServer~Socket_MaxRoutes_RxSize_TxSize~ *-- RouteTable~MaxRoutes~ : contains
    HttpConnection~Socket_RxSize_TxSize~ *-- ParseState  : contains
    HttpConnection~Socket_RxSize_TxSize~ *-- HttpResponse : contains
    HttpConnection~Socket_RxSize_TxSize~ *-- llhttp_t     : uses
    ParseState *-- HttpRequest : contains
    HttpRequest *-- HttpHeader : contains 0..*
    HttpResponse *-- HttpHeader : contains 0..*
    RouteTable~MaxRoutes~ *-- Route : contains 0..*
    Route --> HttpMethod : uses
    HttpRequest --> HttpMethod : uses

    %% Usage relationships
    HttpServer~Socket_MaxRoutes_RxSize_TxSize~ ..> HttpConnection~Socket_RxSize_TxSize~ : creates per request
    HttpConnection~Socket_RxSize_TxSize~ ..> RouteTable~MaxRoutes~ : looks up route
    HttpResponse ..> SocketConcept : sends via
    LinuxAcceptor ..> LinuxSocket : produces
    MockAcceptor  ..> MockSocket  : produces
```

## Notes

- **Template parameters** are abbreviated in class names where Mermaid would make the name too long (e.g. `Socket_RxSize_TxSize` instead of `Socket, RxSize, TxSize`).
- `SocketConcept` and `AcceptorConcept` are C++20 concepts, shown as interfaces for diagram clarity.
- `SocketBase<Derived>` is an optional CRTP helper; `LinuxSocket` currently inherits from it.
- `HandlerFn` is a plain function pointer `void(*)(const HttpRequest&, HttpResponse&)` and is embedded in `Route` rather than shown as a separate class.
- `llhttp_t` is the opaque parser struct from the third-party llhttp library.
- All buffers (`rx_buf_`, `tx_buf_`) are stack/static — no heap allocation anywhere in the library.
