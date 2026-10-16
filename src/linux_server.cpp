// linux_server.cpp – example HTTP server running on localhost
//
// Build (from project root after cmake):
//   cmake -B build && cmake --build build
//   ./build/uchttp_server
//
// Test:
//   curl http://localhost:8080/
//   curl http://localhost:8080/api/hello
//   curl -X POST http://localhost:8080/api/echo -d 'hello world'
//   curl -X PUT  http://localhost:8080/api/item -d '{"id":1}'

#include <cstdio>
#include <csignal>
#include "uchttp.h"
#include "linux_socket.h"

static uchttp::LinuxAcceptor* g_acceptor = nullptr;

static void sigint_handler(int) {
    if (g_acceptor) g_acceptor->stop();
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

static void handle_root(const uchttp::HttpRequest& /*req*/,
                         uchttp::HttpResponse& res) {
    res.set_status(200);
    res.set_content_type("text/html");
    res.set_body("<html><body><h1>uchttp on Linux</h1>"
                 "<p>Microcontroller HTTP server – running on host PC.</p>"
                 "</body></html>");
}

static void handle_hello(const uchttp::HttpRequest& /*req*/,
                          uchttp::HttpResponse& res) {
    res.set_status(200);
    res.set_content_type("application/json");
    res.set_body(R"({"message":"Hello from uchttp!"})");
}

static void handle_echo_post(const uchttp::HttpRequest& req,
                              uchttp::HttpResponse& res) {
    res.set_status(200);
    res.set_content_type("text/plain");
    res.set_body(req.body);  // zero-copy echo
}

static void handle_put_item(const uchttp::HttpRequest& req,
                             uchttp::HttpResponse& res) {
    // In a real MCU app you'd deserialise req.body into a fixed struct.
    (void)req;
    res.set_status(201);
    res.set_content_type("application/json");
    res.set_body(R"({"status":"created"})");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    using Server   = uchttp::HttpServer<uchttp::LinuxSocket>;
    using Acceptor = uchttp::LinuxAcceptor;

    Server   server;
    Acceptor acceptor(8080);

    server.get ("/",          handle_root);
    server.get ("/api/hello", handle_hello);
    server.post("/api/echo",  handle_echo_post);
    server.put ("/api/item",  handle_put_item);

    if (!acceptor.start()) {
        std::fprintf(stderr, "Failed to start acceptor on port 8080\n");
        return 1;
    }

    g_acceptor = &acceptor;
    std::signal(SIGINT,  sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    std::printf("uchttp listening on http://localhost:8080\n");
    std::printf("Press Ctrl+C to stop.\n");

    server.run(acceptor);

    std::printf("\nServer stopped.\n");
    return 0;
}
