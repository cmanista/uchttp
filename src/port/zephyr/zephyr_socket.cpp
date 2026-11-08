#include "zephyr_socket.h"

// Use Zephyr's native socket API (zsock_* / net_*) rather than the POSIX
// aliases. This keeps the port free of <zephyr/posix/...> headers, which on
// native_sim clash with the host libc (pthread types), and works identically
// on real targets. CONFIG_POSIX_API is therefore not required by the port.
#include <zephyr/net/socket.h>

#include <cstring>
#include <utility>

#ifndef UCHTTP_IDLE_TIMEOUT_MS
#  define UCHTTP_IDLE_TIMEOUT_MS 5000
#endif

namespace uchttp {

// ---------------------------------------------------------------------------
// ZephyrSocket
// ---------------------------------------------------------------------------

ZephyrSocket::ZephyrSocket(int fd) noexcept : fd_(fd) {}

ZephyrSocket::ZephyrSocket(ZephyrSocket&& o) noexcept : fd_(o.fd_) {
	o.fd_ = INVALID_FD;
}

ZephyrSocket& ZephyrSocket::operator=(ZephyrSocket&& o) noexcept {
	if (this != &o) {
		close();
		fd_   = o.fd_;
		o.fd_ = INVALID_FD;
	}
	return *this;
}

ZephyrSocket::~ZephyrSocket() {
	close();
}

int ZephyrSocket::read(std::span<uint8_t> buf) noexcept {
	if (fd_ == INVALID_FD) return -1;
	return static_cast<int>(zsock_recv(fd_, buf.data(), buf.size(), 0));
}

int ZephyrSocket::write(std::span<const uint8_t> buf) noexcept {
	if (fd_ == INVALID_FD) return -1;
	// send() may return short counts; loop until the whole span is out.
	size_t sent = 0;
	while (sent < buf.size()) {
		ssize_t n = zsock_send(fd_, buf.data() + sent, buf.size() - sent, 0);
		if (n <= 0) return n == 0 ? static_cast<int>(sent) : -1;
		sent += static_cast<size_t>(n);
	}
	return static_cast<int>(sent);
}

void ZephyrSocket::close() noexcept {
	if (fd_ != INVALID_FD) {
		zsock_close(fd_);
		fd_ = INVALID_FD;
	}
}

bool ZephyrSocket::set_read_timeout(unsigned ms) noexcept {
	if (fd_ == INVALID_FD) return false;
	struct zsock_timeval tv;
	tv.tv_sec  = static_cast<long>(ms / 1000);
	tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
	return zsock_setsockopt(fd_, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO,
				&tv, sizeof(tv)) == 0;
}

bool ZephyrSocket::is_valid() const noexcept { return fd_ != INVALID_FD; }
int  ZephyrSocket::fd()       const noexcept { return fd_; }

// ---------------------------------------------------------------------------
// ZephyrAcceptor
// ---------------------------------------------------------------------------

ZephyrAcceptor::ZephyrAcceptor(uint16_t port, int backlog) noexcept
	: port_(port), backlog_(backlog) {}

ZephyrAcceptor::~ZephyrAcceptor() {
	if (listen_fd_ >= 0) zsock_close(listen_fd_);
}

bool ZephyrAcceptor::start() noexcept {
	listen_fd_ = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (listen_fd_ < 0) return false;

	struct net_sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family             = NET_AF_INET;
	addr.sin_addr.s_addr        = net_htonl(NET_INADDR_ANY);
	addr.sin_port               = net_htons(port_);

	if (zsock_bind(listen_fd_,
		       reinterpret_cast<struct net_sockaddr*>(&addr),
		       sizeof(addr)) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}
	if (zsock_listen(listen_fd_, backlog_) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}

	running_ = true;
	return true;
}

void ZephyrAcceptor::stop() noexcept {
	running_ = false;
	// Wake up a blocked accept() so run() can exit cleanly (graceful shutdown,
	// requirement #6). The fd is closed in the destructor.
	if (listen_fd_ >= 0) {
		zsock_shutdown(listen_fd_, ZSOCK_SHUT_RDWR);
	}
}

ZephyrSocket ZephyrAcceptor::accept() noexcept {
	if (!running_) return ZephyrSocket(ZephyrSocket::INVALID_FD);

	struct net_sockaddr_in client;
	net_socklen_t          len = sizeof(client);
	int cfd = zsock_accept(listen_fd_,
			       reinterpret_cast<struct net_sockaddr*>(&client), &len);
	ZephyrSocket sock(cfd);
	if (sock.is_valid() && UCHTTP_IDLE_TIMEOUT_MS > 0) {
		sock.set_read_timeout(UCHTTP_IDLE_TIMEOUT_MS);
	}
	return sock;
}

bool ZephyrAcceptor::is_running() const noexcept { return running_; }

} // namespace uchttp
