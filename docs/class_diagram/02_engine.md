# Engine: server, connections, routing

Źródło: `include/uchttp.h`, `src/uchttp.cpp`. [← indeks](../../class_diagram.md)

```mermaid
classDiagram
    class RouteTable~MaxRoutes~ {
        +add(method, path, handler) bool
        +add_stream(method, path, stream) bool
        +add_ws(path, handlers) bool
        +find(method, path) const Route*
        +find(method, path, req) const Route*
        +path_exists(path) bool
        +size() size_t
        +operator[](i) const Route&
        -routes_ : Route[MaxRoutes]
        -count_ : size_t
        -match_pattern(pattern, path, req) bool$
    }

    class Route {
        <<struct>>
        +method
        +path
        +handler
        +stream
        +ws
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
        +header_bytes : size_t
        +body_bytes : size_t
        +headers_too_large : bool
        +body_too_large : bool
        +stream_resolve : StreamResolveFn
        +stream_ctx : void*
        +stream_handler : StreamHandlerFn
        +stream_response : HttpResponse*
        +stream_offset : size_t
        +reset() void
        +commit_header() void
    }

    class HttpConnection~Socket_RxSize_TxSize~ {
        +HttpConnection()
        +HttpConnection(socket)
        +assign(socket) void
        +is_valid() bool
        +process~MaxRoutes_WsTraits~(routes, middleware, mw_count, ws_alloc) bool
        +close() void
        -socket_ : Socket
        -state_ : ParseState
        -response_ : HttpResponse
        -parser_ : llhttp_t
        -rx_buf_ : array~uint8_t_RxSize~
        -tx_buf_ : array~char_TxSize~
        -send_error(code, msg, keep) void
        -run_websocket~WsTraits~(handlers, alloc, consumed, buffered) void
        -raw_write_trampoline(ctx, data, len) int$
        -stream_resolve_trampoline~MaxRoutes~(ctx, req) StreamHandlerFn$
    }

    class ConnectionPool~Socket_MaxConns_RxSize_TxSize~ {
        +capacity() size_t$
        +active() size_t
        +full() bool
        +acquire(socket) Connection*
        +release(conn) void
        +close_all() void
        -slots_ : Connection[MaxConns]
        -in_use_ : bool[MaxConns]
        -active_ : size_t
    }

    class HttpServer~Socket_MaxRoutes_RxSize_TxSize_MaxMiddleware_MaxConns_WsTraits~ {
        +use(mw) bool
        +add_route(method, path, handler) bool
        +get / post / put / del(path, handler) bool
        +patch / head / options(path, handler) bool
        +post_stream(path, handler) bool
        +put_stream(path, handler) bool
        +websocket(path, handlers) bool
        +ws_allocator() Allocator&
        +run(acceptor) void
        +poll(acceptor) void
        +stop() void
        +shutdown() void
        +routes() const RouteTable&
        +middleware_count() size_t
        +active_connections() size_t
        +max_connections() size_t$
        -routes_ : RouteTable~MaxRoutes~
        -middleware_ : MiddlewareFn[MaxMiddleware]
        -mw_count_ : size_t
        -pool_ : ConnectionPool
        -ws_alloc_ : WsTraits_Allocator
        -running_ : bool
    }

    class MiddlewareChain {
        <<see 03_middleware>>
    }
    class HttpRequest {
        <<see 01_core_types>>
    }
    class HttpResponse {
        <<see 01_core_types>>
    }
    class WsTraits["ws::Traits"] {
        <<see 04_websocket>>
    }
    class llhttp_t {
        <<external>>
    }
    class AcceptorConcept {
        <<concept>>
    }
    class SocketConcept {
        <<concept>>
    }

    HttpServer *-- RouteTable : owns
    HttpServer *-- ConnectionPool : owns
    HttpServer *-- MiddlewareChain : "fn-ptr array (MiddlewareFn)"
    HttpServer o-- WsTraits : Sha1 + Allocator
    HttpServer ..> AcceptorConcept : run / poll
    ConnectionPool *-- HttpConnection : N slots
    HttpConnection *-- ParseState
    HttpConnection *-- HttpResponse
    HttpConnection *-- llhttp_t : parser
    HttpConnection ..> SocketConcept : Socket
    HttpConnection ..> RouteTable : find(method, path, req)
    HttpConnection ..> MiddlewareChain : builds and runs
    ParseState *-- HttpRequest
    RouteTable *-- Route : 0..N
```

## Przebieg `HttpConnection::process`

1. `llhttp_reset`, podpięcie resolvera tras strumieniowych (`stream_resolve`).
2. Pętla keep-alive: `state_.reset()` → odczyt + `llhttp_execute` aż do `message_complete` (parser pauzowany na granicy wiadomości, reszta bufora przesuwana na początek).
3. Błędy protokołu generowane przez silnik przed middleware: `431` (nagłówki), `413` (body), `400` (składnia), `501` (nieobsługiwana metoda), `405`, `404`.
4. Dispatch:
   - trasa strumieniowa — handler dostał już chunki z `on_body`; silnik wysyła `200 OK`, jeśli handler nie przejął odpowiedzi,
   - trasa WebSocket — `MiddlewareChain` kończy się na `ws::upgrade_handler<Sha1>`; przy `101` → `run_websocket` → `ws::serve`,
   - zwykła trasa — `MiddlewareChain` → handler → `response_.send(socket_)`.
5. `Connection: close` (lub HTTP/1.0 bez keep-alive) kończy pętlę.

## Uwagi

- `HttpServer` obsługuje jedno połączenie naraz (`run` = accept → `pool_.acquire` → `process` → `release`); przy wyczerpaniu puli połączenie jest odrzucane. Dla N równoległych klientów używa się N instancji (wątków) współdzielących acceptor.
- `RouteTable::find(method, path, req)` najpierw szuka dopasowania literalnego, potem wzorców `{name}` (wypełnia `req.path_params`).
- `path_exists` rozróżnia `404` od `405`.
- `shutdown()` = `ConnectionPool::close_all()` (graceful shutdown).
