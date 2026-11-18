# Core types

Source: `include/uchttp.h`, `src/uchttp.cpp`. [← index](README.md)

```mermaid
classDiagram
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

    class SocketBase~Derived~ {
        <<CRTP base, optional>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
        #SocketBase()
    }

    class MethodHelpers {
        <<free functions>>
        +llhttp_method_to_uchttp(uint8_t) HttpMethod
        +method_string(HttpMethod) const char*
    }

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
        +QueryString()
        +QueryString(raw : string_view)
        +begin() iterator
        +end() iterator
        +get(key) string_view
        +has(key) bool
        +empty() bool
        +raw() string_view
        -raw_ : string_view
    }

    class QueryPair["QueryString::Pair"] {
        <<struct>>
        +key : string_view
        +value : string_view
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
        +http_major : uint8_t
        +http_minor : uint8_t
        +keep_alive : bool
        +chunked : bool
        +content_length : uint64_t
        +get_header(name) string_view
        +host() string_view
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
        +set_content_encoding(enc) void
        +set_body(body) void
        +set_keep_alive(v) void
        +keep_alive() bool
        +status() int
        +body() string_view
        +content_type() string_view
        +bind_stream(fn, ctx) void
        +write_raw(data) int
        +hijacked() bool
        +begin_sse() bool
        +send_event(data, event, id) bool
        +scratch() span~char~
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
        -content_encoding_ : string_view
        -body_ : string_view
        -keep_alive_ : bool
        -hdr_count_ : uint8_t
        -resp_headers_ : HttpHeader[UCHTTP_MAX_HEADERS]
        -stream_write_ : RawWriteFn
        -stream_ctx_ : void*
        -hijacked_ : bool
        -scratch_ : char[32]
    }

    class RawWriteFn {
        <<function pointer>>
        int(void* ctx, const uint8_t* data, size_t len)
    }

    class HandlerFn {
        <<function pointer>>
        void(const HttpRequest&, HttpResponse&)
    }

    class StreamHandlerFn {
        <<function pointer>>
        void(req, res, chunk, offset, last)
    }

    class Route {
        <<struct>>
        +method : HttpMethod
        +path : string_view
        +handler : HandlerFn
        +stream : StreamHandlerFn
        +ws : const ws_Handlers*
    }

    class WsHandlers["ws::Handlers"] {
        <<see 04_websocket>>
    }

    SocketConcept <.. SocketBase : helper for
    HttpRequest *-- HttpHeader : 0..N
    HttpRequest *-- PathParam : 0..N
    HttpRequest --> HttpMethod
    MethodHelpers ..> HttpMethod
    HttpRequest ..> QueryString : creates (lazy)
    QueryString ..> QueryPair : iterates
    HttpResponse *-- HttpHeader : 0..N
    HttpResponse o-- RawWriteFn : streaming / SSE
    HttpResponse ..> SocketConcept : send via
    Route --> HttpMethod
    Route o-- HandlerFn
    Route o-- StreamHandlerFn : OTA
    Route o-- WsHandlers : WebSocket endpoint
```

## Notes

- `HttpRequest` holds only `string_view`s that point into the RX buffer (zero-copy). The parser fills the protocol metadata (`keep_alive`, `chunked`, `content_length`, `http_major/minor`) in `on_headers_complete`.
- `HttpResponse` only holds views (`add_header` does not copy). Values generated at run time go into `scratch()`.
- A `Route` is one of three kinds: normal (`handler`), streaming/OTA (`stream`) or WebSocket (`ws`). Exactly one of these fields is non-null.
- A streaming handler (`StreamHandlerFn`) receives the body in consecutive chunks as views into the RX buffer. `last == true` marks the final call.
- `write_raw` / `begin_sse` / `send_event` "hijack" the response (`hijacked()`), so the engine does not send its own buffered reply.
- `SocketBase<Derived>` is an optional CRTP helper (its methods forward to `read_impl` / `write_impl` / `close_impl` of the derived class). None of the bundled ports uses it. `SocketConcept` is satisfied directly.
- `QueryString::Pair` is a nested type (`key`, `value`). The iterator is forward-only and does not allocate.
- `UCHTTP_MAX_HEADERS` (default 16) and `UCHTTP_MAX_PATH_PARAMS` (default 8) set the array sizes in `HttpRequest` / `HttpResponse`. The full list of configuration macros is in [../README.md](../README.md#compile-time-configuration).
