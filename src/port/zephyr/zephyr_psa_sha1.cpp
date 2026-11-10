// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#include "zephyr_psa_sha1.h"

namespace uchttp {

ZephyrPsaSha1::~ZephyrPsaSha1()
{
	if (active_) psa_hash_abort(&op_);
}

bool ZephyrPsaSha1::start() noexcept
{
	/* Idempotent. Zephyr normally initialises PSA at boot (CONFIG_MBEDTLS_INIT)
	 * but a WebSocket handshake must not depend on that. */
	if (psa_crypto_init() != PSA_SUCCESS) return false;

	if (active_) {
		psa_hash_abort(&op_);
		active_ = false;
	}
	op_ = PSA_HASH_OPERATION_INIT;
	if (psa_hash_setup(&op_, PSA_ALG_SHA_1) != PSA_SUCCESS) return false;
	active_ = true;
	return true;
}

bool ZephyrPsaSha1::update(std::span<const uint8_t> data) noexcept
{
	if (!active_) return false;
	if (psa_hash_update(&op_, data.data(), data.size()) != PSA_SUCCESS) {
		psa_hash_abort(&op_);
		active_ = false;
		return false;
	}
	return true;
}

bool ZephyrPsaSha1::finish(std::span<uint8_t, 20> digest) noexcept
{
	if (!active_) return false;
	size_t len = 0;
	psa_status_t st = psa_hash_finish(&op_, digest.data(), digest.size(), &len);
	active_ = false;      /* finish() ends the operation, success or not */
	return st == PSA_SUCCESS && len == 20;
}

} // namespace uchttp
