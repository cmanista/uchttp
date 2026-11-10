// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#include "uchttp_ws.h"

#include <algorithm>
#include <cstring>

namespace uchttp::ws {

/* ------------------------------------------------------------------------ */
/* Session - outgoing side                                                  */
/* ------------------------------------------------------------------------ */

Session::Session(const Transport &t, const Handlers &h, std::span<uint8_t> rx,
		 std::span<uint8_t> tx) noexcept
	: t_(t), h_(h), rx_(rx), tx_(tx), user_(h.user)
{
	/* Keep UCHTTP_WS_CONTROL_RESERVE bytes of rx_ free so that a control
	 * frame can always be received between fragments of a data message. */
	size_t reserve = std::min<size_t>(UCHTTP_WS_CONTROL_RESERVE, rx_.size());
	msg_cap_   = rx_.size() - reserve;
	max_frame_ = std::min<size_t>(UCHTTP_WS_MAX_FRAME_SIZE, msg_cap_);
}

bool Session::is_open() const noexcept { return state_ == State::open; }

bool Session::write_all(const uint8_t *p, size_t n) noexcept
{
	if (!t_.write) { broken_ = true; return false; }
	size_t sent = 0;
	while (sent < n) {
		int w = t_.write(t_.ctx, p + sent, n - sent);
		if (w <= 0) { broken_ = true; return false; }
		sent += static_cast<size_t>(w);
	}
	return true;
}

bool Session::send_frame(Opcode op, const uint8_t *p, size_t n) noexcept
{
	if (state_ == State::closed || broken_) return false;

	uint8_t hdr[max_header_len];
	size_t  hlen = encode_header(hdr, op, n);

	/* Small frames are coalesced into the TX scratch buffer so that they
	 * leave in one write (one TCP segment / one TLS record). Larger payloads
	 * are written straight from the caller's memory, no copy. */
	if (tx_.size() >= hlen + n) {
		std::memcpy(tx_.data(), hdr, hlen);
		if (n > 0) std::memcpy(tx_.data() + hlen, p, n);
		return write_all(tx_.data(), hlen + n);
	}
	if (!write_all(hdr, hlen)) return false;
	return n == 0 || write_all(p, n);
}

bool Session::send_text(std::string_view text) noexcept
{
	if (state_ != State::open) return false;
	return send_frame(Opcode::Text,
			  reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool Session::send_binary(std::span<const uint8_t> data) noexcept
{
	if (state_ != State::open) return false;
	return send_frame(Opcode::Binary, data.data(), data.size());
}

bool Session::ping(std::span<const uint8_t> data) noexcept
{
	if (state_ != State::open) return false;
	return send_frame(Opcode::Ping, data.data(),
			  std::min(data.size(), max_control_len));
}

bool Session::close(uint16_t code, std::string_view reason) noexcept
{
	if (state_ != State::open) return false;

	uint8_t payload[max_control_len];
	size_t  n = 0;
	payload[n++] = static_cast<uint8_t>((code >> 8) & 0xFF);
	payload[n++] = static_cast<uint8_t>(code & 0xFF);
	size_t rn = std::min(reason.size(), max_control_len - 2);
	if (rn > 0) std::memcpy(payload + n, reason.data(), rn);
	n += rn;

	bool ok = send_frame(Opcode::Close, payload, n);
	state_ = State::closing;
	return ok;
}

void Session::fail(uint16_t code) noexcept
{
	/* Best effort Close frame; the connection ends regardless. */
	uint8_t payload[2] = {
		static_cast<uint8_t>((code >> 8) & 0xFF),
		static_cast<uint8_t>(code & 0xFF),
	};
	if (state_ == State::open && send_frame(Opcode::Close, payload, 2))
		drain();
	finish(code);
}

/* Lingering close. The failure that got us here usually leaves unread bytes
 * in the socket (the rest of an oversized or malformed frame). Closing a TCP
 * socket with unread data makes the stack answer with RST, and a RST makes the
 * peer drop the Close frame we just sent, so it would only see "1006". Swallow
 * what the peer still has in flight - a bounded number of reads, each bounded
 * by the idle timeout - so the Close frame is delivered before the FIN. */
void Session::drain() noexcept
{
	if (!t_.read || broken_) return;
	for (unsigned i = 0; i < 16; ++i) {
		int n = t_.read(t_.ctx, rx_.data(), rx_.size());
		if (n <= 0) break;   /* EOF (peer saw the Close) or timeout */
	}
}

void Session::finish(uint16_t code) noexcept
{
	if (state_ == State::closed) return;
	state_ = State::closed;
	if (h_.on_close) h_.on_close(*this, code);
}

/* ------------------------------------------------------------------------ */
/* Session - the read / dispatch loop                                       */
/*                                                                          */
/* rx_ layout (single buffer, no copies for the common single-frame case):  */
/*                                                                          */
/*   [0, msg_len)          payload of the data message being reassembled    */
/*   [msg_len, filled)     raw bytes of the current / next frame            */
/* ------------------------------------------------------------------------ */

void Session::run(const HttpRequest &req, std::span<const uint8_t> leftover) noexcept
{
	const unsigned idle_ms = h_.idle_interval_ms ? h_.idle_interval_ms
						     : UCHTTP_WS_IDLE_INTERVAL_MS;
	/* The keep-alive can not be switched off: it is also what ends a
	 * session whose socket keeps failing without ever delivering EOF. */
	const unsigned ping_ms = h_.ping_interval_ms ? h_.ping_interval_ms
						     : UCHTTP_WS_PING_INTERVAL_MS;

	if (t_.set_timeout) t_.set_timeout(t_.ctx, idle_ms);

	uint8_t *const B = rx_.data();
	const size_t   S = rx_.size();
	size_t filled = 0;

	if (h_.on_open) h_.on_open(*this, req);
	if (state_ == State::closed) return;

	if (!leftover.empty()) {
		if (leftover.size() > S) { fail(close_code::too_big); return; }
		std::memcpy(B, leftover.data(), leftover.size());
		filled = leftover.size();
	}

	size_t  msg_len    = 0;
	bool    assembling = false;
	Opcode  msg_type   = Opcode::Text;

	unsigned silence_ms       = 0;
	bool     ping_outstanding = false;

	/* Read more bytes into B[filled, S). Idle timeouts are handled here:
	 * on_idle(), then Ping after `ping_ms` of silence and drop after the
	 * same time again. Returns false when the session is over. */
	auto fill = [&]() -> bool {
		for (;;) {
			if (state_ == State::closed) return false;
			if (broken_) { finish(close_code::abnormal); return false; }
			if (filled >= S) { fail(close_code::too_big); return false; }

			int n = t_.read ? t_.read(t_.ctx, B + filled, S - filled) : -1;
			if (n > 0) {
				filled += static_cast<size_t>(n);
				silence_ms = 0;
				ping_outstanding = false;
				return true;
			}
			if (n == 0) { finish(close_code::abnormal); return false; }

			/* n < 0: read timeout (or transient error) = one idle tick. */
			if (state_ == State::closing) {   /* peer never answered Close */
				finish(close_code::abnormal);
				return false;
			}
			if (h_.on_idle) h_.on_idle(*this);
			if (state_ == State::closed) return false;

			silence_ms += idle_ms;
			if (silence_ms >= ping_ms && state_ == State::open) {
				if (ping_outstanding) {
					finish(close_code::abnormal);
					return false;
				}
				ping();
				ping_outstanding = true;
				silence_ms = 0;
			}
		}
	};

	/* Deliver a complete data message to the application. */
	auto deliver = [&](Opcode type, uint8_t *data, size_t len) -> bool {
		if (type == Opcode::Text &&
		    !utf8_valid(std::span<const uint8_t>(data, len))) {
			fail(close_code::invalid_payload);
			return false;
		}
		if (h_.on_message)
			h_.on_message(*this, type, std::span<const uint8_t>(data, len));
		return true;
	};

	while (state_ != State::closed) {
		FrameHeader fh{};
		ParseResult pr = parse_header(
			std::span<const uint8_t>(B + msg_len, filled - msg_len), fh);
		if (pr == ParseResult::need_more) {
			if (!fill()) return;
			continue;
		}
		if (pr == ParseResult::error) { fail(close_code::protocol_error); return; }

		/* --- validate the header before reading the payload (DoS guard) -- */
		const bool is_control = fh.opcode >= 0x8;
		if (fh.rsv != 0 || !fh.masked) {            /* no extensions; client must mask */
			fail(close_code::protocol_error);
			return;
		}
		if ((fh.opcode > 0x2 && fh.opcode < 0x8) || fh.opcode > 0xA) {
			fail(close_code::protocol_error);       /* reserved opcode */
			return;
		}
		if (is_control && (!fh.fin || fh.payload_len > max_control_len)) {
			fail(close_code::protocol_error);
			return;
		}
		if (!is_control) {
			if (fh.payload_len > max_frame_ ||
			    msg_len + fh.payload_len > msg_cap_) {
				fail(close_code::too_big);
				return;
			}
		}

		const size_t plen  = static_cast<size_t>(fh.payload_len);
		const size_t total = fh.header_len + plen;
		if (filled - msg_len < total) {
			if (!fill()) return;
			continue;
		}

		uint8_t *frame   = B + msg_len;
		uint8_t *payload = frame + fh.header_len;
		const size_t rem = filled - msg_len - total;   /* bytes after this frame */
		apply_mask(payload, plen, fh.mask);

		switch (static_cast<Opcode>(fh.opcode)) {
		case Opcode::Ping:
			send_frame(Opcode::Pong, payload, plen);
			std::memmove(frame, frame + total, rem);
			filled -= total;
			break;

		case Opcode::Pong:
			std::memmove(frame, frame + total, rem);
			filled -= total;
			break;

		case Opcode::Close: {
			uint16_t code = close_code::no_status;
			if (plen == 1) { fail(close_code::protocol_error); return; }
			if (plen >= 2) {
				code = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
				if (!close_code_valid(code)) {
					fail(close_code::protocol_error);
					return;
				}
				if (plen > 2 && !utf8_valid(std::span<const uint8_t>(
							payload + 2, plen - 2))) {
					fail(close_code::invalid_payload);
					return;
				}
			}
			/* Answer with the peer's code (RFC 6455 5.5.1) unless we
			 * started the closing handshake ourselves. */
			if (state_ == State::open)
				send_frame(Opcode::Close, payload, plen >= 2 ? 2 : 0);
			finish(code);
			return;
		}

		case Opcode::Text:
		case Opcode::Binary:
		case Opcode::Continuation: {
			const Opcode op = static_cast<Opcode>(fh.opcode);

			if (state_ == State::closing) {          /* draining until Close */
				std::memmove(frame, frame + total, rem);
				filled -= total;
				break;
			}
			if (op == Opcode::Continuation) {
				if (!assembling) { fail(close_code::protocol_error); return; }
			} else {
				if (assembling)  { fail(close_code::protocol_error); return; }
				msg_type   = op;
				assembling = !fh.fin;
			}

			if (fh.fin && msg_len == 0) {
				/* Common case: the whole message is in this one frame;
				 * hand the payload over in place. */
				bool ok = deliver(op, payload, plen);
				std::memmove(B, frame + total, rem);
				filled = rem;
				if (!ok) return;
				break;
			}

			/* Fragment: append the payload to the message area and move
			 * the bytes that follow right behind it. */
			std::memmove(B + msg_len, payload, plen);
			std::memmove(B + msg_len + plen, frame + total, rem);
			msg_len += plen;
			filled   = msg_len + rem;

			if (fh.fin) {
				assembling = false;
				bool ok = deliver(msg_type, B, msg_len);
				std::memmove(B, B + msg_len, rem);
				filled  = rem;
				msg_len = 0;
				if (!ok) return;
			}
			break;
		}
		}
	}
}

/* ------------------------------------------------------------------------ */
/* serve() - public entry point                                             */
/* ------------------------------------------------------------------------ */

void serve(const Transport &t, const Handlers &h, const HttpRequest &req,
	   std::span<uint8_t> rx, std::span<uint8_t> tx,
	   std::span<const uint8_t> leftover) noexcept
{
	Session s(t, h, rx, tx);
	s.run(req, leftover);
}

} // namespace uchttp::ws
