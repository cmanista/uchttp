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
#include "uchttp_security.h"
#include "uchttp_app_middleware.h"
#include "uchttp_tls.h"
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
    RefMockSocket() = default;
    RefMockSocket(std::string_view rx, std::string& tx_out)
        : rx_(rx), tx_(&tx_out) {}

    int read(std::span<uint8_t> buf) noexcept {
        if (rx_pos_ >= rx_.size()) return 0;
        size_t n = std::min(rx_.size() - rx_pos_, buf.size());
        std::memcpy(buf.data(), rx_.data() + rx_pos_, n);
        rx_pos_ += n;
        return static_cast<int>(n);
    }

    int write(std::span<const uint8_t> buf) noexcept {
        if (tx_) tx_->append(reinterpret_cast<const char*>(buf.data()),
                             buf.size());
        return static_cast<int>(buf.size());
    }

    void close() noexcept { valid_ = false; }
    bool is_valid() const noexcept { return valid_; }

private:
    std::string  rx_;
    size_t       rx_pos_ { 0 };
    std::string* tx_ { nullptr };
    bool         valid_ { true };
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

TEST(HttpConnectionTest, PUT_WrongMethodOnGetRoute_Returns405) {
    // /hello only has GET registered -> path exists, method not allowed.
    const std::string raw =
        "PUT /hello HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\n\r\n";
    std::string tx = process_request(raw, kRoutes);
    EXPECT_EQ(parse_status_code(tx), 405);
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

// ===========================================================================
// QueryString – standalone unit tests (no socket needed)
// ===========================================================================

TEST(QueryStringTest, EmptyIsEmpty) {
    QueryString qs;
    EXPECT_TRUE(qs.empty());
    EXPECT_EQ(qs.begin(), qs.end());
    EXPECT_EQ(qs.get("x"), "");
    EXPECT_FALSE(qs.has("x"));
}

TEST(QueryStringTest, SinglePair) {
    QueryString qs("filter=active");
    EXPECT_FALSE(qs.empty());
    EXPECT_EQ(qs.get("filter"), "active");
    EXPECT_TRUE(qs.has("filter"));
    EXPECT_FALSE(qs.has("missing"));
}

TEST(QueryStringTest, MultiplePairs) {
    QueryString qs("filter=active&limit=10&sort=desc");
    EXPECT_EQ(qs.get("filter"), "active");
    EXPECT_EQ(qs.get("limit"),  "10");
    EXPECT_EQ(qs.get("sort"),   "desc");
    EXPECT_EQ(qs.get("absent"), "");
}

TEST(QueryStringTest, KeyWithoutValue) {
    QueryString qs("flag&limit=5");
    EXPECT_TRUE(qs.has("flag"));
    EXPECT_EQ(qs.get("flag"), "");
    EXPECT_EQ(qs.get("limit"), "5");
}

TEST(QueryStringTest, EmptyValue) {
    QueryString qs("key=");
    EXPECT_TRUE(qs.has("key"));
    EXPECT_EQ(qs.get("key"), "");
}

TEST(QueryStringTest, IterationOrder) {
    QueryString qs("a=1&b=2&c=3");
    std::string collected;
    for (auto p : qs) {
        collected += std::string(p.key);
        collected += "=";
        collected += std::string(p.value);
        collected += ";";
    }
    EXPECT_EQ(collected, "a=1;b=2;c=3;");
}

TEST(QueryStringTest, FirstMatchWins) {
    QueryString qs("x=1&x=2");
    EXPECT_EQ(qs.get("x"), "1");
}

// ===========================================================================
// Query string parsing through the full request pipeline
// ===========================================================================

TEST(HttpConnectionTest, URL_PathStrippedOfQuery) {
    RouteTable<8> rt;
    static std::string captured_path;
    static std::string captured_query;
    rt.add(HttpMethod::GET, "/search", [](const HttpRequest& req, HttpResponse& res) {
        captured_path  = std::string(req.path);
        captured_query = std::string(req.raw_query);
        res.set_status(200);
    });

    const std::string raw =
        "GET /search?q=temp&limit=5 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(captured_path,  "/search");
    EXPECT_EQ(captured_query, "q=temp&limit=5");
}

TEST(HttpConnectionTest, QueryParamAccessor) {
    RouteTable<8> rt;
    static std::string q_val;
    static std::string limit_val;
    rt.add(HttpMethod::GET, "/search", [](const HttpRequest& req, HttpResponse& res) {
        q_val     = std::string(req.query_param("q"));
        limit_val = std::string(req.query_param("limit"));
        res.set_status(200);
    });

    const std::string raw =
        "GET /search?q=temperature&limit=10 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(q_val, "temperature");
    EXPECT_EQ(limit_val, "10");
}

TEST(HttpConnectionTest, NoQueryString_RawQueryEmpty) {
    RouteTable<8> rt;
    static bool query_empty = false;
    static std::string captured_path;
    rt.add(HttpMethod::GET, "/plain", [](const HttpRequest& req, HttpResponse& res) {
        query_empty   = req.query().empty();
        captured_path = std::string(req.path);
        res.set_status(200);
    });

    const std::string raw =
        "GET /plain HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_TRUE(query_empty);
    EXPECT_EQ(captured_path, "/plain");
}

// ===========================================================================
// Path parameters
// ===========================================================================

TEST(RouteTableTest, PatternMatchExtractsParam) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET, "/api/sensors/{id}", h);

    HttpRequest req;
    req.method = HttpMethod::GET;
    const Route* r = rt.find(HttpMethod::GET, "/api/sensors/42", req);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(req.param_count, 1);
    EXPECT_EQ(req.path_param("id"), "42");
}

TEST(RouteTableTest, MultiplePathParams) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET, "/api/{group}/items/{item}", h);

    HttpRequest req;
    const Route* r = rt.find(HttpMethod::GET, "/api/sensors/items/temp", req);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(req.param_count, 2);
    EXPECT_EQ(req.path_param("group"), "sensors");
    EXPECT_EQ(req.path_param("item"),  "temp");
}

TEST(RouteTableTest, ExactMatchPreferredOverPattern) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET, "/api/sensors/{id}", h);
    rt.add(HttpMethod::GET, "/api/sensors/all",  h);

    HttpRequest req;
    const Route* r = rt.find(HttpMethod::GET, "/api/sensors/all", req);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->path, "/api/sensors/all");
    EXPECT_EQ(req.param_count, 0);  // literal match, no capture
}

TEST(RouteTableTest, PatternLengthMismatchNoMatch) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET, "/api/sensors/{id}", h);

    HttpRequest req;
    // too many segments
    EXPECT_EQ(rt.find(HttpMethod::GET, "/api/sensors/42/extra", req), nullptr);
    // too few segments
    EXPECT_EQ(rt.find(HttpMethod::GET, "/api/sensors", req), nullptr);
}

TEST(RouteTableTest, PathExists) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    rt.add(HttpMethod::GET,  "/api/sensors/{id}", h);
    rt.add(HttpMethod::POST, "/api/config",       h);

    EXPECT_TRUE(rt.path_exists("/api/sensors/7"));
    EXPECT_TRUE(rt.path_exists("/api/config"));
    EXPECT_FALSE(rt.path_exists("/nope"));
}

TEST(HttpConnectionTest, PathParam_FullPipeline) {
    RouteTable<8> rt;
    static std::string captured_id;
    rt.add(HttpMethod::GET, "/api/sensors/{id}", [](const HttpRequest& req, HttpResponse& res) {
        captured_id = std::string(req.path_param("id"));
        res.set_status(200);
        res.set_body("sensor");
    });

    const std::string raw =
        "GET /api/sensors/temp42 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(captured_id, "temp42");
}

TEST(HttpConnectionTest, PathParam_WithQueryString) {
    RouteTable<8> rt;
    static std::string captured_id;
    static std::string captured_unit;
    rt.add(HttpMethod::GET, "/api/sensors/{id}", [](const HttpRequest& req, HttpResponse& res) {
        captured_id   = std::string(req.path_param("id"));
        captured_unit = std::string(req.query_param("unit"));
        res.set_status(200);
    });

    const std::string raw =
        "GET /api/sensors/5?unit=celsius HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(captured_id,   "5");
    EXPECT_EQ(captured_unit, "celsius");
}

// ===========================================================================
// HTTP methods: DELETE, PATCH, HEAD, OPTIONS
// ===========================================================================

TEST(HttpMethodTest, StringRoundTrip) {
    EXPECT_STREQ(method_string(HttpMethod::DELETE),  "DELETE");
    EXPECT_STREQ(method_string(HttpMethod::PATCH),   "PATCH");
    EXPECT_STREQ(method_string(HttpMethod::HEAD),    "HEAD");
    EXPECT_STREQ(method_string(HttpMethod::OPTIONS), "OPTIONS");
}

TEST(HttpConnectionTest, DELETE_Dispatched) {
    RouteTable<8> rt;
    static HttpMethod seen = HttpMethod::UNKNOWN;
    static std::string seen_id;
    rt.add(HttpMethod::DELETE, "/api/items/{id}", [](const HttpRequest& req, HttpResponse& res) {
        seen    = req.method;
        seen_id = std::string(req.path_param("id"));
        res.set_status(204);
    });

    const std::string raw =
        "DELETE /api/items/99 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 204);
    EXPECT_EQ(seen, HttpMethod::DELETE);
    EXPECT_EQ(seen_id, "99");
}

TEST(HttpConnectionTest, OPTIONS_Dispatched) {
    RouteTable<8> rt;
    static bool called = false;
    rt.add(HttpMethod::OPTIONS, "/api/data", [](const HttpRequest& req, HttpResponse& res) {
        called = (req.method == HttpMethod::OPTIONS);
        res.set_status(204);
        res.add_header("Allow", "GET, POST, OPTIONS");
        res.add_header("Access-Control-Allow-Origin", "*");
    });

    const std::string raw =
        "OPTIONS /api/data HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 204);
    EXPECT_TRUE(called);
    EXPECT_EQ(header_value(tx, "Access-Control-Allow-Origin"), "*");
}

TEST(HttpConnectionTest, PATCH_Dispatched) {
    RouteTable<8> rt;
    static std::string body;
    rt.add(HttpMethod::PATCH, "/api/config", [](const HttpRequest& req, HttpResponse& res) {
        body = std::string(req.body);
        res.set_status(200);
    });

    std::string payload = R"({"level":3})";
    std::string raw =
        "PATCH /api/config HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: " + std::to_string(payload.size()) + "\r\n"
        "\r\n" + payload;
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(body, payload);
}

TEST(HttpConnectionTest, HEAD_Dispatched) {
    RouteTable<8> rt;
    static HttpMethod seen = HttpMethod::UNKNOWN;
    rt.add(HttpMethod::HEAD, "/api/status", [](const HttpRequest& req, HttpResponse& res) {
        seen = req.method;
        res.set_status(200);
    });

    const std::string raw =
        "HEAD /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(seen, HttpMethod::HEAD);
}

TEST(HttpConnectionTest, WrongMethod_Returns405) {
    RouteTable<8> rt;
    auto h = [](const HttpRequest&, HttpResponse& res) { res.set_status(200); };
    rt.add(HttpMethod::GET, "/api/only-get", h);

    const std::string raw =
        "DELETE /api/only-get HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 405);
}

// ===========================================================================
// HttpServer method-helper registration
// ===========================================================================

TEST(HttpServerTest, MethodHelpersRegisterRoutes) {
    HttpServer<MockSocket> server;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    EXPECT_TRUE(server.get    ("/a", h));
    EXPECT_TRUE(server.post   ("/b", h));
    EXPECT_TRUE(server.put    ("/c", h));
    EXPECT_TRUE(server.del    ("/d", h));
    EXPECT_TRUE(server.patch  ("/e", h));
    EXPECT_TRUE(server.head   ("/f", h));
    EXPECT_TRUE(server.options("/g", h));

    const auto& rt = server.routes();
    EXPECT_EQ(rt.size(), 7u);
    HttpRequest req;
    EXPECT_NE(rt.find(HttpMethod::DELETE,  "/d", req), nullptr);
    EXPECT_NE(rt.find(HttpMethod::OPTIONS, "/g", req), nullptr);
}

// ===========================================================================
// Middleware (Chain of Responsibility)
// ===========================================================================

namespace {
    // Shared trace so middleware/handlers can record execution order.
    std::string g_trace;

    void mw_append(const char* tag) { g_trace += tag; }
}

TEST(MiddlewareTest, SingleMiddlewareCallsNextThenHandler) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/mw", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");
        res.set_status(200);
        res.set_body("ok");
    });

    MiddlewareFn mws[] = {
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            mw_append("A>");
            chain.next(req, res);
            mw_append("<A");
        },
    };

    const std::string raw = "GET /mw HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(g_trace, "A>H<A");  // pre, handler, post (on the way out)
}

TEST(MiddlewareTest, MultipleMiddlewareExecuteInOrder) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/mw", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");
        res.set_status(200);
    });

    MiddlewareFn mws[] = {
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            mw_append("1>"); chain.next(req, res); mw_append("<1");
        },
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            mw_append("2>"); chain.next(req, res); mw_append("<2");
        },
    };

    const std::string raw = "GET /mw HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 2);

    // onion model: 1 wraps 2 wraps handler
    EXPECT_EQ(g_trace, "1>2>H<2<1");
}

TEST(MiddlewareTest, ShortCircuitSkipsHandler) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/secret", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");  // must NOT run
        res.set_status(200);
    });

    // Auth-like middleware that rejects and does NOT call next().
    MiddlewareFn mws[] = {
        [](const HttpRequest&, HttpResponse& res, MiddlewareChain&) {
            mw_append("DENY");
            res.set_status(401);
            res.set_body("unauthorized");
        },
    };

    const std::string raw = "GET /secret HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 401);
    EXPECT_EQ(g_trace, "DENY");          // handler never ran
}

TEST(MiddlewareTest, SecondMiddlewareCanShortCircuit) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/mw", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");
        res.set_status(200);
    });

    MiddlewareFn mws[] = {
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            mw_append("1>"); chain.next(req, res); mw_append("<1");
        },
        [](const HttpRequest&, HttpResponse& res, MiddlewareChain&) {
            mw_append("STOP");
            res.set_status(403);
        },
    };

    const std::string raw = "GET /mw HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 2);

    EXPECT_EQ(parse_status_code(tx), 403);
    EXPECT_EQ(g_trace, "1>STOP<1");  // mw1 pre/post run, handler skipped
}

TEST(MiddlewareTest, MiddlewareInjectsResponseHeader) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/data", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
        res.set_body("{}");
    });

    // Security-header middleware: inject headers after the handler runs.
    MiddlewareFn mws[] = {
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            chain.next(req, res);
            res.add_header("X-Content-Type-Options", "nosniff");
            res.add_header("Access-Control-Allow-Origin", "*");
        },
    };

    const std::string raw = "GET /data HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(header_value(tx, "X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(header_value(tx, "Access-Control-Allow-Origin"), "*");
}

TEST(MiddlewareTest, NullSlotIsSkipped) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/mw", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");
        res.set_status(200);
    });

    MiddlewareFn mws[] = {
        nullptr,  // null slot must be skipped gracefully
        [](const HttpRequest& req, HttpResponse& res, MiddlewareChain& chain) {
            mw_append("A"); chain.next(req, res);
        },
    };

    const std::string raw = "GET /mw HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 2);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(g_trace, "AH");
}

TEST(MiddlewareTest, NoMiddlewareRunsHandlerDirectly) {
    g_trace.clear();
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/mw", [](const HttpRequest&, HttpResponse& res) {
        mw_append("H");
        res.set_status(200);
    });

    const std::string raw = "GET /mw HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, nullptr, 0);

    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(g_trace, "H");
}

TEST(MiddlewareChainTest, StandaloneRunWithoutSocket) {
    // Exercise MiddlewareChain directly, no parsing/socket involved.
    g_trace.clear();
    char buf[256];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    HttpRequest req;

    MiddlewareFn mws[] = {
        [](const HttpRequest& r, HttpResponse& s, MiddlewareChain& c) {
            mw_append("a"); c.next(r, s); mw_append("A");
        },
    };
    HandlerFn handler = [](const HttpRequest&, HttpResponse& s) {
        mw_append("H"); s.set_status(200);
    };

    MiddlewareChain chain(mws, 1, handler);
    chain.run(req, res);
    EXPECT_EQ(g_trace, "aHA");
}

// ===========================================================================
// HttpServer middleware registration
// ===========================================================================

TEST(HttpServerTest, UseRegistersMiddleware) {
    HttpServer<MockSocket> server;
    auto mw = [](const HttpRequest& r, HttpResponse& s, MiddlewareChain& c) {
        c.next(r, s);
    };
    EXPECT_TRUE(server.use(mw));
    EXPECT_TRUE(server.use(mw));
    EXPECT_EQ(server.middleware_count(), 2u);
}

TEST(HttpServerTest, UseRespectsCapacity) {
    // MaxMiddleware template param = 2
    HttpServer<MockSocket, UCHTTP_MAX_ROUTES, UCHTTP_RX_BUFFER_SIZE,
               UCHTTP_TX_BUFFER_SIZE, 2> server;
    auto mw = [](const HttpRequest& r, HttpResponse& s, MiddlewareChain& c) {
        c.next(r, s);
    };
    EXPECT_TRUE(server.use(mw));
    EXPECT_TRUE(server.use(mw));
    EXPECT_FALSE(server.use(mw));  // full
    EXPECT_EQ(server.middleware_count(), 2u);
}

// ===========================================================================
// Requirement #3 — headers handled directly by the engine
// ===========================================================================

// Count how many HTTP responses are present in a raw TX stream.
static int count_responses(std::string_view tx) {
    int n = 0;
    size_t pos = 0;
    while ((pos = tx.find("HTTP/1.1 ", pos)) != std::string_view::npos) {
        ++n; pos += 9;
    }
    return n;
}

// --- Host accessor ---------------------------------------------------------

TEST(CoreHeadersTest, HostAccessor) {
    RouteTable<8> rt;
    static std::string captured_host;
    rt.add(HttpMethod::GET, "/h", [](const HttpRequest& req, HttpResponse& res) {
        captured_host = std::string(req.host());
        res.set_status(200);
    });

    const std::string raw =
        "GET /h HTTP/1.1\r\nHost: example.com:8080\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(captured_host, "example.com:8080");
}

// --- Content-Length / Transfer-Encoding exposure ---------------------------

TEST(CoreHeadersTest, ContentLengthExposed) {
    RouteTable<8> rt;
    static uint64_t seen_len = 0;
    static bool     seen_chunked = true;
    rt.add(HttpMethod::POST, "/c", [](const HttpRequest& req, HttpResponse& res) {
        seen_len     = req.content_length;
        seen_chunked = req.chunked;
        res.set_status(200);
    });

    std::string payload = "hello world";   // 11 bytes
    std::string raw =
        "POST /c HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(seen_len, 11u);
    EXPECT_FALSE(seen_chunked);
}

TEST(CoreHeadersTest, ChunkedTransferEncodingExposed) {
    RouteTable<8> rt;
    static bool        seen_chunked = false;
    static std::string seen_body;
    rt.add(HttpMethod::POST, "/chunk", [](const HttpRequest& req, HttpResponse& res) {
        seen_chunked = req.chunked;
        seen_body    = std::string(req.body);
        res.set_status(200);
    });

    // "Wikipedia" split into chunks per the classic RFC example.
    const std::string raw =
        "POST /chunk HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_TRUE(seen_chunked);
    EXPECT_EQ(seen_body, "Wikipedia");
}

TEST(CoreHeadersTest, HttpVersionExposed) {
    RouteTable<8> rt;
    static uint8_t major = 0, minor = 0;
    rt.add(HttpMethod::GET, "/v", [](const HttpRequest& req, HttpResponse& res) {
        major = req.http_major; minor = req.http_minor;
        res.set_status(200);
    });

    const std::string raw = "GET /v HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(major, 1);
    EXPECT_EQ(minor, 1);
}

// --- Connection header in the response -------------------------------------

TEST(CoreHeadersTest, ResponseConnectionCloseForHttp10) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/x", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    // HTTP/1.0 without keep-alive -> server must announce close.
    const std::string raw = "GET /x HTTP/1.0\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(header_value(tx, "Connection"), "close");
}

TEST(CoreHeadersTest, ResponseConnectionKeepAliveForHttp11) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/x", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    // HTTP/1.1 defaults to keep-alive.
    const std::string raw = "GET /x HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(header_value(tx, "Connection"), "keep-alive");
}

TEST(CoreHeadersTest, ExplicitConnectionCloseHonoured) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/x", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    const std::string raw =
        "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(header_value(tx, "Connection"), "close");
}

// --- Keep-alive loop: multiple requests on one connection ------------------

TEST(CoreHeadersTest, KeepAliveServesTwoRequests) {
    RouteTable<8> rt;
    static int hits = 0;
    rt.add(HttpMethod::GET, "/a", [](const HttpRequest&, HttpResponse& res) {
        ++hits; res.set_status(200); res.set_body("A");
    });
    rt.add(HttpMethod::GET, "/b", [](const HttpRequest&, HttpResponse& res) {
        ++hits; res.set_status(200); res.set_body("B");
    });

    // Two pipelined keep-alive requests, second one closes the connection.
    const std::string raw =
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    bool ok = conn.process(rt);

    EXPECT_TRUE(ok);
    EXPECT_EQ(hits, 2);                 // both handlers ran
    EXPECT_EQ(count_responses(tx), 2);  // two responses emitted
    // First response keep-alive, second close.
    auto first_conn = header_value(tx, "Connection");
    EXPECT_EQ(first_conn, "keep-alive");
}

TEST(CoreHeadersTest, CloseStopsAfterOneRequest) {
    RouteTable<8> rt;
    static int hits = 0;
    rt.add(HttpMethod::GET, "/a", [](const HttpRequest&, HttpResponse& res) {
        ++hits; res.set_status(200);
    });

    // Even though a second request is present, Connection: close on the first
    // must stop the loop and leave the second unserved.
    const std::string raw =
        "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(hits, 1);
    EXPECT_EQ(count_responses(tx), 1);
}

// --- 501 Not Implemented ---------------------------------------------------

TEST(CoreHeadersTest, UnknownMethodReturns501) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/t", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    // TRACE is a valid HTTP method llhttp parses, but uchttp does not map it
    // -> HttpMethod::UNKNOWN -> 501 Not Implemented.
    const std::string raw = "TRACE /t HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 501);
}

// --- Content-Encoding in the response --------------------------------------

TEST(CoreHeadersTest, ResponseContentEncoding) {
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    res.set_status(200);
    res.set_content_type("application/json");
    res.set_content_encoding("gzip");
    res.set_body("{}");
    MockSocket sock;
    res.send(sock);

    EXPECT_EQ(header_value(sock.tx_string(), "Content-Encoding"), "gzip");
}

TEST(CoreHeadersTest, ResponseDefaultsToConnectionClose) {
    // A standalone HttpResponse (no engine) defaults to Connection: close.
    char buf[512];
    HttpResponse res;
    res.bind(std::span<char>(buf, sizeof(buf)));
    MockSocket sock;
    res.send(sock, 200, "x");
    EXPECT_EQ(header_value(sock.tx_string(), "Connection"), "close");
}

// ===========================================================================
// Requirement #4 — security: secure_compare, auth, headers, DoS limits, TLS
// ===========================================================================

// --- secure_compare --------------------------------------------------------

TEST(SecurityTest, SecureCompareEqual) {
    EXPECT_TRUE(security::secure_compare("s3cr3t", "s3cr3t"));
    EXPECT_TRUE(security::secure_compare("", ""));
}

TEST(SecurityTest, SecureCompareDifferentContent) {
    EXPECT_FALSE(security::secure_compare("s3cr3t", "s3cr3T"));
    EXPECT_FALSE(security::secure_compare("abc", "abd"));
}

TEST(SecurityTest, SecureCompareDifferentLength) {
    EXPECT_FALSE(security::secure_compare("abc", "abcd"));
    EXPECT_FALSE(security::secure_compare("abcd", "abc"));
    EXPECT_FALSE(security::secure_compare("secret", ""));
}

// --- API key auth middleware -----------------------------------------------

TEST(SecurityTest, ApiKeyAuthAcceptsValidKey) {
    security::set_api_key("letmein");
    RouteTable<8> rt;
    static bool handler_ran = false;
    rt.add(HttpMethod::GET, "/p", [](const HttpRequest&, HttpResponse& res) {
        handler_ran = true; res.set_status(200); res.set_body("ok");
    });
    MiddlewareFn mws[] = { security::api_key_auth };

    handler_ran = false;
    const std::string raw =
        "GET /p HTTP/1.1\r\nHost: x\r\nX-API-Key: letmein\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_TRUE(handler_ran);
    EXPECT_EQ(parse_status_code(tx), 200);
}

TEST(SecurityTest, ApiKeyAuthRejectsWrongKey) {
    security::set_api_key("letmein");
    RouteTable<8> rt;
    static bool handler_ran = false;
    rt.add(HttpMethod::GET, "/p", [](const HttpRequest&, HttpResponse& res) {
        handler_ran = true; res.set_status(200);
    });
    MiddlewareFn mws[] = { security::api_key_auth };

    handler_ran = false;
    const std::string raw =
        "GET /p HTTP/1.1\r\nHost: x\r\nX-API-Key: wrong\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_FALSE(handler_ran);                 // short-circuited
    EXPECT_EQ(parse_status_code(tx), 401);
}

TEST(SecurityTest, ApiKeyAuthRejectsMissingKey) {
    security::set_api_key("letmein");
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/p", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { security::api_key_auth };

    const std::string raw = "GET /p HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 401);
}

TEST(SecurityTest, ApiKeyAuthFailsClosedWhenUnconfigured) {
    security::set_api_key("");          // no key configured
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/p", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { security::api_key_auth };

    // Even sending some key must be rejected (fail closed).
    const std::string raw =
        "GET /p HTTP/1.1\r\nHost: x\r\nX-API-Key: anything\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 401);
    security::set_api_key("letmein");   // restore for other tests
}

// --- Bearer auth middleware ------------------------------------------------

TEST(SecurityTest, BearerAuthAcceptsValidToken) {
    security::set_bearer_token("tok-123");
    RouteTable<8> rt;
    static bool handler_ran = false;
    rt.add(HttpMethod::GET, "/b", [](const HttpRequest&, HttpResponse& res) {
        handler_ran = true; res.set_status(200);
    });
    MiddlewareFn mws[] = { security::bearer_auth };

    handler_ran = false;
    const std::string raw =
        "GET /b HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer tok-123\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_TRUE(handler_ran);
    EXPECT_EQ(parse_status_code(tx), 200);
}

TEST(SecurityTest, BearerAuthRejectsWrongToken) {
    security::set_bearer_token("tok-123");
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/b", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { security::bearer_auth };

    const std::string raw =
        "GET /b HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer nope\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 401);
    EXPECT_EQ(header_value(tx, "WWW-Authenticate"), "Bearer");
}

TEST(SecurityTest, BearerAuthRejectsMissingScheme) {
    security::set_bearer_token("tok-123");
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/b", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { security::bearer_auth };

    // Token sent without the "Bearer " scheme -> rejected.
    const std::string raw =
        "GET /b HTTP/1.1\r\nHost: x\r\nAuthorization: tok-123\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 401);
}

// --- Security-headers middleware -------------------------------------------

TEST(SecurityTest, SecurityHeadersInjected) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/d", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200); res.set_body("{}");
    });
    MiddlewareFn mws[] = { security::security_headers };

    const std::string raw = "GET /d HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(header_value(tx, "X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(header_value(tx, "X-Frame-Options"), "DENY");
    EXPECT_EQ(header_value(tx, "Access-Control-Allow-Origin"), "*");
    EXPECT_EQ(header_value(tx, "Content-Security-Policy"), "default-src 'self'");
}

TEST(SecurityTest, SecurityHeadersConfigurable) {
    security::set_cors_origin("https://app.example");
    security::set_csp("default-src 'none'");
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/d", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { security::security_headers };

    const std::string raw = "GET /d HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(header_value(tx, "Access-Control-Allow-Origin"), "https://app.example");
    EXPECT_EQ(header_value(tx, "Content-Security-Policy"), "default-src 'none'");
    // Restore defaults for any later test.
    security::set_cors_origin("*");
    security::set_csp("default-src 'self'");
}

// --- DoS protection: oversize header / body --------------------------------

TEST(SecurityTest, OversizeHeadersRejectedWith431) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/x", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    // One header value far beyond UCHTTP_MAX_HEADER_BYTES (2048).
    std::string big(4000, 'A');
    const std::string raw =
        "GET /x HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket, 8192> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 431);
}

TEST(SecurityTest, OversizeBodyRejectedWith413) {
    RouteTable<8> rt;
    rt.add(HttpMethod::POST, "/x", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });

    // Body beyond UCHTTP_MAX_BODY_BYTES (4096).
    std::string body(5000, 'B');
    const std::string raw =
        "POST /x HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket, 8192> conn(std::move(sock));
    conn.process(rt);

    EXPECT_EQ(parse_status_code(tx), 413);
}

// --- TLS abstraction: TlsSocket<Session> satisfies SocketConcept -----------

namespace {
// Minimal in-memory TLS session satisfying TlsSessionConcept, so we can verify
// the abstraction compiles and TlsSocket models SocketConcept without needing
// a real crypto library linked into the unit tests.
class FakeTlsSession {
public:
    FakeTlsSession() = default;
    bool handshake() noexcept { handshaken_ = true; return true; }
    int  read(std::span<uint8_t>) noexcept { return 0; }
    int  write(std::span<const uint8_t> b) noexcept {
        return static_cast<int>(b.size());
    }
    void shutdown() noexcept { valid_ = false; }
    bool is_valid() const noexcept { return valid_; }
    bool set_read_timeout(unsigned) noexcept { return true; }
private:
    bool handshaken_ { false };
    bool valid_      { true };
};
} // namespace

static_assert(TlsSessionConcept<FakeTlsSession>,
              "FakeTlsSession must satisfy TlsSessionConcept");
static_assert(SocketConcept<TlsSocket<FakeTlsSession>>,
              "TlsSocket must satisfy SocketConcept");

TEST(TlsTest, TlsSocketModelsSocketConcept) {
    TlsSocket<FakeTlsSession> sock{ FakeTlsSession{} };
    EXPECT_TRUE(sock.is_valid());
    // Lazy handshake on first write, then data passes through.
    const uint8_t data[] = { 'h', 'i' };
    EXPECT_EQ(sock.write(std::span<const uint8_t>(data, 2)), 2);
    EXPECT_TRUE(sock.set_read_timeout(1000));
    sock.close();
    EXPECT_FALSE(sock.is_valid());
}

TEST(TlsTest, TlsServerTypeCompiles) {
    // The whole engine must instantiate over a TLS socket type unchanged.
    using TlsSock = TlsSocket<FakeTlsSession>;
    HttpServer<TlsSock> server;
    auto h = [](const HttpRequest&, HttpResponse&) {};
    EXPECT_TRUE(server.get("/secure", h));
    EXPECT_EQ(server.routes().size(), 1u);
}

// ===========================================================================
// Requirement #5 — application middleware (cache / etag / request-id)
// ===========================================================================

TEST(AppMiddlewareTest, CacheControlInjected) {
    app::set_cache_control("public, max-age=3600");
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/c", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200); res.set_body("x");
    });
    MiddlewareFn mws[] = { app::cache_control };

    const std::string raw = "GET /c HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(header_value(tx, "Cache-Control"), "public, max-age=3600");
    app::set_cache_control("no-cache");   // restore default
}

TEST(AppMiddlewareTest, ComputeEtagStableAndQuoted) {
    char a[20], b[20];
    size_t na = app::compute_etag("hello", a, sizeof(a));
    size_t nb = app::compute_etag("hello", b, sizeof(b));
    ASSERT_EQ(na, 18u);
    EXPECT_EQ(std::string_view(a, na), std::string_view(b, nb));  // deterministic
    EXPECT_EQ(a[0], '"');
    EXPECT_EQ(a[17], '"');
    // Different body -> different tag.
    char c[20];
    app::compute_etag("world", c, sizeof(c));
    EXPECT_NE(std::string_view(a, 18), std::string_view(c, 18));
}

TEST(AppMiddlewareTest, EtagHeaderAdded) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/e", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200); res.set_body("payload");
    });
    MiddlewareFn mws[] = { app::etag };

    const std::string raw = "GET /e HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 200);
    char expect[20];
    app::compute_etag("payload", expect, sizeof(expect));
    EXPECT_EQ(header_value(tx, "ETag"), std::string_view(expect, 18));
}

TEST(AppMiddlewareTest, IfNoneMatchReturns304) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/e", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200); res.set_body("payload");
    });
    MiddlewareFn mws[] = { app::etag };

    char tag[20];
    app::compute_etag("payload", tag, sizeof(tag));
    std::string raw =
        "GET /e HTTP/1.1\r\nHost: x\r\nIf-None-Match: " +
        std::string(tag, 18) + "\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(parse_status_code(tx), 304);
    // 304 must carry no body.
    auto pos = tx.find("\r\n\r\n");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(tx.substr(pos + 4), "");
}

TEST(AppMiddlewareTest, AcceptEncodingDetection) {
    RouteTable<8> rt;
    static bool gzip = false, br = false, deflate = false;
    rt.add(HttpMethod::GET, "/a", [](const HttpRequest& req, HttpResponse& res) {
        gzip    = app::client_accepts_encoding(req, "gzip");
        br      = app::client_accepts_encoding(req, "br");
        deflate = app::client_accepts_encoding(req, "deflate");
        res.set_status(200);
    });

    const std::string raw =
        "GET /a HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, deflate\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, nullptr, 0);

    EXPECT_TRUE(gzip);
    EXPECT_TRUE(deflate);
    EXPECT_FALSE(br);
}

TEST(AppMiddlewareTest, RequestIdEchoed) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/r", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { app::request_id };

    const std::string raw =
        "GET /r HTTP/1.1\r\nHost: x\r\nX-Request-ID: abc-123\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    EXPECT_EQ(header_value(tx, "X-Request-ID"), "abc-123");
}

TEST(AppMiddlewareTest, RequestIdGeneratedWhenAbsent) {
    RouteTable<8> rt;
    rt.add(HttpMethod::GET, "/r", [](const HttpRequest&, HttpResponse& res) {
        res.set_status(200);
    });
    MiddlewareFn mws[] = { app::request_id };

    const std::string raw = "GET /r HTTP/1.1\r\nHost: x\r\n\r\n";
    std::string tx;
    RefMockSocket sock(raw, tx);
    HttpConnection<RefMockSocket> conn(std::move(sock));
    conn.process(rt, mws, 1);

    auto id = header_value(tx, "X-Request-ID");
    EXPECT_EQ(id.size(), 16u);           // 16 hex digits
}

// ===========================================================================
// Requirement #6 — ConnectionPool + graceful shutdown
// ===========================================================================

TEST(ConnectionPoolTest, AcquireReleaseReuse) {
    ConnectionPool<RefMockSocket, 2> pool;
    EXPECT_EQ(pool.capacity(), 2u);
    EXPECT_EQ(pool.active(), 0u);
    EXPECT_FALSE(pool.full());

    std::string tx1;
    auto* c1 = pool.acquire(RefMockSocket("", tx1));
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(pool.active(), 1u);

    std::string tx2;
    auto* c2 = pool.acquire(RefMockSocket("", tx2));
    ASSERT_NE(c2, nullptr);
    EXPECT_TRUE(pool.full());
    EXPECT_NE(c1, c2);                    // distinct slots

    pool.release(c1);
    EXPECT_EQ(pool.active(), 1u);
    EXPECT_FALSE(pool.full());

    // A new acquire reuses the freed slot (no allocation).
    std::string tx3;
    auto* c3 = pool.acquire(RefMockSocket("", tx3));
    ASSERT_NE(c3, nullptr);
    EXPECT_EQ(c3, c1);                    // same storage reused
}

TEST(ConnectionPoolTest, ExhaustionReturnsNullptr) {
    ConnectionPool<RefMockSocket, 1> pool;
    std::string tx1;
    auto* c1 = pool.acquire(RefMockSocket("", tx1));
    ASSERT_NE(c1, nullptr);
    std::string tx2;
    auto* c2 = pool.acquire(RefMockSocket("", tx2));
    EXPECT_EQ(c2, nullptr);               // pool exhausted
}

TEST(ConnectionPoolTest, CloseAllGracefulShutdown) {
    ConnectionPool<RefMockSocket, 3> pool;
    std::string t1, t2, t3;
    pool.acquire(RefMockSocket("", t1));
    pool.acquire(RefMockSocket("", t2));
    pool.acquire(RefMockSocket("", t3));
    EXPECT_EQ(pool.active(), 3u);

    pool.close_all();
    EXPECT_EQ(pool.active(), 0u);
    EXPECT_FALSE(pool.full());

    // Pool is reusable after shutdown.
    std::string t4;
    auto* c = pool.acquire(RefMockSocket("", t4));
    EXPECT_NE(c, nullptr);
}

TEST(ConnectionPoolTest, PooledConnectionServesRequest) {
    RouteTable<8> rt;
    static bool ran = false;
    rt.add(HttpMethod::GET, "/p", [](const HttpRequest&, HttpResponse& res) {
        ran = true; res.set_status(200); res.set_body("pooled");
    });

    ConnectionPool<RefMockSocket, 2> pool;
    std::string tx;
    auto* conn = pool.acquire(
        RefMockSocket("GET /p HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", tx));
    ASSERT_NE(conn, nullptr);
    conn->process(rt);
    pool.release(conn);

    EXPECT_TRUE(ran);
    EXPECT_EQ(parse_status_code(tx), 200);
    EXPECT_EQ(pool.active(), 0u);
}

TEST(HttpServerTest, PoolIntrospection) {
    HttpServer<MockSocket> server;
    EXPECT_EQ(server.active_connections(), 0u);
    EXPECT_EQ(server.max_connections(), UCHTTP_MAX_CONNECTIONS);
    server.shutdown();                    // safe to call when idle
    EXPECT_EQ(server.active_connections(), 0u);
}
