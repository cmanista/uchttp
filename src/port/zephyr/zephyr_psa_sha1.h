#pragma once

/*
 * zephyr_psa_sha1.h - ws::Sha1Concept implemented with PSA Crypto.
 *
 * The WebSocket handshake needs one SHA-1 per connection. Going through the
 * PSA API lets the SoC's crypto engine do it where the PSA driver supports
 * SHA-1 (FRDM-RW612: ELS/PKC driver), and falls back to Mbed TLS' software
 * implementation otherwise - without the WebSocket core knowing anything about
 * either. Needs CONFIG_UCHTTP_WS_SHA1_PSA=y (which enables PSA_WANT_ALG_SHA_1).
 *
 * SHA-1 is used here only because RFC 6455 mandates it for Sec-WebSocket-
 * Accept; it protects nothing (the value just proves the server understood the
 * upgrade request).
 */

#include <cstdint>
#include <span>

#include <psa/crypto.h>

namespace uchttp {

class ZephyrPsaSha1 {
public:
	ZephyrPsaSha1() noexcept = default;
	~ZephyrPsaSha1();

	ZephyrPsaSha1(const ZephyrPsaSha1 &)            = delete;
	ZephyrPsaSha1 &operator=(const ZephyrPsaSha1 &) = delete;

	/* ws::Sha1Concept */
	bool start() noexcept;
	bool update(std::span<const uint8_t> data) noexcept;
	bool finish(std::span<uint8_t, 20> digest) noexcept;

private:
	psa_hash_operation_t op_ = PSA_HASH_OPERATION_INIT;
	bool                 active_ { false };
};

} // namespace uchttp
