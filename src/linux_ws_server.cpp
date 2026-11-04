// linux_ws_server.cpp - WebSocket example (requirement #8) running on a PC
//
//   cmake -B build && cmake --build build
//   ./build/uchttp_ws_server            # http://localhost:8080
//
// Try it:
//   open http://localhost:8080/ in a browser         (page with a JS client)
//   wscat -c ws://localhost:8080/ws                   (type text, get echoes)
//   curl -i -H 'Upgrade: websocket' ... /ws           (handshake only)
//
// /ws behaviour:
//   - on open     : sends {"event":"hello"}
//   - on message  : echoes it back ("echo: <text>" for text, raw for binary);
//                   the text "close" makes the server start the close handshake
//   - every second: pushes a telemetry JSON (on_idle) - server push without
//                   any extra thread
//
// HttpServer serves one connection at a time and a WebSocket keeps its
// connection for as long as it lives, so this example runs several worker
// threads, each with its own HttpServer, all accepting on the same socket.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "uchttp.h"
#include "linux_socket.h"
#include "examples/ws_demo.h"

static uchttp::LinuxAcceptor *g_acceptor = nullptr;

static void on_signal(int)
{
	if (g_acceptor) g_acceptor->stop();
}

// ---------------------------------------------------------------------------
// WebSocket handlers - plain function pointers, state through Session::user()
// ---------------------------------------------------------------------------

struct ClientState {
	unsigned seq { 0 };
	char     echo[UCHTTP_WS_MAX_MESSAGE_SIZE + 8];   // "echo: " + message
};

static void ws_open(uchttp::ws::Session &s, const uchttp::HttpRequest &req)
{
	std::printf("[WS] open  %.*s\n", static_cast<int>(req.path.size()),
		    req.path.data());
	static_cast<ClientState *>(s.user())->seq = 0;
	s.send_text("{\"event\":\"hello\"}");
}

static void ws_message(uchttp::ws::Session &s, uchttp::ws::Opcode type,
		       std::span<const uint8_t> data)
{
	std::string_view text(reinterpret_cast<const char *>(data.data()), data.size());
	if (type == uchttp::ws::Opcode::Text) {
		if (text == "close") {
			s.close(uchttp::ws::close_code::normal, "bye");
			return;
		}
		auto *st = static_cast<ClientState *>(s.user());
		std::memcpy(st->echo, "echo: ", 6);
		std::memcpy(st->echo + 6, text.data(), text.size());
		s.send_text(std::string_view(st->echo, 6 + text.size()));
	} else {
		s.send_binary(data);
	}
}

static void ws_idle(uchttp::ws::Session &s)
{
	auto *st = static_cast<ClientState *>(s.user());
	char buf[64];
	size_t n = uchttp::demo::format_telemetry(buf, sizeof(buf), st->seq++);
	s.send_text(std::string_view(buf, n));
}

static void ws_close(uchttp::ws::Session &, uint16_t code)
{
	std::printf("[WS] close code=%u\n", code);
}

// One handler set per worker thread: its `user` points at that thread's state.
struct Worker {
	ClientState         state;
	uchttp::ws::Handlers handlers;
};

static void handle_root(const uchttp::HttpRequest &, uchttp::HttpResponse &res)
{
	res.set_status(200);
	res.set_content_type("text/html");
	res.set_body(uchttp::demo::ws_page);
}

static void worker_main(uchttp::LinuxAcceptor *acceptor, Worker *w)
{
	w->handlers.user       = &w->state;
	w->handlers.on_open    = ws_open;
	w->handlers.on_message = ws_message;
	w->handlers.on_idle    = ws_idle;
	w->handlers.on_close   = ws_close;
	w->handlers.idle_interval_ms = 1000;

	uchttp::HttpServer<uchttp::LinuxSocket> server;
	server.get("/", handle_root);
	server.websocket("/ws", w->handlers);
	server.run(*acceptor);
}

int main()
{
	constexpr int kWorkers = 4;
	std::setvbuf(stdout, nullptr, _IOLBF, 0);   // visible when redirected to a file
	uchttp::LinuxAcceptor acceptor(8080);
	if (!acceptor.start()) {
		std::fprintf(stderr, "cannot listen on port 8080\n");
		return 1;
	}
	g_acceptor = &acceptor;
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	std::printf("uchttp WebSocket demo on http://localhost:8080  (ws://localhost:8080/ws)\n");

	static Worker workers[kWorkers];
	std::vector<std::thread> threads;
	for (int i = 0; i < kWorkers; ++i)
		threads.emplace_back(worker_main, &acceptor, &workers[i]);
	for (auto &t : threads) t.join();

	std::printf("\nstopped\n");
	return 0;
}
