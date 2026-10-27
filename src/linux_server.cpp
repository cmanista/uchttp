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
//   curl http://localhost:8080/api/sensors/42           # path parameter
//   curl 'http://localhost:8080/api/search?q=temp&limit=5'  # query string
//   curl -X DELETE http://localhost:8080/api/sensors/42  # DELETE method
//   curl -X OPTIONS http://localhost:8080/api/data -i    # CORS preflight

#include <cstdio>
#include <csignal>
#include "uchttp.h"
#include "linux_socket.h"

static uchttp::LinuxAcceptor* g_acceptor = nullptr;

static void sigint_handler(int) {
    if (g_acceptor) g_acceptor->stop();
}

// ---------------------------------------------------------------------------
// Middleware (Chain of Responsibility) – requirement #2
// ---------------------------------------------------------------------------

// Logging middleware: records the method/path, runs the rest of the chain,
// then logs the resulting status code. Code after chain.next() runs on the
// way back out, so the response status is already set.
static void mw_logger(const uchttp::HttpRequest& req,
                      uchttp::HttpResponse& res,
                      uchttp::MiddlewareChain& chain) {
    const char* method = uchttp::method_string(req.method);
    chain.next(req, res);   // run inner middleware + handler
    std::printf("[HTTP] %s %.*s\n",
                method,
                static_cast<int>(req.path.size()), req.path.data());
    std::fflush(stdout);
}

// Security-headers middleware: injects headers into every response after the
// handler has produced its body (requirement #4/#5, applied globally here).
static void mw_security_headers(const uchttp::HttpRequest& req,
                                uchttp::HttpResponse& res,
                                uchttp::MiddlewareChain& chain) {
    chain.next(req, res);
    res.add_header("X-Content-Type-Options", "nosniff");
    res.add_header("X-Frame-Options", "DENY");
    res.add_header("Access-Control-Allow-Origin", "*");
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

// Path parameter demo: GET /api/sensors/{id}
// The dynamic {id} segment is captured zero-copy from the RX buffer.
static void handle_get_sensor(const uchttp::HttpRequest& req,
                              uchttp::HttpResponse& res) {
    std::string_view id = req.path_param("id");
    // A real handler would look up the sensor by id; here we just echo it back.
    static char buf[128];
    int n = std::snprintf(buf, sizeof(buf),
                          R"({"sensor":"%.*s","value":23.5})",
                          static_cast<int>(id.size()), id.data());
    res.set_status(200);
    res.set_content_type("application/json");
    res.set_body(std::string_view(buf, static_cast<size_t>(n)));
}

// DELETE /api/sensors/{id}
static void handle_delete_sensor(const uchttp::HttpRequest& /*req*/,
                                 uchttp::HttpResponse& res) {
    res.set_status(204);  // No Content
}

// Query string demo: GET /api/search?q=...&limit=...
static void handle_search(const uchttp::HttpRequest& req,
                          uchttp::HttpResponse& res) {
    std::string_view q     = req.query_param("q");
    std::string_view limit = req.query_param("limit");
    static char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
                          R"({"query":"%.*s","limit":"%.*s"})",
                          static_cast<int>(q.size()),     q.data(),
                          static_cast<int>(limit.size()), limit.data());
    res.set_status(200);
    res.set_content_type("application/json");
    res.set_body(std::string_view(buf, static_cast<size_t>(n)));
}

// OPTIONS /api/data – CORS preflight (requirement #1: OPTIONS for CORS).
static void handle_options_data(const uchttp::HttpRequest& /*req*/,
                                uchttp::HttpResponse& res) {
    res.set_status(204);
    res.add_header("Allow", "GET, POST, OPTIONS");
    res.add_header("Access-Control-Allow-Origin", "*");
    res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    using Server   = uchttp::HttpServer<uchttp::LinuxSocket>;
    using Acceptor = uchttp::LinuxAcceptor;

    Server   server;
    Acceptor acceptor(8080);

    // Global middleware – run in registration order around every handler.
    server.use(mw_logger);
    server.use(mw_security_headers);

    server.get ("/",          handle_root);
    server.get ("/api/hello", handle_hello);
    server.post("/api/echo",  handle_echo_post);
    server.put ("/api/item",  handle_put_item);

    // Advanced routing (requirement #1):
    server.get    ("/api/sensors/{id}", handle_get_sensor);     // path parameter
    server.del    ("/api/sensors/{id}", handle_delete_sensor);  // DELETE method
    server.get    ("/api/search",       handle_search);         // query string
    server.options("/api/data",         handle_options_data);   // OPTIONS/CORS

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
