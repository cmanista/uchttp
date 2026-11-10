// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

// Linux / POSIX TCP socket implementing uchttp::SocketConcept.
// Used for local testing on a host PC – NOT intended for MCU targets.

#include <cstdint>
#include <span>

namespace uchttp {

// ---------------------------------------------------------------------------
// LinuxSocket  – wraps an already-connected file descriptor
// ---------------------------------------------------------------------------

class LinuxSocket {
public:
    static constexpr int INVALID_FD = -1;

    explicit LinuxSocket(int fd = INVALID_FD) noexcept;

    // Move-only
    LinuxSocket(const LinuxSocket&)            = delete;
    LinuxSocket& operator=(const LinuxSocket&) = delete;
    LinuxSocket(LinuxSocket&& o) noexcept;
    LinuxSocket& operator=(LinuxSocket&& o) noexcept;

    ~LinuxSocket();

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
// LinuxAcceptor  – listens on a TCP port, produces LinuxSocket instances
// ---------------------------------------------------------------------------

class LinuxAcceptor {
public:
    explicit LinuxAcceptor(uint16_t port, int backlog = 5) noexcept;
    ~LinuxAcceptor();

    bool start() noexcept;
    void stop()  noexcept;

    // AcceptorConcept interface
    LinuxSocket accept()     noexcept;
    bool        is_running() const noexcept;

private:
    uint16_t port_;
    int      backlog_;
    int      listen_fd_ { -1 };
    bool     running_   { false };
};

} // namespace uchttp
