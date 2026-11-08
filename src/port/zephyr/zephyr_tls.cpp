#include "zephyr_tls.h"

// Native Zephyr socket + TLS APIs (zsock_* / net_* names) mirror the plaintext
// port in zephyr_socket.cpp, keeping <zephyr/posix/...> out of the build so it
// compiles identically on native_sim and real targets.
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <cstring>

#ifndef UCHTTP_IDLE_TIMEOUT_MS
#  define UCHTTP_IDLE_TIMEOUT_MS 5000
#endif

namespace uchttp {

// ---------------------------------------------------------------------------
// ZephyrTlsCredentials
// ---------------------------------------------------------------------------

bool ZephyrTlsCredentials::add_server_cert(int tag,
					   const void *cert, size_t cert_len,
					   const void *key,  size_t key_len) noexcept
{
	if (tls_credential_add(static_cast<sec_tag_t>(tag),
			       TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
			       cert, cert_len) < 0) {
		return false;
	}
	return tls_credential_add(static_cast<sec_tag_t>(tag),
				  TLS_CREDENTIAL_PRIVATE_KEY,
				  key, key_len) == 0;
}

bool ZephyrTlsCredentials::add_ca_cert(int tag,
				       const void *ca, size_t ca_len) noexcept
{
	return tls_credential_add(static_cast<sec_tag_t>(tag),
				  TLS_CREDENTIAL_CA_CERTIFICATE,
				  ca, ca_len) == 0;
}

bool ZephyrTlsCredentials::add_psk(int tag,
				   const void *psk, size_t psk_len,
				   const void *identity,
				   size_t identity_len) noexcept
{
	if (tls_credential_add(static_cast<sec_tag_t>(tag),
			       TLS_CREDENTIAL_PSK,
			       psk, psk_len) < 0) {
		return false;
	}
	return tls_credential_add(static_cast<sec_tag_t>(tag),
				  TLS_CREDENTIAL_PSK_ID,
				  identity, identity_len) == 0;
}

// ---------------------------------------------------------------------------
// ZephyrTlsAcceptor
// ---------------------------------------------------------------------------

ZephyrTlsAcceptor::ZephyrTlsAcceptor(uint16_t port, int sec_tag,
				     int backlog) noexcept
	: port_(port), sec_tag_(sec_tag), backlog_(backlog) {}

ZephyrTlsAcceptor::~ZephyrTlsAcceptor()
{
	if (listen_fd_ >= 0) zsock_close(listen_fd_);
}

bool ZephyrTlsAcceptor::start() noexcept
{
	// A TLS listening socket: the networking stack performs the handshake
	// and (de)encryption transparently inside accept()/recv()/send().
	listen_fd_ = zsock_socket(NET_AF_INET, NET_SOCK_STREAM,
				  NET_IPPROTO_TLS_1_2);
	if (listen_fd_ < 0) return false;

	// Bind the credential set (certificate + key, or PSK) to this socket.
	const sec_tag_t sec_tag_list[] = { static_cast<sec_tag_t>(sec_tag_) };
	if (zsock_setsockopt(listen_fd_, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST,
			     sec_tag_list, sizeof(sec_tag_list)) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}

	// Server-side peer (client-certificate) verification. Default is NONE:
	// standard one-way HTTPS where only the server is authenticated.
	int verify = peer_verify_;
	if (zsock_setsockopt(listen_fd_, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY,
			     &verify, sizeof(verify)) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}

	struct net_sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family      = NET_AF_INET;
	addr.sin_addr.s_addr = net_htonl(NET_INADDR_ANY);
	addr.sin_port        = net_htons(port_);

	if (zsock_bind(listen_fd_,
		       reinterpret_cast<struct net_sockaddr *>(&addr),
		       sizeof(addr)) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}
	if (zsock_listen(listen_fd_, backlog_) < 0) {
		zsock_close(listen_fd_); listen_fd_ = -1; return false;
	}

	running_ = true;
	return true;
}

void ZephyrTlsAcceptor::stop() noexcept
{
	running_ = false;
	if (listen_fd_ >= 0) {
		zsock_shutdown(listen_fd_, ZSOCK_SHUT_RDWR);
	}
}

ZephyrSocket ZephyrTlsAcceptor::accept() noexcept
{
	if (!running_) return ZephyrSocket(ZephyrSocket::INVALID_FD);

	struct net_sockaddr_in client;
	net_socklen_t          len = sizeof(client);
	int cfd = zsock_accept(listen_fd_,
			       reinterpret_cast<struct net_sockaddr *>(&client),
			       &len);
	// The accepted fd is a TLS socket: recv()/send() transparently decrypt/
	// encrypt, so a plain ZephyrSocket drives the engine unchanged.
	ZephyrSocket sock(cfd);
	if (sock.is_valid() && UCHTTP_IDLE_TIMEOUT_MS > 0) {
		sock.set_read_timeout(UCHTTP_IDLE_TIMEOUT_MS);
	}
	return sock;
}

bool ZephyrTlsAcceptor::is_running() const noexcept { return running_; }

} // namespace uchttp
