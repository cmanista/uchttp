# WebSocket (RFC 6455)

Source: `include/uchttp_ws.h`, `include/uchttp.h` (handshake),
`src/uchttp_ws*.cpp`, `src/port/zephyr/zephyr_ws.h`, `src/port/zephyr/zephyr_psa_sha1.h`.
[← index](README.md)

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

    class CloseCode {
        <<namespace uchttp::ws::close_code>>
        normal = 1000
        going_away = 1001
        protocol_error = 1002
        unsupported = 1003
        no_status = 1005
        abnormal = 1006
        invalid_payload = 1007
        policy = 1008
        too_big = 1009
        extension = 1010
        internal_error = 1011
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
        -user_ : void*
        -msg_cap_ : size_t
        -max_frame_ : size_t
        -state_ : State
        -broken_ : bool
        -Session(t, h, rx, tx)
        -run(req, leftover) void
        -send_frame(op, p, n) bool
        -write_all(p, n) bool
        -fail(code) void
        -drain() void
        -finish(code) void
    }

    class SessionState {
        <<enumeration>>
        open
        closing
        closed
    }

    class Handshake {
        <<namespace uchttp::ws>>
        +find_header(req, name) string_view
        +header_has_token(list, token, ignore_case) bool
        +validate_handshake(req, key) int
        +upgrade_handler~Sha1~(req, res) void
    }

    class Serve {
        <<free function, friend of Session>>
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
        -inited_ : bool
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
    Session *-- SessionState
    Session ..> Opcode
    Session ..> CloseCode
    Serve ..> Session : creates (friend)
    Handshake ..> Codec : compute_accept
    HttpConnection ..> Handshake : upgrade_handler
    HttpConnection ..> Serve : serve()
    HttpConnection ..> Transport : binds to Socket
    HttpConnection ..> AllocatorConcept : session buffer
```

## Notes

- The WebSocket core (`uchttp_ws.h`) knows nothing about sockets, the OS or a crypto library. Everything is injected: SHA-1 (`Sha1Concept`), session memory (`AllocatorConcept`) and the transport (`Transport`: function pointers plus a context, bound by `HttpConnection`).
- A WebSocket endpoint is an ordinary route (`HttpServer::websocket`). After the `101` reply, `HttpConnection` takes a buffer from the allocator (no memory means `503`), calls the optional `Handlers::accept` (refusal means `403`) and runs `ws::serve`.
- `Session` has a private constructor. Only `ws::serve` (a friend) creates it.
- Transport: `ReadFn` returns `>0` bytes, `0` when the peer closed, `<0` on timeout or error. `set_timeout` is optional (set only when the `Socket` has `set_read_timeout`).
- `UCHTTP_WS_CONTROL_RESERVE` (144 B) keeps room in the session buffer for a control frame between the fragments of a message. `UCHTTP_WS_BUFFER_SIZE` = `UCHTTP_WS_MAX_MESSAGE_SIZE` + that reserve.
- Limits: `UCHTTP_WS_MAX_FRAME_SIZE` (checked from the frame header alone) and `UCHTTP_WS_MAX_MESSAGE_SIZE` (after reassembly, close code `1009`). A keep-alive Ping is sent after `UCHTTP_WS_PING_INTERVAL_MS`.
- `on_idle` is called every time the read timeout expires (`idle_interval_ms`), which gives periodic push without an extra thread.
- The Zephyr components (`ZephyrPsaSha1`, `ZephyrSlabAllocator`) are also described in [06_platform_ports.md](06_platform_ports.md).
