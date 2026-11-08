// linux_https_server.cpp — example HTTPS server (OpenSSL backend, requirement #4)
//
// Build (OpenSSL backend is built automatically when OpenSSL is found):
//   cmake -B build && cmake --build build
//
// Generate a self-signed certificate for local testing:
//   openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key
//       -out server.crt -days 1 -subj '/CN=localhost'
//
//   ./build/uchttp_https_server server.crt server.key
//   curl -k https://localhost:8443/api/hello

#include <cstdio>
#include <cstdlib>
#include <csignal>

#include "uchttp.h"
#include "uchttp_security.h"
#include "linux_socket.h"
#include "uchttp_tls.h"
#include "openssl_tls.h"

static uchttp::LinuxAcceptor* g_acceptor = nullptr;

static void sigint_handler(int) {
    if (g_acceptor) g_acceptor->stop();
}

static void handle_hello(const uchttp::HttpRequest&, uchttp::HttpResponse& res) {
    res.set_status(200);
    res.set_content_type("application/json");
    res.set_body(R"({"message":"Hello over TLS!"})");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <cert.pem> <key.pem> [port]\n", argv[0]);
        return 2;
    }
    const char*    cert = argv[1];
    const char*    key  = argv[2];
    const uint16_t port = (argc > 3)
        ? static_cast<uint16_t>(std::atoi(argv[3])) : 8443;

    // 1. Crypto context: load the server certificate + private key.
    uchttp::OpenSslContext ctx;
    if (!ctx.is_valid() || !ctx.load_cert_and_key(cert, key)) {
        std::fprintf(stderr, "Failed to load cert/key\n");
        return 1;
    }

    // 2. Raw TCP acceptor + TLS wrapper that upgrades every connection.
    uchttp::LinuxAcceptor raw(port);
    if (!raw.start()) {
        std::fprintf(stderr, "Failed to listen on %u\n", port);
        return 1;
    }
    g_acceptor = &raw;
    std::signal(SIGINT,  sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    uchttp::TlsAcceptor<uchttp::OpenSslContext, uchttp::LinuxAcceptor>
        tls(ctx, raw);

    // 3. The HTTP engine runs unchanged over the TLS socket type.
    using TlsSock = uchttp::TlsSocket<uchttp::OpenSslSession>;
    uchttp::HttpServer<TlsSock> server;
    server.use(uchttp::security::security_headers);
    server.get("/api/hello", handle_hello);

    std::printf("uchttp HTTPS listening on https://localhost:%u\n", port);
    server.run(tls);

    std::printf("\nServer stopped.\n");
    return 0;
}
