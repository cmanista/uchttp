#pragma once

/*
 * zephyr_ws.h - Zephyr plug-ins for the platform independent WebSocket core
 * (requirement #8):
 *
 *   ZephyrSlabAllocator<Blocks, BlockSize>   ws::AllocatorConcept on top of
 *                                            a Zephyr k_mem_slab
 *   ZephyrPsaSha1                            ws::Sha1Concept on top of the
 *                                            PSA Crypto API (zephyr_psa_sha1.h)
 *
 * Typical use:
 *
 *   using WsTraits = uchttp::ws::Traits<uchttp::ZephyrPsaSha1,
 *                                       uchttp::ZephyrSlabAllocator<1, UCHTTP_WS_BUFFER_SIZE>>;
 *   uchttp::HttpServer<uchttp::ZephyrSocket, UCHTTP_MAX_ROUTES,
 *                      UCHTTP_RX_BUFFER_SIZE, UCHTTP_TX_BUFFER_SIZE,
 *                      UCHTTP_MAX_MIDDLEWARE, UCHTTP_MAX_CONNECTIONS, WsTraits> server;
 *
 * Include "uchttp.h" BEFORE this header: llhttp.h declares a member called
 * `_current`, which collides with the `_current` macro of <zephyr/kernel.h>.
 */

#include <cstddef>
#include <cstdint>

#include "uchttp_ws.h"

#include <zephyr/kernel.h>

namespace uchttp {

/*
 * Session buffers served from a k_mem_slab: O(1) allocation, no heap, no
 * fragmentation, usable from any thread. The slab storage lives inside the
 * object, so declare it with static storage duration (or as a member of the
 * server). The slab is initialised on first use; call init() earlier if you
 * want that done at a defined point.
 */
template <size_t Blocks, size_t BlockSize>
class ZephyrSlabAllocator {
public:
	static constexpr size_t block_size() noexcept { return BlockSize; }

	void init() noexcept
	{
		if (!inited_) {
			k_mem_slab_init(&slab_, buf_, kBlock, Blocks);
			inited_ = true;
		}
	}

	void *allocate(size_t n) noexcept
	{
		if (n > BlockSize) return nullptr;
		init();
		void *mem = nullptr;
		if (k_mem_slab_alloc(&slab_, &mem, K_NO_WAIT) != 0) return nullptr;
		return mem;
	}

	void deallocate(void *p, size_t) noexcept
	{
		if (p) k_mem_slab_free(&slab_, p);
	}

	/* Blocks currently handed out. */
	size_t used() const noexcept
	{
		return inited_ ? k_mem_slab_num_used_get(
				const_cast<struct k_mem_slab *>(&slab_)) : 0;
	}

private:
	static constexpr size_t kBlock = (BlockSize + 7u) & ~size_t{7};  /* 8 aligned */

	struct k_mem_slab slab_;
	alignas(8) uint8_t buf_[Blocks * kBlock];
	bool inited_ { false };
};

static_assert(ws::AllocatorConcept<ZephyrSlabAllocator<1, 64>>);

} // namespace uchttp
