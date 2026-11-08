#pragma once

// zephyr_socket.h — Zephyr BSD-socket implementation of uchttp::SocketConcept
// and an acceptor, mirroring the Linux port. Requires CONFIG_POSIX_API=y and
// CONFIG_NET_SOCKETS=y so the standard socket names (socket/bind/listen/accept/
// recv/send/close) are available.
//
// The core library (uchttp.h) is OS-agnostic; this is the only Zephyr-specific
// glue needed to run HttpServer<ZephyrSocket> on the RTOS.

#include <cstdint>
#include <span>

namespace uchttp {

// ---------------------------------------------------------------------------
// ZephyrSocket — wraps an accepted socket descriptor.
// ---------------------------------------------------------------------------

class ZephyrSocket {
public:
    static constexpr int INVALID_FD = -1;

    explicit ZephyrSocket(int fd = INVALID_FD) noexcept;

    // Move-only
    ZephyrSocket(const ZephyrSocket&)            = delete;
    ZephyrSocket& operator=(const ZephyrSocket&) = delete;
    ZephyrSocket(ZephyrSocket&& o) noexcept;
    ZephyrSocket& operator=(ZephyrSocket&& o) noexcept;

    ~ZephyrSocket();

    // SocketConcept interface
    int  read (std::span<uint8_t>       buf) noexcept;
    int  write(std::span<const uint8_t> buf) noexcept;
    void close() noexcept;

    // Slowloris protection (requirement #4): drop the connection if the peer
    // stalls for longer than `ms` between reads. 0 disables the timeout.
    bool set_read_timeout(unsigned ms) noexcept;

    bool is_valid() const noexcept;
    int  fd()       const noexcept;

private:
    int fd_;
};

// ---------------------------------------------------------------------------
// ZephyrAcceptor — listens on a TCP port, produces ZephyrSocket instances.
// ---------------------------------------------------------------------------

class ZephyrAcceptor {
public:
    explicit ZephyrAcceptor(uint16_t port, int backlog = 4) noexcept;
    ~ZephyrAcceptor();

    bool start() noexcept;
    void stop()  noexcept;

    // AcceptorConcept interface
    ZephyrSocket accept()     noexcept;
    bool         is_running() const noexcept;

private:
    uint16_t port_;
    int      backlog_;
    int      listen_fd_ { -1 };
    bool     running_   { false };
};

} // namespace uchttp
