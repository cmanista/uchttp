#include "linux_socket.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>

namespace uchttp {

// ---------------------------------------------------------------------------
// LinuxSocket
// ---------------------------------------------------------------------------

LinuxSocket::LinuxSocket(int fd) noexcept : fd_(fd) {}

LinuxSocket::LinuxSocket(LinuxSocket&& o) noexcept : fd_(o.fd_) {
    o.fd_ = INVALID_FD;
}

LinuxSocket& LinuxSocket::operator=(LinuxSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_   = o.fd_;
        o.fd_ = INVALID_FD;
    }
    return *this;
}

LinuxSocket::~LinuxSocket() {
    close();
}

int LinuxSocket::read(std::span<uint8_t> buf) noexcept {
    if (fd_ == INVALID_FD) return -1;
    ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
    return static_cast<int>(n);
}

int LinuxSocket::write(std::span<const uint8_t> buf) noexcept {
    if (fd_ == INVALID_FD) return -1;
    ssize_t n = ::send(fd_, buf.data(), buf.size(), MSG_NOSIGNAL);
    return static_cast<int>(n);
}

void LinuxSocket::close() noexcept {
    if (fd_ != INVALID_FD) {
        ::close(fd_);
        fd_ = INVALID_FD;
    }
}

bool LinuxSocket::is_valid() const noexcept { return fd_ != INVALID_FD; }
int  LinuxSocket::fd()       const noexcept { return fd_; }

// ---------------------------------------------------------------------------
// LinuxAcceptor
// ---------------------------------------------------------------------------

LinuxAcceptor::LinuxAcceptor(uint16_t port, int backlog) noexcept
    : port_(port), backlog_(backlog) {}

LinuxAcceptor::~LinuxAcceptor() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool LinuxAcceptor::start() noexcept {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("socket"); return false; }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    if (::listen(listen_fd_, backlog_) < 0) {
        perror("listen"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }

    running_ = true;
    return true;
}

void LinuxAcceptor::stop() noexcept { running_ = false; }

LinuxSocket LinuxAcceptor::accept() noexcept {
    sockaddr_in client{};
    socklen_t   len = sizeof(client);
    int cfd = ::accept(listen_fd_,
                       reinterpret_cast<sockaddr*>(&client), &len);
    return LinuxSocket(cfd);
}

bool LinuxAcceptor::is_running() const noexcept { return running_; }

} // namespace uchttp
