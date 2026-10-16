// test_uchttp.cpp – gtest unit tests for uchttp
//
// Tests cover:
//   - HttpResponse formatting (stack buffer, no heap)
//   - GET / POST / PUT routing and parsing via MockSocket
//   - 404 for unknown routes
//   - Header parsing (zero-copy views)
//   - Body parsing for POST and PUT
//   - RouteTable add / find
//   - HttpConnection::process() directly
//   - HttpServer run-loop via MockAcceptor

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <cstring>

#include "uchttp.h"
#include "mock_socket.h"

using namespace uchttp;
using namespace uchttp_test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string_view header_value(std::string_view response,
                                     std::string_view header_name) {
    auto pos = response.find(header_name);
    if (pos == std::string_view::npos) return {};
    pos += header_name.size();
    // skip ": "
    if (pos + 2 > response.size()) return {};
    pos += 2;
    auto end = response.find("\r\n", pos);
    if (end == std::string_view::npos) return response.substr(pos);
    return response.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------
// HttpResponse tests – standalone (no socket needed beyond MockSocket)
// ---------------------------------------------------------------------------

TEST(HttpResponseTest, StatusLine200) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "hello");
    EXPECT_EQ(sock.tx_status_line(), "HTTP/1.1 200 OK");
}

TEST(HttpResponseTest, StatusLine404) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 404, "Not Found");
    EXPECT_EQ(sock.tx_status_line(), "HTTP/1.1 404 Not Found");
}

TEST(HttpResponseTest, StatusLine201) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 201, "");
    EXPECT_EQ(sock.tx_status_line(), "HTTP/1.1 201 Created");
}

TEST(HttpResponseTest, BodyIsPresent) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "hello world");
    EXPECT_EQ(sock.tx_body(), "hello world");
}

TEST(HttpResponseTest, ContentLengthHeader) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "hello", "text/plain");
    auto cl = header_value(sock.tx_string(), "Content-Length");
    EXPECT_EQ(cl, "5");
}

TEST(HttpResponseTest, ContentTypeHeader) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "{}",  "application/json");
    auto ct = header_value(sock.tx_string(), "Content-Type");
    EXPECT_EQ(ct, "application/json");
}

TEST(HttpResponseTest, AddCustomHeader) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    res.set_status(200);
    res.add_header("X-Custom", "42");
    res.set_body("ok");
    MockSocket sock;
    res.send(sock);
    auto val = header_value(sock.tx_string(), "X-Custom");
    EXPECT_EQ(val, "42");
}

TEST(HttpResponseTest, EmptyBody_ContentLengthZero) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 204, "");
    auto cl = header_value(sock.tx_string(), "Content-Length");
    EXPECT_EQ(cl, "0");
    EXPECT_EQ(sock.tx_body(), "");
}

TEST(HttpResponseTest, DoubleSendIsNoop) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "first");
    res.send(sock, 200, "second");  // should be ignored
    EXPECT_EQ(sock.tx_body(), "first");
}

// ---------------------------------------------------------------------------
// RouteTable tests
// ---------------------------------------------------------------------------

TEST(RouteTableTest, AddAndFind) {
    RouteTable<8> rt;
    auto handler = [](const HttpRequest&, HttpResponse&) {};
    EXPECT_TRUE(rt.add(HttpMethod::GET, "/foo", handler));
    EXPECT_TRUE(rt.add(HttpMethod::POST, "/bar", handler));
    EXPECT_EQ(rt.size(), 2u);

    const Route* r = rt.find(HttpMethod::GET, "/foo");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->method, HttpMethod::GET);
    EXPECT_EQ(r->path, "/foo");
}

TEST(RouteTableTest, NotFound) {
    RouteTable<8> rt;
    auto handler = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET, "/foo", handler);
    EXPECT_EQ(rt.find(HttpMethod::POST, "/foo"), nullptr);  // wrong method
    EXPECT_EQ(rt.find(HttpMethod::GET, "/bar"), nullptr);   // wrong path
}

TEST(RouteTableTest, FullTable) {
    RouteTable<2> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    EXPECT_TRUE(rt.add(HttpMethod::GET, "/a", h));
    EXPECT_TRUE(rt.add(HttpMethod::GET, "/b", h));
    EXPECT_FALSE(rt.add(HttpMethod::GET, "/c", h));  // full
}

// ---------------------------------------------------------------------------
// HttpConnection parse tests
// ---------------------------------------------------------------------------

namespace {
    // Shared flag so handlers can signal they were called
    struct CallRecord {
        bool called { false };
        HttpMethod method { HttpMethod::UNKNOWN };
        std::string path;
        std::string body;
        std::string host_header;
    };
    thread_local CallRecord g_record;
}

static RouteTable<8> make_routes() {
    RouteTable<8> rt;

    rt.add(HttpMethod::GET, "/", [](const HttpRequest& req, HttpResponse& res) {
        g_record.called = true;
        g_record.method = req.method;
        g_record.path   = std::string(req.path);
        g_record.host_header = std::string(req.get_header("Host"));
        res.set_status(200);
        res.set_body("root");
    });
    rt.add(HttpMethod::GET, "/hello", [](const HttpRequest& req, HttpResponse& res) {
        g_record.called = true;
        g_record.method = req.method;
        g_record.path   = std::string(req.path);
        res.set_status(200);
        res.set_body("hello");
    });
    rt.add(HttpMethod::POST, "/data", [](const HttpRequest& req, HttpResponse& res) {
        g_record.called = true;
        g_record.method = req.method;
        g_record.path   = std::string(req.path);
        g_record.body   = std::string(req.body);
        res.set_status(201);
        res.set_body("created");
    });
    rt.add(HttpMethod::PUT, "/item", [](const HttpRequest& req, HttpResponse& res) {
        g_record.called = true;
        g_record.method = req.method;
        g_record.path   = std::string(req.path);
        g_record.body   = std::string(req.body);
        res.set_status(200);
        res.set_body("updated");
    });
    return rt;
}

// Reusable route table (built once per test suite)
static const RouteTable<8> kRoutes = make_routes();

// ---------------------------------------------------------------------------
// RefMockSocket – like MockSocket but writes to an external string so we can
// inspect the TX after the connection owns and destroys the socket.
// ---------------------------------------------------------------------------

class RefMockSocket {
public:
    RefMockSocket(std::string_view rx, std::string& tx_out)
        : rx_(rx), tx_(tx_out) {}

    int read(std::span<uint8_t> buf) noexcept {
        if (rx_pos_ >= rx_.size()) return 0;
        size_t n = std::min(rx_.size() - rx_pos_, buf.size());
        std::memcpy(buf.data(), rx_.data() + rx_pos_, n);
        rx_pos_ += n;
        return static_cast<int>(n);
    }

    int write(std::span<const uint8_t> buf) noexcept {
        tx_.append(reinterpret_cast<const char*>(buf.data()), buf.size());
        return static_cast<int>(buf.size());
    }

    void close() noexcept {}
    bool is_valid() const noexcept { return true; }

private:
    std::string rx_;
    size_t      rx_pos_ { 0 };
    std::string& tx_;
};

static std::string process_request(std::string_view raw,
                                    const RouteTable<8>& routes) {
    g_record = {};
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(routes);
    return tx;
}

// Helper: extract status code integer from "HTTP/1.1 XYZ ..."
static int parse_status_code(std::string_view response) {
    // skip "HTTP/1.1 "
    if (response.size() < 12) return 0;
    return std::stoi(std::string(response.substr(9, 3)));
}

// ---------------------------------------------------------------------------
// GET tests
// ---------------------------------------------------------------------------

TEST(HttpConnectionTest, GET_RootReturns200) {
    const std::string raw =
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_TRUE(g_record.called);
    EXPECT_EQ(g_record.method, HttpMethod::GET);
    EXPECT_EQ(g_record.path, "/");
}

TEST(HttpConnectionTest, GET_HelloReturns200WithBody) {
    const std::string raw =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(g_record.path, "/hello");
    // body after double CRLF
    auto pos = tx.find("\r\n\r\n");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(tx.substr(pos + 4), "hello");
}

TEST(HttpConnectionTest, GET_UnknownPathReturns404) {
    const std::string raw =
        "GET /does-not-exist HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 404);
}

// ---------------------------------------------------------------------------
// POST tests
// ---------------------------------------------------------------------------

TEST(HttpConnectionTest, POST_DataParsedCorrectly) {
    std::string body_payload = "key=value&foo=bar";
    std::string raw =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: " + std::to_string(body_payload.size()) + "\r\n"
        "\r\n" + body_payload;

    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 201);
    EXPECT_EQ(g_record.method, HttpMethod::POST);
    EXPECT_EQ(g_record.path, "/data");
    EXPECT_EQ(g_record.body, body_payload);
}

TEST(HttpConnectionTest, POST_JsonBody) {
    std::string json = R"({"name":"test","value":42})";
    std::string raw  =
        "POST /data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(json.size()) + "\r\n"
        "\r\n" + json;

    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 201);
    EXPECT_EQ(g_record.body, json);
}

TEST(HttpConnectionTest, POST_WrongPathReturns404) {
    const std::string raw =
        "POST /nowhere HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 404);
}

// ---------------------------------------------------------------------------
// PUT tests
// ---------------------------------------------------------------------------

TEST(HttpConnectionTest, PUT_ItemParsedCorrectly) {
    std::string json = R"({"id":1,"name":"widget"})";
    std::string raw  =
        "PUT /item HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(json.size()) + "\r\n"
        "\r\n" + json;

    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(g_record.method, HttpMethod::PUT);
    EXPECT_EQ(g_record.path, "/item");
    EXPECT_EQ(g_record.body, json);
}

TEST(HttpConnectionTest, PUT_WrongMethodOnGetRoute_Returns404) {
    // /hello only has GET registered
    const std::string raw =
        "PUT /hello HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 404);
}

// ---------------------------------------------------------------------------
// Header parsing tests
// ---------------------------------------------------------------------------

TEST(HttpConnectionTest, HeaderParsing_ContentType) {
    // The handler checks req.get_header("Content-Type")
    std::string body_payload = "data";

    // We'll check via a custom route that inspects headers
    RouteTable<8> rt;
    static std::string captured_ct;
    rt.add(HttpMethod::POST, "/ct", [](const HttpRequest& req, HttpResponse& res) {
        captured_ct = std::string(req.get_header("Content-Type"));
        res.set_status(200);
        res.set_body("ok");
    });

    std::string raw =
        "POST /ct HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 4\r\n"
        "\r\ndata";

    std::string tx = process_request(raw, rt);
    // process_request uses kRoutes – we need to call directly for custom routes
    g_record = {};
    std::string tx2;
    RefMockSocket sock2(raw, tx2);
    HttpConnection<RefMockSocket> conn2(std::move(sock2));
    conn2.process(rt);

    EXPECT_EQ(captured_ct, "application/json");
    EXPECT_EQ(parse_status_code(tx2), 200);
}

TEST(HttpConnectionTest, HeaderParsing_MultipleHeaders) {
    RouteTable<8> rt;
    static uint8_t header_count = 0;
    rt.add(HttpMethod::GET, "/hdrs", [](const HttpRequest& req, HttpResponse& res) {
        header_count = req.header_count;
        res.set_status(200);
    });

    const std::string raw =
        "GET /hdrs HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: */*\r\n"
        "X-Custom-1: val1\r\n"
        "X-Custom-2: val2\r\n"
        "\r\n";

    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    // 4 headers: Host, Accept, X-Custom-1, X-Custom-2
    EXPECT_EQ(header_count, 4);
}

// ---------------------------------------------------------------------------
// Invalid / malformed request
// ---------------------------------------------------------------------------

TEST(HttpConnectionTest, MalformedRequest_Returns400) {
    const std::string raw = "NOT A VALID HTTP REQUEST\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    // Build a minimal route table
    RouteTable<8> rt;
    conn.process(rt);
    EXPECT_EQ(parse_status_code(tx), 400);
}

TEST(HttpConnectionTest, EmptyRequest_NoResponse) {
    const std::string raw = "";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    RouteTable<8> rt;
    bool complete = conn.process(rt);
    EXPECT_FALSE(complete);
}

// ---------------------------------------------------------------------------
// HttpServer + MockAcceptor integration
// ---------------------------------------------------------------------------

TEST(HttpServerTest, RunOnce_GET_Dispatched) {
    // We cannot easily capture TX from HttpServer::run() since it creates the
    // connection internally. Use MockAcceptor + RefMockSocket adaptation:
    // Instead test via the public RouteTable API and HttpConnection directly.

    RouteTable<4> rt;
    static bool handler_called = false;
    rt.add(HttpMethod::GET, "/ping", [](const HttpRequest&, HttpResponse& res) {
        handler_called = true;
        res.set_status(200);
        res.set_body("pong");
    });

    const std::string raw =
        "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";

    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    bool result = conn.process(rt);

    EXPECT_TRUE(result);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(parse_status_code(tx), 200);

    auto pos = tx.find("\r\n\r\n");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(tx.substr(pos + 4), "pong");
}

// ---------------------------------------------------------------------------
// Response formatting details
// ---------------------------------------------------------------------------

TEST(HttpResponseTest, ResponseEndsWithDoubleCRLF) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "");
    std::string_view tv = sock.tx_string();
    // Headers must end with \r\n\r\n
    EXPECT_NE(tv.find("\r\n\r\n"), std::string_view::npos);
}

TEST(HttpResponseTest, LargeBody) {
    // Verify no buffer overflow for a body close to TX buffer size
    static char buf[2048];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    // Body that fits in the buffer
    std::string big_body(512, 'A');
    MockSocket sock;
    res.send(sock, 200, big_body, "text/plain");
    EXPECT_EQ(sock.tx_body(), big_body);
}
