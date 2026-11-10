// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

/*
 * uchttp_ws.h - WebSocket (RFC 6455) support, requirement #8.
 *
 * Platform independent: nothing in here knows about sockets, an OS or a
 * crypto library. Everything platform specific is injected:
 *
 *   - SHA-1 (for Sec-WebSocket-Accept)   -> Sha1Concept   (CRTP/concepts, no
 *     virtuals). SoftSha1 is the portable default; a port may supply a
 *     hardware-accelerated or crypto-library backed provider instead.
 *   - Session buffer memory              -> AllocatorConcept. StaticAllocator
 *     is the default (static storage, no heap); an RTOS port may plug in e.g.
 *     a Zephyr k_mem_slab.
 *   - Byte transport                     -> Transport (fn pointers + ctx),
 *     bound by HttpConnection to the concrete Socket.
 *
 * The frame codec (parse_header / encode_header / apply_mask), the UTF-8
 * validator and the base64 encoder are plain functions and can be unit tested
 * without any socket.
 *
 * Application API is callback based, see Handlers and Session.
 */

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// ---------------------------------------------------------------------------
// Configuration - tune for your target MCU
// ---------------------------------------------------------------------------

/* Largest data message (after reassembly of fragments) the server accepts. */
#ifndef UCHTTP_WS_MAX_MESSAGE_SIZE
#  define UCHTTP_WS_MAX_MESSAGE_SIZE 1024
#endif
/* Largest payload of a single frame the server accepts (frame size limit).
 * A bigger frame is rejected with close code 1009 right after its header was
 * parsed, before its payload is read. */
#ifndef UCHTTP_WS_MAX_FRAME_SIZE
#  define UCHTTP_WS_MAX_FRAME_SIZE UCHTTP_WS_MAX_MESSAGE_SIZE
#endif
/* Room kept free in the session RX buffer so that a control frame (max 125
 * bytes payload + 14 bytes header) can always be received between the
 * fragments of a data message. */
#ifndef UCHTTP_WS_CONTROL_RESERVE
#  define UCHTTP_WS_CONTROL_RESERVE 144
#endif
/* Session RX buffer: one block handed out by the allocator per session. */
#ifndef UCHTTP_WS_BUFFER_SIZE
#  define UCHTTP_WS_BUFFER_SIZE \
	(UCHTTP_WS_MAX_MESSAGE_SIZE + UCHTTP_WS_CONTROL_RESERVE)
#endif
/* Blocks in the default static allocator (= concurrent sessions per server;
 * HttpServer serves one connection at a time, so 1 is enough). */
#ifndef UCHTTP_WS_MAX_SESSIONS
#  define UCHTTP_WS_MAX_SESSIONS 1
#endif
/* Read timeout used as the session "tick": every timeout calls on_idle(). */
#ifndef UCHTTP_WS_IDLE_INTERVAL_MS
#  define UCHTTP_WS_IDLE_INTERVAL_MS 1000
#endif
/* Silence after which the server sends a Ping; the peer is dropped if it stays
 * silent for the same time again. 0 disables the keep-alive. */
#ifndef UCHTTP_WS_PING_INTERVAL_MS
#  define UCHTTP_WS_PING_INTERVAL_MS 15000
#endif

namespace uchttp {

struct HttpRequest;

namespace ws {

/* ------------------------------------------------------------------------ */
/* Protocol constants                                                       */
/* ------------------------------------------------------------------------ */

enum class Opcode : uint8_t {
	Continuation = 0x0,
	Text         = 0x1,
	Binary       = 0x2,
	Close        = 0x8,
	Ping         = 0x9,
	Pong         = 0xA,
};

namespace close_code {
inline constexpr uint16_t normal          = 1000;
inline constexpr uint16_t going_away      = 1001;
inline constexpr uint16_t protocol_error  = 1002;
inline constexpr uint16_t unsupported     = 1003;
inline constexpr uint16_t no_status       = 1005;  /* never sent on the wire */
inline constexpr uint16_t abnormal        = 1006;  /* never sent on the wire */
inline constexpr uint16_t invalid_payload = 1007;
inline constexpr uint16_t policy          = 1008;
inline constexpr uint16_t too_big         = 1009;
inline constexpr uint16_t extension       = 1010;
inline constexpr uint16_t internal_error  = 1011;
}

/* RFC 6455 4.2.2: GUID appended to Sec-WebSocket-Key before hashing. */
inline constexpr std::string_view handshake_guid =
	"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline constexpr size_t accept_key_len  = 28;  /* base64(SHA-1) */
inline constexpr size_t max_header_len  = 14;  /* 2 + 8 (len) + 4 (mask) */
inline constexpr size_t max_control_len = 125;

/* ------------------------------------------------------------------------ */
/* Frame codec (pure functions, no I/O)                                     */
/* ------------------------------------------------------------------------ */

struct FrameHeader {
	bool     fin         { false };
	uint8_t  rsv         { 0 };      /* RSV1..3 as bits 2..0 */
	uint8_t  opcode      { 0 };      /* raw 4-bit opcode */
	bool     masked      { false };
	uint8_t  mask[4]     {};
	uint64_t payload_len { 0 };
	uint8_t  header_len  { 0 };      /* bytes occupied by the header */
};

enum class ParseResult : uint8_t {
	ok,          /* header complete, `out` filled */
	need_more,   /* not enough bytes yet */
	error,       /* malformed header (e.g. non-minimal length) */
};

/* Parse a frame header from the start of `in`. Does not look at the payload. */
ParseResult parse_header(std::span<const uint8_t> in, FrameHeader &out) noexcept;

/* Encode a frame header into `out` (needs max_header_len bytes). If `mask_key`
 * is non-null the MASK bit is set and the key appended (client frames, tests).
 * Server frames are unmasked. Returns the header length. */
size_t encode_header(uint8_t *out, Opcode op, uint64_t payload_len,
		     bool fin = true, const uint8_t *mask_key = nullptr) noexcept;

/* XOR `data` with the 4 byte masking key (RFC 6455 5.3). Symmetric. */
void apply_mask(uint8_t *data, size_t len, const uint8_t key[4]) noexcept;

/* Strict UTF-8 validation (no overlongs, no surrogates, <= U+10FFFF). */
bool utf8_valid(std::span<const uint8_t> data) noexcept;

/* Close status codes that may appear on the wire (RFC 6455 7.4). */
bool close_code_valid(uint16_t code) noexcept;

/* Standard base64 with padding; returns bytes written (0 if `cap` too small). */
size_t base64_encode(std::span<const uint8_t> in, char *out, size_t cap) noexcept;

/* Sec-WebSocket-Key must be base64 of 16 bytes (24 chars, "==" padded). */
bool key_valid(std::string_view key) noexcept;

/* ------------------------------------------------------------------------ */
/* SHA-1 abstraction                                                        */
/* ------------------------------------------------------------------------ */

/* A SHA-1 provider. Default constructed per handshake; may wrap a hardware
 * engine or a crypto library. Each step reports failure so that a hardware
 * back end can bail out. */
template <typename T>
concept Sha1Concept = requires(T &h, std::span<const uint8_t> d,
			       std::span<uint8_t, 20> out) {
	{ h.start()      } -> std::same_as<bool>;
	{ h.update(d)    } -> std::same_as<bool>;
	{ h.finish(out)  } -> std::same_as<bool>;
};

/* Portable, dependency-free SHA-1 (FIPS 180-4). */
class SoftSha1 {
public:
	bool start() noexcept;
	bool update(std::span<const uint8_t> data) noexcept;
	bool finish(std::span<uint8_t, 20> digest) noexcept;

private:
	void block(const uint8_t *b) noexcept;

	uint32_t h_[5]      {};
	uint64_t total_     { 0 };
	uint8_t  buf_[64]   {};
	size_t   buf_len_   { 0 };
};

/* Sec-WebSocket-Accept = base64(SHA-1(key + GUID)). Writes accept_key_len
 * chars to `out`; returns that length, or 0 on failure. */
template <Sha1Concept Sha1>
size_t compute_accept(std::string_view key, std::span<char> out) noexcept
{
	if (out.size() < accept_key_len) return 0;
	Sha1 h;
	uint8_t digest[20];
	auto bytes = [](std::string_view s) {
		return std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(s.data()), s.size());
	};
	if (!h.start() || !h.update(bytes(key)) ||
	    !h.update(bytes(handshake_guid)) || !h.finish(digest)) {
		return 0;
	}
	return base64_encode(digest, out.data(), out.size());
}

/* ------------------------------------------------------------------------ */
/* Buffer allocator abstraction                                             */
/* ------------------------------------------------------------------------ */

/* Supplies the per-session RX buffer. allocate() returns nullptr when memory
 * is exhausted (the upgrade is then refused with 503). */
template <typename T>
concept AllocatorConcept = requires(T &a, size_t n, void *p) {
	{ a.allocate(n)       } -> std::same_as<void *>;
	{ a.deallocate(p, n)  } -> std::same_as<void>;
};

/* Fixed pool of `Blocks` blocks of `BlockSize` bytes in the object itself:
 * no heap. */
template <size_t Blocks, size_t BlockSize>
class StaticAllocator {
public:
	static constexpr size_t block_size() noexcept { return BlockSize; }

	void *allocate(size_t n) noexcept
	{
		if (n > BlockSize) return nullptr;
		for (size_t i = 0; i < Blocks; ++i) {
			if (!used_[i]) {
				used_[i] = true;
				return pool_[i];
			}
		}
		return nullptr;
	}

	void deallocate(void *p, size_t) noexcept
	{
		for (size_t i = 0; i < Blocks; ++i) {
			if (p == static_cast<void *>(pool_[i])) {
				used_[i] = false;
				return;
			}
		}
	}

private:
	alignas(8) uint8_t pool_[Blocks][BlockSize];
	bool used_[Blocks] {};
};

/* ------------------------------------------------------------------------ */
/* Application API                                                          */
/* ------------------------------------------------------------------------ */

class Session;

/* Callbacks of a WebSocket endpoint. Plain function pointers (no
 * std::function, no virtuals); any member may be left null. The object is
 * stored by pointer in the route table: keep it alive (static storage). */
struct Handlers {
	/* Handshake done, 101 sent. `req` is the upgrade request (views into the
	 * HTTP RX buffer, valid during this call only). */
	void (*on_open)(Session &s, const HttpRequest &req) { nullptr };

	/* A complete message arrived (fragments are reassembled). Text messages
	 * are valid UTF-8. `data` is valid during this call only. */
	void (*on_message)(Session &s, Opcode type,
			   std::span<const uint8_t> data) { nullptr };

	/* Session over, called exactly once. `code` is the peer's close code, or
	 * close_code::no_status / abnormal (1006: connection lost). */
	void (*on_close)(Session &s, uint16_t code) { nullptr };

	/* The read timeout expired with nothing received (every
	 * idle_interval_ms). Place to push periodic telemetry. */
	void (*on_idle)(Session &s) { nullptr };

	/* Optional gate called before 101 is sent (after the middleware ran):
	 * return false to refuse the upgrade with 403 (e.g. Origin check). */
	bool (*accept)(const HttpRequest &req) { nullptr };

	/* Sub-protocol to select if the client offers it (Sec-WebSocket-Protocol). */
	std::string_view subprotocol {};

	unsigned idle_interval_ms { UCHTTP_WS_IDLE_INTERVAL_MS };
	unsigned ping_interval_ms { UCHTTP_WS_PING_INTERVAL_MS };

	/* Application context, available through Session::user(). */
	void *user { nullptr };
};

/* Byte transport the session runs over (bound by HttpConnection). read():
 * >0 bytes, 0 = peer closed, <0 = timeout/error. */
struct Transport {
	using ReadFn    = int  (*)(void *ctx, uint8_t *buf, size_t len) noexcept;
	using WriteFn   = int  (*)(void *ctx, const uint8_t *buf, size_t len) noexcept;
	using TimeoutFn = bool (*)(void *ctx, unsigned ms) noexcept;

	ReadFn    read        { nullptr };
	WriteFn   write       { nullptr };
	TimeoutFn set_timeout { nullptr };   /* optional */
	void     *ctx         { nullptr };
};

/* One established WebSocket connection, handed to every callback. Send
 * methods are meant to be called from the callbacks (single threaded
 * engine); they never allocate. */
class Session {
public:
	Session(const Session &)            = delete;
	Session &operator=(const Session &) = delete;

	bool send_text(std::string_view text) noexcept;
	bool send_binary(std::span<const uint8_t> data) noexcept;
	bool ping(std::span<const uint8_t> data = {}) noexcept;

	/* Start the closing handshake (Close frame; reason <= 123 bytes). The
	 * session ends when the peer answers or after one idle interval. */
	bool close(uint16_t code = close_code::normal,
		   std::string_view reason = {}) noexcept;

	bool  is_open() const noexcept;
	void *user() const noexcept { return user_; }
	void  set_user(void *p) noexcept { user_ = p; }

	/* Largest message / frame payload this session accepts. */
	size_t max_message_size() const noexcept { return msg_cap_; }

private:
	friend void serve(const Transport &, const Handlers &, const HttpRequest &,
			  std::span<uint8_t>, std::span<uint8_t>,
			  std::span<const uint8_t>) noexcept;

	enum class State : uint8_t { open, closing, closed };

	Session(const Transport &t, const Handlers &h, std::span<uint8_t> rx,
		std::span<uint8_t> tx) noexcept;

	void run(const HttpRequest &req, std::span<const uint8_t> leftover) noexcept;
	bool send_frame(Opcode op, const uint8_t *p, size_t n) noexcept;
	bool write_all(const uint8_t *p, size_t n) noexcept;
	void fail(uint16_t code) noexcept;
	void drain() noexcept;
	void finish(uint16_t code) noexcept;

	Transport          t_;
	const Handlers    &h_;
	std::span<uint8_t> rx_;
	std::span<uint8_t> tx_;
	void              *user_ { nullptr };
	size_t             msg_cap_ { 0 };
	size_t             max_frame_ { 0 };
	State              state_ { State::open };
	bool               broken_ { false };
};

/* Run a session on an already upgraded connection until it ends. `rx` is the
 * session buffer (from the allocator), `tx` a scratch buffer used to coalesce
 * outgoing frames, `leftover` the bytes the HTTP parser already read past the
 * upgrade request. Returns when the connection is finished; the caller then
 * closes the socket. */
void serve(const Transport &t, const Handlers &h, const HttpRequest &req,
	   std::span<uint8_t> rx, std::span<uint8_t> tx,
	   std::span<const uint8_t> leftover) noexcept;

/* Compile-time bundle of the platform hooks HttpServer is parameterised with. */
template <Sha1Concept Sha1T, AllocatorConcept Alloc>
struct Traits {
	using Sha1      = Sha1T;
	using Allocator = Alloc;
};

using DefaultTraits = Traits<SoftSha1,
			     StaticAllocator<UCHTTP_WS_MAX_SESSIONS,
					     UCHTTP_WS_BUFFER_SIZE>>;

} // namespace ws
} // namespace uchttp
