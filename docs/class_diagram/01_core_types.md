# Core types

Źródło: `include/uchttp.h`, `src/uchttp.cpp`. [← indeks](../../class_diagram.md)

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
        <<CRTP base>>
        +read(span~uint8_t~) int
        +write(span~const uint8_t~) int
        +close() void
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
        +QueryString(raw : string_view)
        +begin() iterator
        +end() iterator
        +get(key) string_view
        +has(key) bool
        +empty() bool
        +raw() string_view
        -raw_ : string_view
    }

    class QueryPair {
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

## Uwagi

- `HttpRequest` zawiera wyłącznie `string_view` wskazujące w bufor RX (zero-copy). Metadane protokołu (`keep_alive`, `chunked`, `content_length`, `http_major/minor`) wypełnia parser w `on_headers_complete`.
- `HttpResponse` tylko trzyma widoki (`add_header` nie kopiuje) — wartości generowane w czasie działania trafiają do `scratch()`.
- `Route` ma trzy rodzaje: zwykły (`handler`), strumieniowy/OTA (`stream`) lub WebSocket (`ws`); dokładnie jedno pole jest niepuste.
- Handler strumieniowy (`StreamHandlerFn`) dostaje kolejne kawałki body jako widoki w bufor RX; `last == true` przy ostatnim.
- `write_raw` / `begin_sse` / `send_event` "przejmują" odpowiedź (`hijacked()`), więc silnik nie wysyła już własnej, buforowanej.
- `SocketBase<Derived>` to opcjonalny helper CRTP; `SocketConcept` jest spełniany także bez niego.
