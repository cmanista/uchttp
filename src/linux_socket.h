#pragma once

// Linux / POSIX TCP socket implementing uchttp::SocketConcept.
// Used for local testing on a host PC – NOT intended for MCU targets.

#include <cstdint>
#include <span>
#include <cerrno>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>

namespace uchttp {

// ---------------------------------------------------------------------------
// LinuxSocket  – wraps an already-connected file descriptor
// ---------------------------------------------------------------------------

class LinuxSocket {
public:
    static constexpr int INVALID_FD = -1;

    explicit LinuxSocket(int fd = INVALID_FD) noexcept : fd_(fd) {}

    // Move-only (socket ownership)
    LinuxSocket(const LinuxSocket&)            = delete;
    LinuxSocket& operator=(const LinuxSocket&) = delete;

    LinuxSocket(LinuxSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = INVALID_FD; }
    LinuxSocket& operator=(LinuxSocket&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = INVALID_FD; }
        return *this;
    }

    ~LinuxSocket() { close(); }

    // SocketConcept interface ---------------------------------------------------

    int read(std::span<uint8_t> buf) noexcept {
        if (fd_ == INVALID_FD) return -1;
        ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
        return static_cast<int>(n);
    }

    int write(std::span<const uint8_t> buf) noexcept {
        if (fd_ == INVALID_FD) return -1;
        ssize_t n = ::send(fd_, buf.data(), buf.size(), MSG_NOSIGNAL);
        return static_cast<int>(n);
    }

    void close() noexcept {
        if (fd_ != INVALID_FD) {
            ::close(fd_);
            fd_ = INVALID_FD;
        }
    }

    // Extra helpers ------------------------------------------------------------

    bool is_valid() const noexcept { return fd_ != INVALID_FD; }
    int  fd()       const noexcept { return fd_; }

private:
    int fd_;
};

// ---------------------------------------------------------------------------
// LinuxAcceptor  – listens on a TCP port, produces LinuxSocket instances
// ---------------------------------------------------------------------------

class LinuxAcceptor {
public:
    explicit LinuxAcceptor(uint16_t port, int backlog = 5) noexcept
        : port_(port), backlog_(backlog) {}

    ~LinuxAcceptor() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    // Open the listening socket. Returns true on success.
    bool start() noexcept {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { perror("socket"); return false; }

        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            perror("bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
        }
        if (::listen(listen_fd_, backlog_) < 0) {
            perror("listen"); ::close(listen_fd_); listen_fd_ = -1; return false;
        }
        running_ = true;
        return true;
    }

    void stop() noexcept { running_ = false; }

    // AcceptorConcept interface ------------------------------------------------

    LinuxSocket accept() noexcept {
        sockaddr_in client{};
        socklen_t   len = sizeof(client);
        int cfd = ::accept(listen_fd_,
                           reinterpret_cast<sockaddr*>(&client), &len);
        return LinuxSocket(cfd);
    }

    bool is_running() const noexcept { return running_; }

private:
    uint16_t port_;
    int      backlog_;
    int      listen_fd_ { -1 };
    bool     running_   { false };
};

} // namespace uchttp
