#include "uchttp_ws.h"

#include <cstring>

namespace uchttp::ws {

/* ------------------------------------------------------------------------ */
/* Frame codec                                                              */
/* ------------------------------------------------------------------------ */

ParseResult parse_header(std::span<const uint8_t> in, FrameHeader &out) noexcept
{
	if (in.size() < 2) return ParseResult::need_more;

	uint8_t b0 = in[0];
	uint8_t b1 = in[1];

	out.fin    = (b0 & 0x80) != 0;
	out.rsv    = (b0 >> 4) & 0x07;
	out.opcode = b0 & 0x0F;
	out.masked = (b1 & 0x80) != 0;

	uint8_t len7 = b1 & 0x7F;
	size_t  pos  = 2;

	uint64_t len;
	if (len7 <= 125) {
		len = len7;
	} else if (len7 == 126) {
		if (in.size() < pos + 2) return ParseResult::need_more;
		len = (static_cast<uint64_t>(in[pos]) << 8) | in[pos + 1];
		if (len < 126) return ParseResult::error;   /* non-minimal encoding */
		pos += 2;
	} else {
		if (in.size() < pos + 8) return ParseResult::need_more;
		len = 0;
		for (int i = 0; i < 8; ++i)
			len = (len << 8) | in[pos + i];
		if (len <= 0xFFFF) return ParseResult::error;
		if (len & (uint64_t{1} << 63)) return ParseResult::error; /* MSB must be 0 */
		pos += 8;
	}

	if (out.masked) {
		if (in.size() < pos + 4) return ParseResult::need_more;
		for (int i = 0; i < 4; ++i) out.mask[i] = in[pos + i];
		pos += 4;
	}

	if (pos > 0xFF) return ParseResult::error;   /* cannot happen, defensive */

	out.payload_len = len;
	out.header_len  = static_cast<uint8_t>(pos);
	return ParseResult::ok;
}

size_t encode_header(uint8_t *out, Opcode op, uint64_t payload_len, bool fin,
		     const uint8_t *mask_key) noexcept
{
	size_t pos = 0;
	out[pos++] = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
					  (static_cast<uint8_t>(op) & 0x0F));

	uint8_t mask_bit = mask_key ? 0x80 : 0x00;
	if (payload_len <= 125) {
		out[pos++] = static_cast<uint8_t>(mask_bit | payload_len);
	} else if (payload_len <= 0xFFFF) {
		out[pos++] = static_cast<uint8_t>(mask_bit | 126);
		out[pos++] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
		out[pos++] = static_cast<uint8_t>(payload_len & 0xFF);
	} else {
		out[pos++] = static_cast<uint8_t>(mask_bit | 127);
		for (int i = 7; i >= 0; --i)
			out[pos++] = static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF);
	}

	if (mask_key) {
		for (int i = 0; i < 4; ++i) out[pos++] = mask_key[i];
	}
	return pos;
}

void apply_mask(uint8_t *data, size_t len, const uint8_t key[4]) noexcept
{
	for (size_t i = 0; i < len; ++i) data[i] ^= key[i & 3];
}

bool utf8_valid(std::span<const uint8_t> data) noexcept
{
	size_t i = 0, n = data.size();
	while (i < n) {
		uint8_t b0 = data[i];
		if (b0 < 0x80) { ++i; continue; }

		size_t   extra;
		uint32_t cp;
		uint32_t min_cp;
		if ((b0 & 0xE0) == 0xC0)      { extra = 1; cp = b0 & 0x1F; min_cp = 0x80; }
		else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; min_cp = 0x800; }
		else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; min_cp = 0x10000; }
		else return false;

		if (i + extra >= n) return false;
		for (size_t k = 1; k <= extra; ++k) {
			uint8_t b = data[i + k];
			if ((b & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (b & 0x3F);
		}
		if (cp < min_cp) return false;                 /* overlong */
		if (cp >= 0xD800 && cp <= 0xDFFF) return false; /* surrogate */
		if (cp > 0x10FFFF) return false;
		i += extra + 1;
	}
	return true;
}

bool close_code_valid(uint16_t code) noexcept
{
	if (code == close_code::no_status || code == close_code::abnormal)
		return false;   /* reserved, must never appear on the wire */
	if (code < 1000) return false;
	if (code >= 1000 && code <= 1011) {
		switch (code) {
		case 1004: case 1005: case 1006: case 1014: case 1015:
			return false;
		default:
			return true;
		}
	}
	return code >= 3000 && code <= 4999;   /* app/registered range */
}

size_t base64_encode(std::span<const uint8_t> in, char *out, size_t cap) noexcept
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	size_t need = ((in.size() + 2) / 3) * 4;
	if (need > cap) return 0;

	size_t o = 0, i = 0;
	while (i + 3 <= in.size()) {
		uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = tbl[(v >> 6) & 0x3F];
		out[o++] = tbl[v & 0x3F];
		i += 3;
	}
	size_t rem = in.size() - i;
	if (rem == 1) {
		uint32_t v = uint32_t(in[i]) << 16;
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = '=';
		out[o++] = '=';
	} else if (rem == 2) {
		uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = tbl[(v >> 6) & 0x3F];
		out[o++] = '=';
	}
	return o;
}

namespace {

int8_t base64_val(char c) noexcept
{
	if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
	if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
	if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

} // namespace

bool key_valid(std::string_view key) noexcept
{
	if (key.size() != 24) return false;
	if (key[22] != '=' || key[23] != '=') return false;
	for (size_t i = 0; i < 22; ++i)
		if (base64_val(key[i]) < 0) return false;
	/* Decoded 16 bytes must be exactly 24 base64 chars with "==" padding and
	 * the last real char's low 4 bits zero (16 bytes -> 2 leftover bits). */
	int8_t last = base64_val(key[21]);
	return (last & 0x0F) == 0;
}

/* ------------------------------------------------------------------------ */
/* SoftSha1 - FIPS 180-4, portable, no dependencies.                        */
/* ------------------------------------------------------------------------ */

namespace {
inline uint32_t rol(uint32_t v, int n) noexcept { return (v << n) | (v >> (32 - n)); }
}

bool SoftSha1::start() noexcept
{
	h_[0] = 0x67452301u; h_[1] = 0xEFCDAB89u; h_[2] = 0x98BADCFEu;
	h_[3] = 0x10325476u; h_[4] = 0xC3D2E1F0u;
	total_   = 0;
	buf_len_ = 0;
	return true;
}

void SoftSha1::block(const uint8_t *b) noexcept
{
	uint32_t w[80];
	for (int i = 0; i < 16; ++i) {
		w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) |
		       (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
	}
	for (int i = 16; i < 80; ++i)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
	for (int i = 0; i < 80; ++i) {
		uint32_t f, k;
		if (i < 20)      { f = (bb & c) | (~bb & d);        k = 0x5A827999u; }
		else if (i < 40) { f = bb ^ c ^ d;                  k = 0x6ED9EBA1u; }
		else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
		else             { f = bb ^ c ^ d;                  k = 0xCA62C1D6u; }

		uint32_t tmp = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(bb, 30); bb = a; a = tmp;
	}
	h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e;
}

bool SoftSha1::update(std::span<const uint8_t> data) noexcept
{
	total_ += data.size();
	size_t i = 0;
	if (buf_len_ > 0) {
		size_t take = std::min(data.size(), size_t{64} - buf_len_);
		std::memcpy(buf_ + buf_len_, data.data(), take);
		buf_len_ += take;
		i += take;
		if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
	}
	for (; i + 64 <= data.size(); i += 64) block(data.data() + i);
	size_t rest = data.size() - i;
	if (rest > 0) {
		std::memcpy(buf_, data.data() + i, rest);
		buf_len_ = rest;
	}
	return true;
}

bool SoftSha1::finish(std::span<uint8_t, 20> digest) noexcept
{
	uint64_t bit_len = total_ * 8;
	uint8_t  pad = 0x80;
	update(std::span<const uint8_t>(&pad, 1));

	uint8_t zero = 0;
	while (buf_len_ != 56) update(std::span<const uint8_t>(&zero, 1));

	uint8_t len_be[8];
	for (int i = 0; i < 8; ++i)
		len_be[i] = static_cast<uint8_t>((bit_len >> ((7 - i) * 8)) & 0xFF);
	/* Append the length directly, bypassing update()'s own length counting. */
	std::memcpy(buf_ + buf_len_, len_be, 8);
	block(buf_);

	for (int i = 0; i < 5; ++i) {
		digest[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFF);
		digest[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFF);
		digest[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFF);
		digest[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFF);
	}
	return true;
}

} // namespace uchttp::ws
