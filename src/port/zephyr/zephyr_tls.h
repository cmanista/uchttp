// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

// zephyr_tls.h — HTTPS support for the Zephyr port (Stage 2).
//
// Unlike the host OpenSSL/wolfSSL backends (which wrap an accepted fd with an
// explicit SSL object and satisfy uchttp::TlsSessionConcept), Zephyr performs
// TLS transparently *at the socket layer*: a socket created with the
// IPPROTO_TLS_1_2 protocol encrypts/decrypts inside recv()/send(). The
// credentials and TLS options are configured once on the listening socket and
// inherited by every accepted socket.
//
// Because of that, the plaintext ZephyrSocket works unchanged over TLS — its
// read()/write() already call zsock_recv()/zsock_send(). All that differs is
// how the *listening* socket is created, so Stage 2 lives in the acceptor:
//
//   uchttp::ZephyrTlsCredentials::add_server_cert(
//       UCHTTP_TLS_SERVER_CERT_TAG, cert, cert_len, key, key_len);
//
//   uchttp::ZephyrTlsAcceptor acceptor(8443);  // sec_tag defaults to the above
//   uchttp::HttpServer<uchttp::ZephyrSocket> server;
//   server.run(acceptor);                      // same engine, now over TLS
//
// The crypto itself is provided by Zephyr's native TLS stack (Mbed TLS / PSA
// Crypto), selected via Kconfig in the application's prj.conf. uchttp adds no
// dynamic allocation; credentials are registered from caller-owned storage.

#include <cstddef>
#include <cstdint>

#include "zephyr_socket.h"

// Default secure tag used by the sample / convenience ctor. A sec_tag is just
// an integer handle that links a listening socket to a set of registered
// credentials (certificate + key, or PSK).
#ifndef UCHTTP_TLS_SERVER_CERT_TAG
#  define UCHTTP_TLS_SERVER_CERT_TAG 1
#endif

#ifndef UCHTTP_TLS_PSK_TAG
#  define UCHTTP_TLS_PSK_TAG 2
#endif

namespace uchttp {

// ---------------------------------------------------------------------------
// ZephyrTlsCredentials — thin wrapper over Zephyr's tls_credential_add(). The
// credential blobs are *not* copied by uchttp; Zephyr's credential store keeps
// a pointer to the caller-provided buffers, so they must stay alive for the
// lifetime of any socket that references the tag (typically `static const`).
// ---------------------------------------------------------------------------

class ZephyrTlsCredentials {
public:
	// Register a server certificate chain (PEM or DER) and its private key
	// under `tag`. Returns true on success.
	static bool add_server_cert(int tag,
				    const void *cert, size_t cert_len,
				    const void *key,  size_t key_len) noexcept;

	// Register a CA certificate under `tag` (only needed when the server
	// also verifies client certificates — mutual TLS).
	static bool add_ca_cert(int tag,
				const void *ca, size_t ca_len) noexcept;

	// Register a pre-shared key and its identity under `tag`.
	static bool add_psk(int tag,
			    const void *psk, size_t psk_len,
			    const void *identity, size_t identity_len) noexcept;
};

// ---------------------------------------------------------------------------
// ZephyrTlsAcceptor — like ZephyrAcceptor, but the listening socket is created
// with IPPROTO_TLS_1_2 and bound to a credential sec_tag. Accepted connections
// are ordinary ZephyrSocket instances (TLS is transparent at the socket API).
// ---------------------------------------------------------------------------

class ZephyrTlsAcceptor {
public:
	// `sec_tag` selects the credentials registered via ZephyrTlsCredentials.
	// Pass TLS_PEER_VERIFY_* in `peer_verify` (default: no client-cert check,
	// i.e. server-only auth, which is the common HTTPS case).
	explicit ZephyrTlsAcceptor(uint16_t port,
				   int sec_tag = UCHTTP_TLS_SERVER_CERT_TAG,
				   int backlog = 4) noexcept;
	~ZephyrTlsAcceptor();

	// Require/optional client certificate verification (mutual TLS). Must be
	// called before start(). Values mirror ZSOCK_TLS_PEER_VERIFY_*.
	void set_peer_verify(int mode) noexcept { peer_verify_ = mode; }

	bool start() noexcept;
	void stop()  noexcept;

	// AcceptorConcept interface
	ZephyrSocket accept()     noexcept;
	bool         is_running() const noexcept;

private:
	uint16_t port_;
	int      sec_tag_;
	int      backlog_;
	int      peer_verify_ { 0 };   // ZSOCK_TLS_PEER_VERIFY_NONE
	int      listen_fd_   { -1 };
	bool     running_     { false };
};

} // namespace uchttp
