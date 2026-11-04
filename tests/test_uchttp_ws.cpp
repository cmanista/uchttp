// test_uchttp_ws.cpp - gtest unit tests for the WebSocket support (req. #8)
//
//   - frame codec, masking, UTF-8, base64, SHA-1 (RFC test vectors)
//   - ws::Session / ws::serve() driven through a scripted in-memory
//     transport: fragmentation, control frames, protocol errors, close
//     handshake, keep-alive, size limits, partial reads
//   - handshake + hand-over through HttpConnection::process() with
//     middleware, allocator and SHA-1 plug-ins (mock socket, no network)

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "uchttp.h"

using namespace uchttp;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::span<const uint8_t> bytes(std::string_view s) noexcept
{
	return { reinterpret_cast<const uint8_t *>(s.data()), s.size() };
}

std::string hex(std::span<const uint8_t> d)
{
	static const char *h = "0123456789abcdef";
	std::string s;
	for (uint8_t b : d) { s += h[b >> 4]; s += h[b & 15]; }
	return s;
}

std::string sha1_hex(std::string_view data)
{
	ws::SoftSha1 h;
	uint8_t out[20];
	h.start();
	h.update(bytes(data));
	h.finish(out);
	return hex(out);
}

// A client->server frame, masked as every real client frame is.
std::string cframe(uint8_t opcode, std::string_view payload, bool fin = true,
		   bool mask = true, uint8_t rsv = 0)
{
	static const uint8_t key[4] = { 0x37, 0xfa, 0x21, 0x3d };
	std::string f;
	f += static_cast<char>((fin ? 0x80 : 0) | (rsv << 4) | opcode);
	uint8_t m = mask ? 0x80 : 0;
	if (payload.size() <= 125) {
		f += static_cast<char>(m | payload.size());
	} else if (payload.size() <= 0xFFFF) {
		f += static_cast<char>(m | 126);
		f += static_cast<char>(payload.size() >> 8);
		f += static_cast<char>(payload.size() & 0xFF);
	} else {
		f += static_cast<char>(m | 127);
		for (int i = 7; i >= 0; --i)
			f += static_cast<char>((uint64_t(payload.size()) >> (i * 8)) & 0xFF);
	}
	if (mask) f.append(reinterpret_cast<const char *>(key), 4);
	for (size_t i = 0; i < payload.size(); ++i)
		f += static_cast<char>(static_cast<uint8_t>(payload[i]) ^
				       (mask ? key[i & 3] : 0));
	return f;
}

std::string close_payload(uint16_t code, std::string_view reason = {})
{
	std::string p;
	p += static_cast<char>(code >> 8);
	p += static_cast<char>(code & 0xFF);
	p.append(reason);
	return p;
}

struct ServerFrame {
	uint8_t     opcode;
	bool        fin;
	std::string payload;

	uint16_t close_code() const
	{
		return payload.size() < 2 ? 0
			: static_cast<uint16_t>((uint8_t(payload[0]) << 8) | uint8_t(payload[1]));
	}
};

// Decode the (unmasked) frames the server wrote into `tx`.
std::vector<ServerFrame> sframes(const std::string &tx)
{
	std::vector<ServerFrame> v;
	size_t pos = 0;
	while (pos + 2 <= tx.size()) {
		uint8_t b0 = static_cast<uint8_t>(tx[pos]);
		uint8_t b1 = static_cast<uint8_t>(tx[pos + 1]);
		pos += 2;
		uint64_t len = b1 & 0x7F;
		if (len == 126) {
			if (pos + 2 > tx.size()) break;
			len = (uint8_t(tx[pos]) << 8) | uint8_t(tx[pos + 1]);
			pos += 2;
		} else if (len == 127) {
			if (pos + 8 > tx.size()) break;
			len = 0;
			for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(tx[pos + i]);
			pos += 8;
		}
		if (pos + len > tx.size()) break;
		v.push_back({ uint8_t(b0 & 0x0F), (b0 & 0x80) != 0,
			      std::string(tx.substr(pos, len)) });
		pos += len;
	}
	return v;
}

// ---------------------------------------------------------------------------
// Scripted transport: a queue of read events (data / timeout / eof) plus a
// recorder for everything written. Drives ws::serve() without any socket.
// ---------------------------------------------------------------------------

struct ScriptedTransport {
	enum class Kind { Data, Timeout, Eof };
	struct Step { Kind kind; std::string data; };

	std::vector<Step> script;
	size_t            next { 0 };
	size_t            pos_in_step { 0 };
	size_t            max_chunk { 0 };      // 0 = hand over a whole step per read
	std::string       tx;
	int               writes { 0 };
	bool              fail_writes { false };

	void push_data(std::string_view d) { script.push_back({ Kind::Data, std::string(d) }); }
	void push_timeout()                { script.push_back({ Kind::Timeout, {} }); }
	void push_eof()                    { script.push_back({ Kind::Eof, {} }); }

	int do_read(uint8_t *buf, size_t len) noexcept
	{
		while (next < script.size()) {
			Step &s = script[next];
			if (s.kind == Kind::Timeout) { ++next; return -1; }
			if (s.kind == Kind::Eof)     { ++next; return 0; }
			size_t avail = s.data.size() - pos_in_step;
			if (avail == 0) { ++next; pos_in_step = 0; continue; }
			size_t chunk = std::min(avail, len);
			if (max_chunk) chunk = std::min(chunk, max_chunk);
			std::memcpy(buf, s.data.data() + pos_in_step, chunk);
			pos_in_step += chunk;
			return static_cast<int>(chunk);
		}
		return -1;   // script exhausted: behave like an endless idle timeout
	}

	int do_write(const uint8_t *buf, size_t len) noexcept
	{
		if (fail_writes) return -1;
		tx.append(reinterpret_cast<const char *>(buf), len);
		++writes;
		return static_cast<int>(len);
	}

	static int read_tramp(void *ctx, uint8_t *buf, size_t len) noexcept
	{
		return static_cast<ScriptedTransport *>(ctx)->do_read(buf, len);
	}
	static int write_tramp(void *ctx, const uint8_t *buf, size_t len) noexcept
	{
		return static_cast<ScriptedTransport *>(ctx)->do_write(buf, len);
	}

	ws::Transport transport() noexcept
	{
		ws::Transport t;
		t.read  = &read_tramp;
		t.write = &write_tramp;
		t.ctx   = this;
		return t;
	}
};

// Recording state shared with the (capture-less) callbacks via Handlers::user.
struct Log {
	std::vector<std::string> messages;   // "text:<payload>" / "binary:<payload>"
	std::vector<std::string> events;     // "open" / "idle" / "close:<code>"

	int closes() const
	{
		return static_cast<int>(std::count_if(events.begin(), events.end(),
			[](const std::string &e) { return e.rfind("close:", 0) == 0; }));
	}
	bool has(const std::string &e) const
	{
		return std::find(events.begin(), events.end(), e) != events.end();
	}
};

Log &log_of(ws::Session &s) { return *static_cast<Log *>(s.user()); }

void rec_open(ws::Session &s, const HttpRequest &) { log_of(s).events.push_back("open"); }
void rec_idle(ws::Session &s)                      { log_of(s).events.push_back("idle"); }
void rec_close(ws::Session &s, uint16_t code)
{
	log_of(s).events.push_back("close:" + std::to_string(code));
}
void rec_message(ws::Session &s, ws::Opcode type, std::span<const uint8_t> d)
{
	log_of(s).messages.push_back(
		std::string(type == ws::Opcode::Text ? "text:" : "binary:") +
		std::string(reinterpret_cast<const char *>(d.data()), d.size()));
}
// Records, then echoes back what it received.
void rec_echo(ws::Session &s, ws::Opcode type, std::span<const uint8_t> d)
{
	rec_message(s, type, d);
	if (type == ws::Opcode::Text)
		s.send_text(std::string_view(reinterpret_cast<const char *>(d.data()), d.size()));
	else
		s.send_binary(d);
}

struct WsSessionTest : ::testing::Test {
	Log               log;
	ScriptedTransport tr;
	ws::Handlers      h {};
	size_t            rx_size { 256 };
	size_t            tx_size { 256 };

	WsSessionTest()
	{
		h.user       = &log;
		h.on_open    = rec_open;
		h.on_message = rec_message;
		h.on_close   = rec_close;
	}

	void run(std::string_view leftover = {})
	{
		std::vector<uint8_t> rx(rx_size), tx(tx_size);
		HttpRequest req;
		ws::serve(tr.transport(), h, req, rx, tx, bytes(leftover));
	}

	std::vector<ServerFrame> out() const { return sframes(tr.tx); }
};

} // namespace

// ===========================================================================
// Frame codec
// ===========================================================================

TEST(WsCodec, ParseNeedsMoreOnEmpty) {
	ws::FrameHeader fh;
	EXPECT_EQ(ws::parse_header({}, fh), ws::ParseResult::need_more);
}

TEST(WsCodec, ParseSmallMaskedFrame) {
	std::string f = cframe(0x1, "hello");
	ws::FrameHeader fh;
	ASSERT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::ok);
	EXPECT_TRUE(fh.fin);
	EXPECT_EQ(fh.opcode, 0x1);
	EXPECT_TRUE(fh.masked);
	EXPECT_EQ(fh.payload_len, 5u);
	EXPECT_EQ(fh.header_len, 6u);   // 2 + 4 mask
}

TEST(WsCodec, Parse16BitLength) {
	std::string f = cframe(0x2, std::string(200, 'x'));
	ws::FrameHeader fh;
	ASSERT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::ok);
	EXPECT_EQ(fh.payload_len, 200u);
	EXPECT_EQ(fh.header_len, 8u);   // 2 + 2 len + 4 mask
}

TEST(WsCodec, Parse64BitLength) {
	std::string f = cframe(0x2, std::string(70000, 'x'));
	ws::FrameHeader fh;
	ASSERT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::ok);
	EXPECT_EQ(fh.payload_len, 70000u);
	EXPECT_EQ(fh.header_len, 14u);  // 2 + 8 len + 4 mask
}

TEST(WsCodec, RejectsNonMinimal16BitLength) {
	std::string f;
	f += char(0x82);
	f += char(0x80 | 126);
	f += char(0); f += char(10);   // extended length 10: should use the 7-bit form
	f += std::string(4, '\0');
	ws::FrameHeader fh;
	EXPECT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::error);
}

TEST(WsCodec, RejectsNonMinimal64BitLength) {
	std::string f;
	f += char(0x82);
	f += char(0x80 | 127);
	for (int i = 0; i < 6; ++i) f += char(0);
	f += char(0x01); f += char(0x00);   // 256 fits in 16 bits
	f += std::string(4, '\0');
	ws::FrameHeader fh;
	EXPECT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::error);
}

TEST(WsCodec, Rejects64BitLengthWithMsbSet) {
	std::string f;
	f += char(0x82);
	f += char(0x80 | 127);
	f += char(0x80);
	for (int i = 0; i < 7; ++i) f += char(0);
	f += std::string(4, '\0');
	ws::FrameHeader fh;
	EXPECT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::error);
}

TEST(WsCodec, EveryTruncationOfAHeaderNeedsMore) {
	std::string f = cframe(0x2, std::string(300, 'x'));
	ws::FrameHeader fh;
	ASSERT_EQ(ws::parse_header(bytes(f), fh), ws::ParseResult::ok);
	for (size_t n = 0; n < fh.header_len; ++n)
		EXPECT_EQ(ws::parse_header(bytes(f).first(n), fh),
			  ws::ParseResult::need_more) << "n=" << n;
}

TEST(WsCodec, EncodeDecodeRoundTrip) {
	for (uint64_t len : { 0ull, 5ull, 125ull, 126ull, 300ull, 65535ull, 65536ull, 1ull << 33 }) {
		uint8_t hdr[ws::max_header_len];
		size_t n = ws::encode_header(hdr, ws::Opcode::Binary, len, true);
		ws::FrameHeader fh;
		ASSERT_EQ(ws::parse_header(std::span<const uint8_t>(hdr, n), fh),
			  ws::ParseResult::ok) << len;
		EXPECT_EQ(fh.opcode, static_cast<uint8_t>(ws::Opcode::Binary));
		EXPECT_EQ(fh.payload_len, len);
		EXPECT_FALSE(fh.masked);        // server frames are never masked
	}
}

TEST(WsCodec, EncodeHeaderWithMaskKeyAppendsIt) {
	const uint8_t key[4] = { 9, 8, 7, 6 };
	uint8_t hdr[ws::max_header_len];
	size_t n = ws::encode_header(hdr, ws::Opcode::Text, 3, true, key);
	ASSERT_EQ(n, 6u);
	EXPECT_EQ(hdr[1] & 0x80, 0x80);
	EXPECT_EQ(std::memcmp(hdr + 2, key, 4), 0);
}

// RFC 6455 section 5.7: "Hello" masked with 37 fa 21 3d.
TEST(WsCodec, MaskRfcExample) {
	const uint8_t key[4] = { 0x37, 0xfa, 0x21, 0x3d };
	uint8_t data[5] = { 'H', 'e', 'l', 'l', 'o' };
	ws::apply_mask(data, 5, key);
	const uint8_t expect[5] = { 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	EXPECT_EQ(std::memcmp(data, expect, 5), 0);
	ws::apply_mask(data, 5, key);
	EXPECT_EQ(std::memcmp(data, "Hello", 5), 0);
}

// ===========================================================================
// UTF-8 validation
// ===========================================================================

TEST(WsUtf8, AsciiAndEmptyAreValid) {
	EXPECT_TRUE(ws::utf8_valid(bytes("hello world")));
	EXPECT_TRUE(ws::utf8_valid({}));
}

TEST(WsUtf8, ValidMultiByteSequences) {
	const uint8_t two[]   = { 'h', 0xC3, 0xA9 };                // U+00E9
	const uint8_t three[] = { 0xE2, 0x82, 0xAC };               // U+20AC
	const uint8_t four[]  = { 0xF0, 0x9F, 0x98, 0x80 };         // U+1F600
	EXPECT_TRUE(ws::utf8_valid(two));
	EXPECT_TRUE(ws::utf8_valid(three));
	EXPECT_TRUE(ws::utf8_valid(four));
}

TEST(WsUtf8, RejectsOverlong) {
	const uint8_t data[] = { 0xC0, 0x80 };
	EXPECT_FALSE(ws::utf8_valid(data));
	const uint8_t data3[] = { 0xE0, 0x80, 0x80 };
	EXPECT_FALSE(ws::utf8_valid(data3));
}

TEST(WsUtf8, RejectsSurrogates) {
	const uint8_t data[] = { 0xED, 0xA0, 0x80 };   // U+D800
	EXPECT_FALSE(ws::utf8_valid(data));
}

TEST(WsUtf8, RejectsAboveUnicodeRange) {
	const uint8_t data[] = { 0xF4, 0x90, 0x80, 0x80 };   // U+110000
	EXPECT_FALSE(ws::utf8_valid(data));
}

TEST(WsUtf8, RejectsTruncatedAndBadContinuation) {
	const uint8_t trunc[] = { 0xE2, 0x82 };
	const uint8_t bad[]   = { 0xC2, 0x20 };
	const uint8_t lone[]  = { 0x80 };
	EXPECT_FALSE(ws::utf8_valid(trunc));
	EXPECT_FALSE(ws::utf8_valid(bad));
	EXPECT_FALSE(ws::utf8_valid(lone));
}

// ===========================================================================
// base64, SHA-1, Sec-WebSocket-Accept
// ===========================================================================

TEST(WsBase64, Rfc4648Vectors) {
	struct { const char *in, *out; } v[] = {
		{ "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
	};
	for (auto &t : v) {
		char out[16];
		size_t n = ws::base64_encode(bytes(t.in), out, sizeof(out));
		EXPECT_EQ(std::string_view(out, n), t.out) << t.in;
	}
}

TEST(WsBase64, ReturnsZeroWhenBufferTooSmall) {
	char out[2];
	EXPECT_EQ(ws::base64_encode(bytes("Man"), out, sizeof(out)), 0u);
}

TEST(WsSha1, Fips180Vectors) {
	EXPECT_EQ(sha1_hex(""),    "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	EXPECT_EQ(sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
	EXPECT_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		  "84983e441c3bd26ebaae4aa1f95129e5e54670f1");   // spans 2 blocks
}

TEST(WsSha1, MillionAs) {
	ws::SoftSha1 h;
	uint8_t out[20];
	std::string chunk(1000, 'a');
	h.start();
	for (int i = 0; i < 1000; ++i) h.update(bytes(chunk));
	h.finish(out);
	EXPECT_EQ(hex(out), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(WsSha1, EveryChunkingGivesTheSameDigest) {
	std::string msg(200, 'q');
	std::string want = sha1_hex(msg);
	for (size_t step = 1; step < 70; ++step) {
		ws::SoftSha1 h;
		uint8_t out[20];
		h.start();
		for (size_t i = 0; i < msg.size(); i += step)
			h.update(bytes(std::string_view(msg).substr(i, step)));
		h.finish(out);
		EXPECT_EQ(hex(out), want) << "step=" << step;
	}
}

// The canonical example of RFC 6455 section 1.3.
TEST(WsHandshake, RfcExampleComputesExpectedAccept) {
	char out[ws::accept_key_len];
	size_t n = ws::compute_accept<ws::SoftSha1>("dGhlIHNhbXBsZSBub25jZQ==", out);
	ASSERT_EQ(n, ws::accept_key_len);
	EXPECT_EQ(std::string_view(out, n), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsHandshake, ComputeAcceptNeedsRoomForTheResult) {
	char out[10];
	EXPECT_EQ(ws::compute_accept<ws::SoftSha1>("dGhlIHNhbXBsZSBub25jZQ==", out), 0u);
}

TEST(WsHandshake, KeyValidation) {
	EXPECT_TRUE(ws::key_valid("dGhlIHNhbXBsZSBub25jZQ=="));
	EXPECT_FALSE(ws::key_valid("dG9vc2hvcnQ="));                   // too short
	EXPECT_FALSE(ws::key_valid("dGhlIHNhbXBsZSBub25jZQAA"));       // no padding
	EXPECT_FALSE(ws::key_valid("dGhlIHNhbXBsZSBub25j!Q=="));       // bad alphabet
	EXPECT_FALSE(ws::key_valid(""));
}

TEST(WsHandshake, CloseCodeValidation) {
	for (uint16_t ok : { 1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 3000, 4999 })
		EXPECT_TRUE(ws::close_code_valid(ok)) << ok;
	for (uint16_t bad : { 0, 999, 1004, 1005, 1006, 1015, 1016, 2999, 5000 })
		EXPECT_FALSE(ws::close_code_valid(bad)) << bad;
}

TEST(WsHandshake, HeaderTokenMatching) {
	EXPECT_TRUE(ws::header_has_token("keep-alive, Upgrade", "upgrade", true));
	EXPECT_TRUE(ws::header_has_token("  websocket ", "websocket", true));
	EXPECT_FALSE(ws::header_has_token("Upgradesomething", "upgrade", true));
	EXPECT_FALSE(ws::header_has_token("chat", "CHAT", false));   // case-sensitive mode
	EXPECT_FALSE(ws::header_has_token("", "x", true));
}

// ===========================================================================
// Handshake validation (plain function, no socket)
// ===========================================================================

namespace {
HttpRequest make_upgrade(std::vector<HttpHeader> extra = {}, bool drop_upgrade = false,
			 std::string_view version = "13",
			 std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==")
{
	HttpRequest r;
	r.method = HttpMethod::GET;
	r.http_major = 1; r.http_minor = 1;
	auto add = [&](HttpHeader hh) { r.headers[r.header_count++] = hh; };
	if (!drop_upgrade) add({ "Upgrade", "websocket" });
	add({ "Connection", "Upgrade" });
	add({ "Sec-WebSocket-Version", version });
	add({ "Sec-WebSocket-Key", key });
	for (auto &e : extra) add(e);
	return r;
}
} // namespace

TEST(WsHandshakeValidate, AcceptsWellFormedUpgrade) {
	HttpRequest r = make_upgrade();
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 0);
	EXPECT_EQ(key, "dGhlIHNhbXBsZSBub25jZQ==");
}

TEST(WsHandshakeValidate, HeaderNamesAreCaseInsensitive) {
	HttpRequest r;
	r.method = HttpMethod::GET;
	r.http_major = 1; r.http_minor = 1;
	r.headers[r.header_count++] = { "upgrade", "WebSocket" };
	r.headers[r.header_count++] = { "CONNECTION", "keep-alive, upgrade" };
	r.headers[r.header_count++] = { "sec-websocket-version", "13" };
	r.headers[r.header_count++] = { "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==" };
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 0);
}

TEST(WsHandshakeValidate, MissingUpgradeIs426) {
	HttpRequest r = make_upgrade({}, /*drop_upgrade=*/true);
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 426);
}

TEST(WsHandshakeValidate, WrongVersionIs426) {
	HttpRequest r = make_upgrade({}, false, "8");
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 426);
}

TEST(WsHandshakeValidate, MalformedKeyIs400) {
	HttpRequest r = make_upgrade({}, false, "13", "not-a-valid-key");
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 400);
}

TEST(WsHandshakeValidate, Http10Is400) {
	HttpRequest r = make_upgrade();
	r.http_minor = 0;
	std::string_view key;
	EXPECT_EQ(ws::validate_handshake(r, key), 400);
}

// ===========================================================================
// Session: messages
// ===========================================================================

TEST_F(WsSessionTest, OpenMessageCloseLifecycle) {
	tr.push_data(cframe(0x1, "hi"));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();

	EXPECT_EQ(log.events, (std::vector<std::string>{ "open", "close:1000" }));
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:hi" }));
	// The peer started the close: the server echoes the code and ends.
	auto f = out();
	ASSERT_EQ(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0x8);
	EXPECT_EQ(f[0].close_code(), 1000);
}

TEST_F(WsSessionTest, EchoesTextAndBinary) {
	h.on_message = rec_echo;
	tr.push_data(cframe(0x1, "text!"));
	tr.push_data(cframe(0x2, std::string("\x00\x01\xff", 3)));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();

	auto f = out();
	ASSERT_GE(f.size(), 2u);
	EXPECT_EQ(f[0].opcode, 0x1);
	EXPECT_EQ(f[0].payload, "text!");
	EXPECT_EQ(f[1].opcode, 0x2);
	EXPECT_EQ(f[1].payload, std::string("\x00\x01\xff", 3));
}

TEST_F(WsSessionTest, EmptyMessageIsDelivered) {
	tr.push_data(cframe(0x1, ""));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:" }));
}

TEST_F(WsSessionTest, SeveralFramesInOneRead) {
	tr.push_data(cframe(0x1, "one") + cframe(0x1, "two") + cframe(0x1, "three") +
		     cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages,
		  (std::vector<std::string>{ "text:one", "text:two", "text:three" }));
}

TEST_F(WsSessionTest, ByteByByteDelivery) {
	tr.max_chunk = 1;
	tr.push_data(cframe(0x1, "He", false) + cframe(0x9, "p") +
		     cframe(0x0, "llo", true) + cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:Hello" }));
	auto f = out();
	ASSERT_GE(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0xA);
	EXPECT_EQ(f[0].payload, "p");
}

TEST_F(WsSessionTest, LargeMessageUsing16BitLength) {
	rx_size = 512;     // msg_cap = 368
	tx_size = 512;
	h.on_message = rec_echo;
	std::string big(300, 'k');
	tr.push_data(cframe(0x1, big));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:" + big }));
	auto f = out();
	ASSERT_GE(f.size(), 1u);
	EXPECT_EQ(f[0].payload, big);
}

TEST_F(WsSessionTest, ReplyLargerThanTxScratchStillSent) {
	tx_size = 32;                      // forces header + payload as two writes
	h.on_message = rec_echo;
	std::string msg(100, 'z');
	tr.push_data(cframe(0x1, msg));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	auto f = out();
	ASSERT_GE(f.size(), 1u);
	EXPECT_EQ(f[0].payload, msg);
}

TEST_F(WsSessionTest, SmallRepliesAreCoalescedIntoOneWrite) {
	h.on_message = rec_echo;
	tr.push_data(cframe(0x1, "abc"));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	// 1 write for the echo, 1 for the Close reply.
	EXPECT_EQ(tr.writes, 2);
}

TEST_F(WsSessionTest, LeftoverBytesFromHttpParserAreProcessedFirst) {
	tr.push_data(cframe(0x8, close_payload(1000)));
	run(cframe(0x1, "carried-over"));
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:carried-over" }));
}

// ===========================================================================
// Session: fragmentation
// ===========================================================================

TEST_F(WsSessionTest, ReassemblesFragmentedMessage) {
	tr.push_data(cframe(0x1, "Hel", false));
	tr.push_data(cframe(0x0, "lo,", false));
	tr.push_data(cframe(0x0, " world", true));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:Hello, world" }));
}

TEST_F(WsSessionTest, PingBetweenFragmentsIsAnsweredAndMessageSurvives) {
	tr.push_data(cframe(0x1, "AB", false));
	tr.push_data(cframe(0x9, "poke"));
	tr.push_data(cframe(0x0, "CD", true));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:ABCD" }));
	auto f = out();
	ASSERT_GE(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0xA);
	EXPECT_EQ(f[0].payload, "poke");
}

TEST_F(WsSessionTest, FragmentedBinaryMessage) {
	tr.push_data(cframe(0x2, "ab", false));
	tr.push_data(cframe(0x0, "cd", true));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "binary:abcd" }));
}

TEST_F(WsSessionTest, ManyConsecutiveFragmentedMessages) {
	for (int i = 0; i < 5; ++i) {
		tr.push_data(cframe(0x1, "x" + std::to_string(i), false));
		tr.push_data(cframe(0x0, "y", true));
	}
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	ASSERT_EQ(log.messages.size(), 5u);
	EXPECT_EQ(log.messages[0], "text:x0y");
	EXPECT_EQ(log.messages[4], "text:x4y");
}

TEST_F(WsSessionTest, InvalidUtf8SplitAcrossFragmentsIsStillCaught) {
	// "€" is E2 82 AC; split it and corrupt the last byte.
	tr.push_data(cframe(0x1, std::string("\xE2\x82", 2), false));
	tr.push_data(cframe(0x0, std::string("\x20", 1), true));
	run();
	auto f = out();
	ASSERT_EQ(f.size(), 1u);
	EXPECT_EQ(f[0].close_code(), ws::close_code::invalid_payload);
}

TEST_F(WsSessionTest, ValidUtf8SplitAcrossFragmentsIsAccepted) {
	tr.push_data(cframe(0x1, std::string("\xE2\x82", 2), false));
	tr.push_data(cframe(0x0, std::string("\xAC", 1), true));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:\xE2\x82\xAC" }));
}

// ===========================================================================
// Session: protocol errors -> Close with the proper code
// ===========================================================================

#define EXPECT_FAILS_WITH(code_)                                         \
	do {                                                             \
		auto f_ = out();                                         \
		ASSERT_EQ(f_.size(), 1u);                                \
		EXPECT_EQ(f_[0].opcode, 0x8);                            \
		EXPECT_EQ(f_[0].close_code(), (code_));                  \
		EXPECT_TRUE(log.has("close:" + std::to_string(code_)));  \
		EXPECT_EQ(log.closes(), 1);                              \
	} while (0)

TEST_F(WsSessionTest, UnmaskedClientFrameIsProtocolError) {
	tr.push_data(cframe(0x1, "hi", true, /*mask=*/false));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
	EXPECT_TRUE(log.messages.empty());
}

TEST_F(WsSessionTest, ReservedBitsAreProtocolError) {
	tr.push_data(cframe(0x1, "hi", true, true, /*rsv=*/0x4));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, ReservedOpcodesAreProtocolError) {
	for (uint8_t op : { 0x3, 0x7, 0xB, 0xF }) {
		ScriptedTransport t2;
		Log l2;
		ws::Handlers h2 = h;
		h2.user = &l2;
		t2.push_data(cframe(op, "x"));
		std::vector<uint8_t> rx(256), tx(256);
		HttpRequest req;
		ws::serve(t2.transport(), h2, req, rx, tx, {});
		auto f = sframes(t2.tx);
		ASSERT_EQ(f.size(), 1u) << int(op);
		EXPECT_EQ(f[0].close_code(), ws::close_code::protocol_error) << int(op);
	}
}

TEST_F(WsSessionTest, FragmentedControlFrameIsProtocolError) {
	tr.push_data(cframe(0x9, "x", /*fin=*/false));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, OversizedControlFrameIsProtocolError) {
	std::string f;
	f += char(0x89);
	f += char(0x80 | 126);
	f += char(0); f += char(200);
	f += std::string(4, '\0');
	f += std::string(200, 'x');
	tr.push_data(f);
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, ContinuationWithoutStartIsProtocolError) {
	tr.push_data(cframe(0x0, "x", true));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, NewDataFrameDuringFragmentationIsProtocolError) {
	tr.push_data(cframe(0x1, "A", false));
	tr.push_data(cframe(0x1, "B", true));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
	EXPECT_TRUE(log.messages.empty());
}

TEST_F(WsSessionTest, NonMinimalLengthIsProtocolError) {
	std::string f;
	f += char(0x81);
	f += char(0x80 | 126);
	f += char(0); f += char(5);
	f += std::string(4, '\0');
	f += std::string(5, 'a');
	tr.push_data(f);
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, InvalidUtf8TextIs1007) {
	tr.push_data(cframe(0x1, std::string("\xC0\x80", 2)));
	run();
	EXPECT_FAILS_WITH(ws::close_code::invalid_payload);
	EXPECT_TRUE(log.messages.empty());
}

TEST_F(WsSessionTest, InvalidUtf8IsAllowedInBinaryMessages) {
	tr.push_data(cframe(0x2, std::string("\xC0\x80", 2)));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(log.messages.size(), 1u);
}

TEST_F(WsSessionTest, FrameAboveTheLimitIs1009BeforeThePayloadIsRead) {
	// Only the header of a huge frame is on the wire: the session must reject
	// it from the header alone instead of waiting for (and buffering) the body.
	std::string f;
	f += char(0x82);
	f += char(0x80 | 127);
	for (int i = 7; i >= 0; --i) f += char((uint64_t(1) << 30 >> (i * 8)) & 0xFF);
	f += std::string(4, '\0');
	tr.push_data(f);
	run();
	EXPECT_FAILS_WITH(ws::close_code::too_big);
}

TEST_F(WsSessionTest, FailureDrainsWhatThePeerStillHasInFlight) {
	// Regression: closing a TCP socket that still holds unread data makes the
	// stack send RST, which can wipe the Close frame out of the peer's receive
	// path. After a failure the session must therefore swallow the leftovers.
	tr.push_data(cframe(0x1, "hi", true, /*mask=*/false));   // protocol error
	tr.push_data(std::string(100, 'x'));                      // rest of the junk
	tr.push_data(std::string(100, 'y'));
	run();
	EXPECT_EQ(tr.next, tr.script.size()) << "unread data left in the socket";
	EXPECT_EQ(out().size(), 1u);
}

TEST_F(WsSessionTest, ReassembledMessageAboveTheLimitIs1009) {
	// rx 256 -> message capacity 112. Three 50 byte fragments exceed it.
	std::string frag(50, 'f');
	tr.push_data(cframe(0x2, frag, false));
	tr.push_data(cframe(0x0, frag, false));
	tr.push_data(cframe(0x0, frag, true));
	run();
	EXPECT_FAILS_WITH(ws::close_code::too_big);
	EXPECT_TRUE(log.messages.empty());
}

TEST_F(WsSessionTest, ControlFramesStillFitWhileAMessageIsAlmostFull) {
	// 100 of 112 bytes assembled; a maximal Ping must still be received.
	tr.push_data(cframe(0x2, std::string(100, 'a'), false));
	tr.push_data(cframe(0x9, std::string(125, 'p')));
	tr.push_data(cframe(0x0, std::string(12, 'b'), true));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	ASSERT_EQ(log.messages.size(), 1u);
	EXPECT_EQ(log.messages[0].size(), std::string("binary:").size() + 112);
	auto f = out();
	ASSERT_GE(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0xA);
	EXPECT_EQ(f[0].payload.size(), 125u);
}

// ===========================================================================
// Session: close handshake
// ===========================================================================

TEST_F(WsSessionTest, CloseWithoutPayloadIsAnsweredWithEmptyClose) {
	tr.push_data(cframe(0x8, ""));
	run();
	auto f = out();
	ASSERT_EQ(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0x8);
	EXPECT_TRUE(f[0].payload.empty());
	EXPECT_TRUE(log.has("close:1005"));
}

TEST_F(WsSessionTest, CloseEchoesCodeButNotTheReason) {
	tr.push_data(cframe(0x8, close_payload(1001, "bye bye")));
	run();
	auto f = out();
	ASSERT_EQ(f.size(), 1u);
	EXPECT_EQ(f[0].close_code(), 1001);
	EXPECT_EQ(f[0].payload.size(), 2u);
	EXPECT_TRUE(log.has("close:1001"));
}

TEST_F(WsSessionTest, ApplicationCodesInTheRegisteredRangeAreAccepted) {
	tr.push_data(cframe(0x8, close_payload(4242)));
	run();
	EXPECT_TRUE(log.has("close:4242"));
	EXPECT_EQ(out()[0].close_code(), 4242);
}

TEST_F(WsSessionTest, OneBytePayloadInCloseIsProtocolError) {
	tr.push_data(cframe(0x8, std::string(1, '\x03')));
	run();
	EXPECT_FAILS_WITH(ws::close_code::protocol_error);
}

TEST_F(WsSessionTest, ReservedCloseCodeIsProtocolError) {
	for (uint16_t code : { 1005, 1006, 1015, 999, 2999, 5000 }) {
		ScriptedTransport t2;
		Log l2;
		ws::Handlers h2 = h;
		h2.user = &l2;
		t2.push_data(cframe(0x8, close_payload(code)));
		std::vector<uint8_t> rx(256), tx(256);
		HttpRequest req;
		ws::serve(t2.transport(), h2, req, rx, tx, {});
		auto f = sframes(t2.tx);
		ASSERT_EQ(f.size(), 1u) << code;
		EXPECT_EQ(f[0].close_code(), ws::close_code::protocol_error) << code;
	}
}

TEST_F(WsSessionTest, InvalidUtf8InCloseReasonIs1007) {
	tr.push_data(cframe(0x8, close_payload(1000, std::string("\xC0\x80", 2))));
	run();
	EXPECT_FAILS_WITH(ws::close_code::invalid_payload);
}

TEST_F(WsSessionTest, ServerInitiatedCloseWaitsForThePeersClose) {
	h.on_message = [](ws::Session &s, ws::Opcode, std::span<const uint8_t>) {
		rec_message(s, ws::Opcode::Text, {});
		s.close(1000, "bye");
	};
	tr.push_data(cframe(0x1, "trigger"));
	tr.push_data(cframe(0x1, "ignored while closing"));   // drained, not delivered
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();

	auto f = out();
	ASSERT_EQ(f.size(), 1u);                 // only our own Close, no second one
	EXPECT_EQ(f[0].close_code(), 1000);
	EXPECT_EQ(f[0].payload.substr(2), "bye");
	EXPECT_EQ(log.messages.size(), 1u);
	EXPECT_TRUE(log.has("close:1000"));
	EXPECT_EQ(log.closes(), 1);
}

TEST_F(WsSessionTest, ServerInitiatedCloseGivesUpWhenPeerStaysSilent) {
	h.on_open = [](ws::Session &s, const HttpRequest &) { s.close(1001); };
	tr.push_timeout();                       // peer never answers
	run();
	EXPECT_TRUE(log.has("close:1006"));
	EXPECT_EQ(log.closes(), 1);
}

TEST_F(WsSessionTest, SendIsRefusedAfterClose) {
	bool refused = false;
	struct Ref { bool *p; } ref { &refused };
	h.user = &ref;
	h.on_open = nullptr;       // the fixture's recorders expect user() to be a Log
	h.on_close = nullptr;
	h.on_message = [](ws::Session &s, ws::Opcode, std::span<const uint8_t>) {
		s.close();
		*static_cast<Ref *>(s.user())->p = !s.send_text("late");
	};
	tr.push_data(cframe(0x1, "go"));
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_TRUE(refused);
}

TEST_F(WsSessionTest, PeerDisappearingWithoutCloseIs1006) {
	tr.push_data(cframe(0x1, "hi"));
	tr.push_eof();
	run();
	EXPECT_TRUE(log.has("close:1006"));
	EXPECT_EQ(log.closes(), 1);
	EXPECT_TRUE(out().empty());              // nothing to say to a gone peer
}

TEST_F(WsSessionTest, TruncatedFrameThenEofIs1006) {
	std::string f = cframe(0x1, "hello");
	tr.push_data(f.substr(0, f.size() - 2));
	tr.push_eof();
	run();
	EXPECT_TRUE(log.has("close:1006"));
	EXPECT_TRUE(log.messages.empty());
}

TEST_F(WsSessionTest, WriteFailureEndsTheSessionWith1006) {
	h.on_message = rec_echo;
	tr.fail_writes = true;
	tr.push_data(cframe(0x1, "echo me"));
	run();
	EXPECT_TRUE(log.has("close:1006"));
	EXPECT_EQ(log.closes(), 1);
}

TEST_F(WsSessionTest, OnCloseIsCalledExactlyOnce) {
	tr.push_data(cframe(0x8, close_payload(1000)));
	tr.push_eof();
	run();
	EXPECT_EQ(log.closes(), 1);
}

// ===========================================================================
// Session: idle ticks and keep-alive
// ===========================================================================

TEST_F(WsSessionTest, IdleCallbackRunsOncePerTimeout) {
	h.on_idle = rec_idle;
	h.idle_interval_ms = 10;
	h.ping_interval_ms = 1000000;   // keep-alive out of the way
	tr.push_timeout();
	tr.push_timeout();
	tr.push_timeout();
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(std::count(log.events.begin(), log.events.end(), "idle"), 3);
}

TEST_F(WsSessionTest, IdleCallbackCanPushData) {
	h.on_idle = [](ws::Session &s) { s.send_text("tick"); };
	h.idle_interval_ms = 10;
	h.ping_interval_ms = 1000000;
	tr.push_timeout();
	tr.push_timeout();
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	auto f = out();
	ASSERT_GE(f.size(), 2u);
	EXPECT_EQ(f[0].payload, "tick");
	EXPECT_EQ(f[1].payload, "tick");
}

TEST_F(WsSessionTest, SilenceTriggersPingAndThenDropsTheDeadPeer) {
	h.idle_interval_ms = 10;
	h.ping_interval_ms = 20;         // 2 ticks of silence
	tr.push_timeout();               // 10 ms
	tr.push_timeout();               // 20 ms -> Ping
	tr.push_timeout();               // 10 ms
	tr.push_timeout();               // 20 ms, still no Pong -> dropped
	run();
	auto f = out();
	ASSERT_EQ(f.size(), 1u);
	EXPECT_EQ(f[0].opcode, 0x9);
	EXPECT_TRUE(log.has("close:1006"));
}

TEST_F(WsSessionTest, AnyTrafficCountsAsLifeSign) {
	h.idle_interval_ms = 10;
	h.ping_interval_ms = 20;
	tr.push_timeout();                       // 10 ms
	tr.push_timeout();                       // 20 ms -> Ping
	tr.push_data(cframe(0xA, ""));           // Pong arrives
	tr.push_timeout();
	tr.push_timeout();                       // next Ping, not a drop
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	int pings = 0;
	for (auto &f : out()) pings += (f.opcode == 0x9);
	EXPECT_EQ(pings, 2);
	EXPECT_TRUE(log.has("close:1000"));
}

TEST_F(WsSessionTest, ZeroIntervalsFallBackToTheDefaultsInsteadOfSpinning) {
	h.idle_interval_ms = 0;
	h.ping_interval_ms = 0;
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_TRUE(log.has("close:1000"));
}

TEST_F(WsSessionTest, SessionAdvertisesItsLimits) {
	struct Cap { size_t v { 0 }; } cap;
	h.user = &cap;
	h.on_open = [](ws::Session &s, const HttpRequest &) {
		static_cast<Cap *>(s.user())->v = s.max_message_size();
	};
	h.on_close = nullptr;
	tr.push_data(cframe(0x8, close_payload(1000)));
	run();
	EXPECT_EQ(cap.v, rx_size - UCHTTP_WS_CONTROL_RESERVE);
}

// ===========================================================================
// Allocator
// ===========================================================================

TEST(WsAllocator, StaticAllocatorHandsOutAndRecyclesBlocks) {
	ws::StaticAllocator<2, 64> a;
	void *p1 = a.allocate(64);
	void *p2 = a.allocate(10);
	ASSERT_NE(p1, nullptr);
	ASSERT_NE(p2, nullptr);
	EXPECT_NE(p1, p2);
	EXPECT_EQ(a.allocate(1), nullptr);          // exhausted
	a.deallocate(p1, 64);
	EXPECT_EQ(a.allocate(64), p1);              // recycled
}

TEST(WsAllocator, StaticAllocatorRefusesOversizedRequests) {
	ws::StaticAllocator<1, 64> a;
	EXPECT_EQ(a.allocate(65), nullptr);
	EXPECT_NE(a.allocate(64), nullptr);
}

TEST(WsAllocator, StaticAllocatorBlocksAreAligned) {
	ws::StaticAllocator<2, 33> a;
	EXPECT_EQ(reinterpret_cast<uintptr_t>(a.allocate(1)) % 8, 0u);
}

static_assert(ws::AllocatorConcept<ws::StaticAllocator<1, 8>>);
static_assert(ws::Sha1Concept<ws::SoftSha1>);

// ===========================================================================
// End to end through HttpConnection::process()
// ===========================================================================

namespace {

// In-memory socket: scripted RX, TX captured outside the connection's reach.
class WsSock {
public:
	WsSock() = default;
	WsSock(std::string_view rx, std::string &tx, unsigned *timeout_seen = nullptr)
		: rx_(rx), tx_(&tx), timeout_seen_(timeout_seen) {}

	int read(std::span<uint8_t> buf) noexcept
	{
		if (pos_ >= rx_.size()) return 0;
		size_t n = std::min(rx_.size() - pos_, buf.size());
		std::memcpy(buf.data(), rx_.data() + pos_, n);
		pos_ += n;
		return static_cast<int>(n);
	}
	int write(std::span<const uint8_t> buf) noexcept
	{
		if (tx_) tx_->append(reinterpret_cast<const char *>(buf.data()), buf.size());
		return static_cast<int>(buf.size());
	}
	void close() noexcept { valid_ = false; }
	bool is_valid() const noexcept { return valid_; }
	bool set_read_timeout(unsigned ms) noexcept
	{
		if (timeout_seen_) *timeout_seen_ = ms;
		return true;
	}

private:
	std::string  rx_;
	size_t       pos_ { 0 };
	std::string *tx_ { nullptr };
	unsigned    *timeout_seen_ { nullptr };
	bool         valid_ { true };
};

int g_sha1_starts = 0;   // counts SHA-1 provider use (see CustomSha1Provider test)

using DefaultAlloc = ws::StaticAllocator<1, UCHTTP_WS_BUFFER_SIZE>;
using DefaultWs    = ws::Traits<ws::SoftSha1, DefaultAlloc>;

std::string upgrade_request(std::string_view path = "/ws",
			    std::string_view extra = {},
			    std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==")
{
	return "GET " + std::string(path) + " HTTP/1.1\r\n"
	       "Host: localhost\r\n"
	       "Upgrade: websocket\r\n"
	       "Connection: Upgrade\r\n"
	       "Sec-WebSocket-Version: 13\r\n"
	       "Sec-WebSocket-Key: " + std::string(key) + "\r\n" +
	       std::string(extra) + "\r\n";
}

// Run one connection and return everything the server wrote.
template <typename Traits = DefaultWs, typename Alloc = DefaultAlloc>
std::string run_conn(const RouteTable<4> &routes, std::string_view rx, Alloc &alloc,
		     const MiddlewareFn *mw = nullptr, size_t mwn = 0,
		     unsigned *timeout_seen = nullptr)
{
	std::string tx;
	HttpConnection<WsSock> conn(WsSock(rx, tx, timeout_seen));
	conn.template process<4, Traits>(routes, mw, mwn, &alloc);
	return tx;
}

std::string body_after_headers(const std::string &tx)
{
	auto p = tx.find("\r\n\r\n");
	return p == std::string::npos ? std::string() : tx.substr(p + 4);
}

} // namespace

TEST(WsEndToEnd, UpgradeEchoClose) {
	ws::Handlers h {};
	h.on_message = rec_echo;
	Log log;
	h.user = &log;
	h.on_close = rec_close;

	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));

	DefaultAlloc alloc;
	std::string raw = upgrade_request() + cframe(0x1, "hi") +
			  cframe(0x8, close_payload(1000));
	std::string tx = run_conn(routes, raw, alloc);

	EXPECT_EQ(tx.rfind("HTTP/1.1 101 Switching Protocols\r\n", 0), 0u);
	EXPECT_NE(tx.find("Upgrade: websocket\r\n"), std::string::npos);
	EXPECT_NE(tx.find("Connection: Upgrade\r\n"), std::string::npos);
	EXPECT_NE(tx.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
		  std::string::npos);
	// A 101 carries no body framing.
	EXPECT_EQ(tx.find("Content-Length"), std::string::npos);

	auto f = sframes(body_after_headers(tx));
	ASSERT_GE(f.size(), 2u);
	EXPECT_EQ(f[0].opcode, 0x1);
	EXPECT_EQ(f[0].payload, "hi");
	EXPECT_EQ(f[1].opcode, 0x8);
	EXPECT_TRUE(log.has("close:1000"));
}

TEST(WsEndToEnd, FramesPipelinedBehindTheRequestAreNotLost) {
	// The very same read() that carries the HTTP request also carries frames.
	ws::Handlers h {};
	Log log;
	h.user = &log;
	h.on_message = rec_message;
	h.on_close = rec_close;
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;

	std::string raw = upgrade_request() + cframe(0x1, "one") + cframe(0x1, "two");
	run_conn(routes, raw, alloc);
	EXPECT_EQ(log.messages, (std::vector<std::string>{ "text:one", "text:two" }));
}

TEST(WsEndToEnd, OnOpenSeesTheRequest) {
	struct Seen { std::string path, query; } seen;
	ws::Handlers h {};
	h.user = &seen;
	h.on_open = [](ws::Session &s, const HttpRequest &req) {
		auto *p = static_cast<Seen *>(s.user());
		p->path  = std::string(req.path);
		p->query = std::string(req.query_param("token"));
	};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	run_conn(routes, upgrade_request("/ws?token=abc123"), alloc);
	EXPECT_EQ(seen.path, "/ws");
	EXPECT_EQ(seen.query, "abc123");
}

TEST(WsEndToEnd, PathParametersWorkForWebSocketRoutes) {
	struct Seen { std::string id; } seen;
	ws::Handlers h {};
	h.user = &seen;
	h.on_open = [](ws::Session &s, const HttpRequest &req) {
		static_cast<Seen *>(s.user())->id = std::string(req.path_param("id"));
	};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws/{id}", h));
	DefaultAlloc alloc;
	run_conn(routes, upgrade_request("/ws/sensor-7"), alloc);
	EXPECT_EQ(seen.id, "sensor-7");
}

TEST(WsEndToEnd, MissingUpgradeHeadersGive426WithHints) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string tx = run_conn(routes,
		"GET /ws HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 426 Upgrade Required\r\n", 0), 0u);
	EXPECT_NE(tx.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
	EXPECT_NE(tx.find("Upgrade: websocket\r\n"), std::string::npos);
}

TEST(WsEndToEnd, UnsupportedVersionGives426) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string raw =
		"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Version: 8\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
	std::string tx = run_conn(routes, raw, alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 426", 0), 0u);
	EXPECT_EQ(tx.find("Sec-WebSocket-Accept"), std::string::npos);
}

TEST(WsEndToEnd, BadKeyGives400) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string tx = run_conn(routes, upgrade_request("/ws", {}, "short"), alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 400", 0), 0u);
}

TEST(WsEndToEnd, LowercaseHeaderNamesAndKeepAliveUpgradeTokenWork) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string raw =
		"GET /ws HTTP/1.1\r\nhost: x\r\nupgrade: websocket\r\n"
		"connection: keep-alive, Upgrade\r\nsec-websocket-version: 13\r\n"
		"sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
	std::string tx = run_conn(routes, raw, alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 101", 0), 0u);
}

TEST(WsEndToEnd, PostToAWebSocketRouteIs405) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string tx = run_conn(routes,
		"POST /ws HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
		"Connection: close\r\n\r\n", alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 405", 0), 0u);
}

TEST(WsEndToEnd, MiddlewareCanShortCircuitTheUpgrade) {
	Log log;
	ws::Handlers h {};
	h.user = &log;
	h.on_open = rec_open;
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;

	MiddlewareFn mw[] = { +[](const HttpRequest &, HttpResponse &res, MiddlewareChain &) {
		res.set_status(401);          // no chain.next(): the upgrade never happens
		res.set_body("denied");
	} };
	std::string tx = run_conn(routes, upgrade_request(), alloc, mw, 1);
	EXPECT_EQ(tx.rfind("HTTP/1.1 401", 0), 0u);
	EXPECT_EQ(tx.find("Sec-WebSocket-Accept"), std::string::npos);
	EXPECT_FALSE(log.has("open"));
}

TEST(WsEndToEnd, MiddlewareHeadersAreAddedToThe101) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;

	MiddlewareFn mw[] = { +[](const HttpRequest &req, HttpResponse &res, MiddlewareChain &c) {
		c.next(req, res);
		res.add_header("X-Frame-Options", "DENY");
	} };
	std::string tx = run_conn(routes, upgrade_request(), alloc, mw, 1);
	EXPECT_EQ(tx.rfind("HTTP/1.1 101", 0), 0u);
	EXPECT_NE(tx.find("X-Frame-Options: DENY\r\n"), std::string::npos);
}

TEST(WsEndToEnd, AcceptGateCanRefuseWith403) {
	Log log;
	ws::Handlers h {};
	h.user = &log;
	h.on_open = rec_open;
	h.accept = [](const HttpRequest &req) {
		return ws::find_header(req, "Origin") == "https://good.example";
	};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;

	std::string bad = run_conn(routes,
		upgrade_request("/ws", "Origin: https://evil.example\r\n"), alloc);
	EXPECT_EQ(bad.rfind("HTTP/1.1 403", 0), 0u);
	EXPECT_FALSE(log.has("open"));

	std::string good = run_conn(routes,
		upgrade_request("/ws", "Origin: https://good.example\r\n"), alloc);
	EXPECT_EQ(good.rfind("HTTP/1.1 101", 0), 0u);
	EXPECT_TRUE(log.has("open"));
}

TEST(WsEndToEnd, SubprotocolIsNegotiated) {
	ws::Handlers h {};
	h.subprotocol = "telemetry.v1";
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;

	std::string yes = run_conn(routes,
		upgrade_request("/ws", "Sec-WebSocket-Protocol: chat, telemetry.v1\r\n"), alloc);
	EXPECT_NE(yes.find("Sec-WebSocket-Protocol: telemetry.v1\r\n"), std::string::npos);

	std::string no = run_conn(routes,
		upgrade_request("/ws", "Sec-WebSocket-Protocol: chat\r\n"), alloc);
	EXPECT_EQ(no.find("Sec-WebSocket-Protocol"), std::string::npos);
	EXPECT_EQ(no.rfind("HTTP/1.1 101", 0), 0u);   // no match is the client's call
}

TEST(WsEndToEnd, ExhaustedAllocatorGives503) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	void *held = alloc.allocate(UCHTTP_WS_BUFFER_SIZE);
	ASSERT_NE(held, nullptr);
	std::string tx = run_conn(routes, upgrade_request(), alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 503", 0), 0u);
	EXPECT_EQ(tx.find("Sec-WebSocket-Accept"), std::string::npos);
}

TEST(WsEndToEnd, MissingAllocatorGives503) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	std::string tx;
	HttpConnection<WsSock> conn(WsSock(upgrade_request(), tx));
	conn.process(routes);       // no allocator passed
	EXPECT_EQ(tx.rfind("HTTP/1.1 503", 0), 0u);
}

TEST(WsEndToEnd, SessionBufferIsReturnedToTheAllocator) {
	struct CountingAlloc {
		int allocs { 0 }, frees { 0 };
		alignas(8) uint8_t block[UCHTTP_WS_BUFFER_SIZE];
		void *allocate(size_t n) noexcept { ++allocs; return n <= sizeof(block) ? block : nullptr; }
		void deallocate(void *, size_t) noexcept { ++frees; }
	};
	static_assert(ws::AllocatorConcept<CountingAlloc>);
	using Tr = ws::Traits<ws::SoftSha1, CountingAlloc>;

	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	CountingAlloc alloc;
	run_conn<Tr>(routes, upgrade_request() + cframe(0x8, close_payload(1000)), alloc);
	EXPECT_EQ(alloc.allocs, 1);
	EXPECT_EQ(alloc.frees, 1);
}

TEST(WsEndToEnd, CustomSha1ProviderIsUsedForTheHandshake) {
	// A port's hardware/crypto-library SHA-1 plugs in through the concept.
	struct CountingSha1 {
		ws::SoftSha1 impl;
		bool start() noexcept { ++g_sha1_starts; return impl.start(); }
		bool update(std::span<const uint8_t> d) noexcept { return impl.update(d); }
		bool finish(std::span<uint8_t, 20> o) noexcept { return impl.finish(o); }
	};
	static_assert(ws::Sha1Concept<CountingSha1>);
	using Tr = ws::Traits<CountingSha1, DefaultAlloc>;

	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	g_sha1_starts = 0;
	std::string tx = run_conn<Tr>(routes, upgrade_request(), alloc);
	EXPECT_EQ(g_sha1_starts, 1);
	EXPECT_NE(tx.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
}

TEST(WsEndToEnd, FailingSha1ProviderGives500) {
	struct BrokenSha1 {
		bool start() noexcept { return false; }
		bool update(std::span<const uint8_t>) noexcept { return true; }
		bool finish(std::span<uint8_t, 20>) noexcept { return true; }
	};
	using Tr = ws::Traits<BrokenSha1, DefaultAlloc>;
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	std::string tx = run_conn<Tr>(routes, upgrade_request(), alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 500", 0), 0u);
}

TEST(WsEndToEnd, SocketReadTimeoutIsSetFromTheHandlers) {
	ws::Handlers h {};
	h.idle_interval_ms = 250;
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	DefaultAlloc alloc;
	unsigned seen = 0;
	run_conn(routes, upgrade_request(), alloc, nullptr, 0, &seen);
	EXPECT_EQ(seen, 250u);
}

TEST(WsEndToEnd, OrdinaryRoutesKeepWorkingNextToWebSocket) {
	ws::Handlers h {};
	RouteTable<4> routes;
	ASSERT_TRUE(routes.add_ws("/ws", h));
	ASSERT_TRUE(routes.add(HttpMethod::GET, "/api/hello",
		+[](const HttpRequest &, HttpResponse &res) {
			res.set_status(200);
			res.set_body("hi");
		}));
	DefaultAlloc alloc;
	std::string tx = run_conn(routes,
		"GET /api/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", alloc);
	EXPECT_EQ(tx.rfind("HTTP/1.1 200", 0), 0u);
	EXPECT_EQ(body_after_headers(tx), "hi");
}

TEST(WsEndToEnd, ServerApiRegistersWebSocketRoutes) {
	static ws::Handlers h;
	HttpServer<WsSock> server;
	EXPECT_TRUE(server.websocket("/ws", h));
	EXPECT_EQ(server.routes().size(), 1u);
	EXPECT_NE(server.routes()[0].ws, nullptr);
}

TEST(WsEndToEnd, ResponseTo101HasNoContentTypeOrBody) {
	char buf[512];
	HttpResponse res;
	res.bind(std::span<char>(buf, sizeof(buf)));
	res.set_status(101);
	res.add_header("Upgrade", "websocket");
	std::string tx;
	WsSock sock("", tx);
	ASSERT_TRUE(res.send(sock));
	EXPECT_EQ(tx, "HTTP/1.1 101 Switching Protocols\r\n"
		      "Connection: Upgrade\r\n"
		      "Upgrade: websocket\r\n\r\n");
}
