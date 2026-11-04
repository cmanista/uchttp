# WebSocket (RFC 6455)

Źródło: `include/uchttp_ws.h`, `include/uchttp.h` (handshake),
`src/uchttp_ws*.cpp`, `src/port/zephyr_ws.h`, `src/port/zephyr_psa_sha1.h`.
[← indeks](../../class_diagram.md)

```mermaid
classDiagram
    class Sha1Concept {
        <<concept>>
        +start() bool
        +update(span~const uint8_t~) bool
        +finish(span~uint8_t_20~) bool
    }

    class AllocatorConcept {
        <<concept>>
        +allocate(n) void*
        +deallocate(p, n) void
    }

    class SoftSha1 {
        +start() bool
        +update(data) bool
        +finish(digest) bool
        -h_ : uint32_t[5]
        -total_ : uint64_t
        -buf_ : uint8_t[64]
        -buf_len_ : size_t
        -block(b) void
    }

    class StaticAllocator~Blocks_BlockSize~ {
        +block_size() size_t$
        +allocate(n) void*
        +deallocate(p, n) void
        -pool_ : uint8_t[Blocks][BlockSize]
        -used_ : bool[Blocks]
    }

    class Traits~Sha1_Allocator~ {
        <<struct>>
        +Sha1
        +Allocator
    }

    class DefaultTraits {
        <<alias>>
        Traits of SoftSha1 and StaticAllocator
    }

    class Opcode {
        <<enumeration>>
        Continuation
        Text
        Binary
        Close
        Ping
        Pong
    }

    class FrameHeader {
        <<struct>>
        +fin : bool
        +rsv : uint8_t
        +opcode : uint8_t
        +masked : bool
        +mask : uint8_t[4]
        +payload_len : uint64_t
        +header_len : uint8_t
    }

    class ParseResult {
        <<enumeration>>
        ok
        need_more
        error
    }

    class Codec {
        <<free functions>>
        +parse_header(in, out) ParseResult
        +encode_header(out, op, len, fin, mask_key) size_t
        +apply_mask(data, len, key) void
        +utf8_valid(data) bool
        +close_code_valid(code) bool
        +base64_encode(in, out, cap) size_t
        +key_valid(key) bool
        +compute_accept~Sha1~(key, out) size_t
    }

    class Handlers {
        <<struct>>
        +on_open(Session, HttpRequest) fn
        +on_message(Session, Opcode, data) fn
        +on_close(Session, code) fn
        +on_idle(Session) fn
        +accept(HttpRequest) bool fn
        +subprotocol : string_view
        +idle_interval_ms : unsigned
        +ping_interval_ms : unsigned
        +user : void*
    }

    class Transport {
        <<struct>>
        +read : ReadFn
        +write : WriteFn
        +set_timeout : TimeoutFn
        +ctx : void*
    }

    class Session {
        +send_text(text) bool
        +send_binary(data) bool
        +ping(data) bool
        +close(code, reason) bool
        +is_open() bool
        +user() void*
        +set_user(p) void
        +max_message_size() size_t
        -t_ : Transport
        -h_ : const Handlers&
        -rx_ : span~uint8_t~
        -tx_ : span~uint8_t~
        -state_ : State
        -run(req, leftover) void
        -send_frame(op, p, n) bool
        -fail(code) void
        -drain() void
        -finish(code) void
    }

    class Handshake {
        <<namespace uchttp::ws>>
        +find_header(req, name) string_view
        +header_has_token(list, token, ignore_case) bool
        +validate_handshake(req, key) int
        +upgrade_handler~Sha1~(req, res) void
        +serve(t, h, req, rx, tx, leftover) void
    }

    class ZephyrPsaSha1 {
        <<Zephyr port>>
        +start() bool
        +update(data) bool
        +finish(digest) bool
    }

    class ZephyrSlabAllocator~Blocks_BlockSize~ {
        <<Zephyr port>>
        +init() void
        +allocate(n) void*
        +deallocate(p, n) void
        +used() size_t
        -slab_ : k_mem_slab
        -buf_ : uint8_t[]
    }

    class HttpConnection {
        <<see 02_engine>>
    }
    class HttpServer {
        <<see 02_engine>>
    }

    Sha1Concept <|.. SoftSha1 : satisfies
    Sha1Concept <|.. ZephyrPsaSha1 : satisfies
    AllocatorConcept <|.. StaticAllocator : satisfies
    AllocatorConcept <|.. ZephyrSlabAllocator : satisfies

    Traits o-- Sha1Concept
    Traits o-- AllocatorConcept
    DefaultTraits ..> SoftSha1
    DefaultTraits ..> StaticAllocator
    HttpServer o-- Traits : last template param

    Codec ..> FrameHeader
    Codec ..> ParseResult
    Codec ..> Sha1Concept : compute_accept
    FrameHeader ..> Opcode : raw opcode
    Session ..> Codec : frames
    Session o-- Handlers
    Session o-- Transport
    Session ..> Opcode
    Handshake ..> Session : creates
    Handshake ..> Codec : compute_accept
    HttpConnection ..> Handshake : upgrade + serve
    HttpConnection ..> Transport : binds to Socket
    HttpConnection ..> AllocatorConcept : session buffer
```

## Uwagi

- Rdzeń WebSocket (`uchttp_ws.h`) nie zna gniazd, OS ani biblioteki kryptograficznej; wszystko jest wstrzykiwane: SHA-1 (`Sha1Concept`), pamięć sesji (`AllocatorConcept`), transport (`Transport` — wskaźniki na funkcje + kontekst, wiązany przez `HttpConnection`).
- Endpoint WS to zwykła trasa (`HttpServer::websocket`). `HttpConnection` po odpowiedzi `101` pobiera bufor z alokatora (brak pamięci → `503`), wywołuje opcjonalne `Handlers::accept` (odmowa → `403`) i uruchamia `ws::serve`.
- `Session` ma prywatny konstruktor; tworzy go wyłącznie `ws::serve` (friend).
- Limity: `UCHTTP_WS_MAX_FRAME_SIZE` (sprawdzany z samego nagłówka), `UCHTTP_WS_MAX_MESSAGE_SIZE` (po złożeniu fragmentów, close `1009`), keep-alive Ping po `UCHTTP_WS_PING_INTERVAL_MS`.
- `on_idle` jest wołane przy każdym wygaśnięciu timeoutu odczytu (`idle_interval_ms`) — okresowy push bez dodatkowego wątku.
- Komponenty Zephyra (`ZephyrPsaSha1`, `ZephyrSlabAllocator`) opisane także w [06_platform_ports.md](06_platform_ports.md).
