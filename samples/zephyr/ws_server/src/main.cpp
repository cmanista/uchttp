/*
 * uchttp on Zephyr - WebSocket example (requirement #8) with WiFi + shell.
 *
 *   http://<device-ip>:8080/      page with a JavaScript WebSocket client
 *   ws://<device-ip>:8080/ws      echo + a telemetry message every second
 *
 * Platform plug-ins used (the WebSocket core itself is platform independent):
 *   - ZephyrSlabAllocator : the per-session buffer comes from a k_mem_slab
 *   - ZephyrPsaSha1       : Sec-WebSocket-Accept via PSA Crypto (HW where the
 *                           PSA driver has SHA-1, else Mbed TLS software)
 *
 * HttpServer serves one connection at a time and a WebSocket keeps its
 * connection for as long as it lives, so the example starts
 * CONFIG_APP_SERVER_THREADS server threads - each with its own HttpServer -
 * that accept on one listening socket: that many simultaneous clients.
 *
 * WiFi: set CONFIG_APP_WIFI_SSID / CONFIG_APP_WIFI_PSK (e.g. in a local
 * `local.conf`, see the README) to join automatically, or leave them empty
 * and connect from the console:
 *
 *   uart:~$ wifi connect -s <SSID> -k 1 -p <PASSWORD>
 */

/* uchttp headers first: llhttp.h declares a field named `_current`, which
 * collides with Zephyr's `_current` macro from <zephyr/kernel.h>. */
#include <cstdio>
#include <cstring>

#include "uchttp.h"
#include "zephyr_socket.h"
#include "zephyr_ws.h"
#ifdef CONFIG_UCHTTP_WS_SHA1_PSA
#include "zephyr_psa_sha1.h"
#endif
#include "ws_demo.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(uchttp_ws_sample, LOG_LEVEL_INF);

namespace {

constexpr uint16_t HTTP_PORT = 8080;
constexpr int      NUM_SERVERS = CONFIG_APP_SERVER_THREADS;
constexpr size_t   SERVER_STACK_SIZE = 6144;

/* --- platform plug-ins ---------------------------------------------------- */

#ifdef CONFIG_UCHTTP_WS_SHA1_PSA
using Sha1 = uchttp::ZephyrPsaSha1;
#else
using Sha1 = uchttp::ws::SoftSha1;
#endif
using WsAlloc  = uchttp::ZephyrSlabAllocator<1, UCHTTP_WS_BUFFER_SIZE>;
using WsTraits = uchttp::ws::Traits<Sha1, WsAlloc>;

using Server = uchttp::HttpServer<uchttp::ZephyrSocket, 4,
				  UCHTTP_RX_BUFFER_SIZE, UCHTTP_TX_BUFFER_SIZE,
				  UCHTTP_MAX_MIDDLEWARE, UCHTTP_MAX_CONNECTIONS,
				  WsTraits>;

/* --- WebSocket handlers --------------------------------------------------- */

struct ClientState {
	unsigned seq;
	char     echo[UCHTTP_WS_MAX_MESSAGE_SIZE + 8];   /* "echo: " + message */
};

void ws_open(uchttp::ws::Session &s, const uchttp::HttpRequest &req)
{
	char path[48];
	std::snprintf(path, sizeof(path), "%.*s", static_cast<int>(req.path.size()),
		      req.path.data());
	LOG_INF("ws: open %s", path);

	static_cast<ClientState *>(s.user())->seq = 0;
	s.send_text("{\"event\":\"hello\",\"board\":\"" CONFIG_BOARD "\"}");
}

void ws_message(uchttp::ws::Session &s, uchttp::ws::Opcode type,
		std::span<const uint8_t> data)
{
	auto *st = static_cast<ClientState *>(s.user());

	if (type != uchttp::ws::Opcode::Text) {
		s.send_binary(data);                       /* binary: echoed as is */
		return;
	}
	std::string_view text(reinterpret_cast<const char *>(data.data()), data.size());
	if (text == "close") {
		s.close(uchttp::ws::close_code::normal, "bye");
		return;
	}
	std::memcpy(st->echo, "echo: ", 6);
	std::memcpy(st->echo + 6, text.data(), text.size());
	s.send_text(std::string_view(st->echo, 6 + text.size()));
}

/* The read timeout expired: push a telemetry sample. No extra thread needed. */
void ws_idle(uchttp::ws::Session &s)
{
	auto *st = static_cast<ClientState *>(s.user());
	char buf[96];
	int n = std::snprintf(buf, sizeof(buf),
			      "{\"seq\":%u,\"uptime_ms\":%lld,\"temp\":%u.%u}",
			      st->seq, static_cast<long long>(k_uptime_get()),
			      20u + (st->seq % 7), (st->seq * 3) % 10);
	++st->seq;
	if (n > 0) s.send_text(std::string_view(buf, static_cast<size_t>(n)));
}

void ws_close(uchttp::ws::Session &, uint16_t code)
{
	LOG_INF("ws: closed, code %u", code);
}

void handle_root(const uchttp::HttpRequest &, uchttp::HttpResponse &res)
{
	res.set_status(200);
	res.set_content_type("text/html");
	res.set_body(uchttp::demo::ws_page);
}

/* --- server threads ------------------------------------------------------- */

struct Worker {
	Server               server;
	ClientState          client;
	uchttp::ws::Handlers handlers;
	k_thread             thread;
};

Worker              workers[NUM_SERVERS];
K_THREAD_STACK_ARRAY_DEFINE(server_stacks, NUM_SERVERS, SERVER_STACK_SIZE);
uchttp::ZephyrAcceptor acceptor(HTTP_PORT, NUM_SERVERS + 2);

void server_thread(void *arg1, void *, void *)
{
	auto *w = static_cast<Worker *>(arg1);

	w->handlers.user       = &w->client;
	w->handlers.on_open    = ws_open;
	w->handlers.on_message = ws_message;
	w->handlers.on_idle    = ws_idle;
	w->handlers.on_close   = ws_close;

	w->server.get("/", handle_root);
	w->server.websocket("/ws", w->handlers);

	w->server.run(acceptor);      /* blocking accept/serve loop */
}

/* --- network bring-up ----------------------------------------------------- */

bool have_ipv4(char *out, size_t len)
{
	net_if *iface = net_if_get_default();
	if (iface == nullptr) return false;
	in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (a == nullptr) return false;
	net_addr_ntop(NET_AF_INET, a, out, len);
	return true;
}

void wifi_autoconnect()
{
	if (sizeof(CONFIG_APP_WIFI_SSID) <= 1) return;   // empty: connect from the shell

	net_if *iface = net_if_get_default();
	wifi_connect_req_params params {};

	params.ssid        = reinterpret_cast<const uint8_t *>(CONFIG_APP_WIFI_SSID);
	params.ssid_length = sizeof(CONFIG_APP_WIFI_SSID) - 1;
	params.psk         = reinterpret_cast<const uint8_t *>(CONFIG_APP_WIFI_PSK);
	params.psk_length  = sizeof(CONFIG_APP_WIFI_PSK) - 1;
	params.security    = params.psk_length ? WIFI_SECURITY_TYPE_PSK
					       : WIFI_SECURITY_TYPE_NONE;
	params.channel     = WIFI_CHANNEL_ANY;
	params.band        = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp         = WIFI_MFP_OPTIONAL;

	LOG_INF("wifi: joining \"%s\"", CONFIG_APP_WIFI_SSID);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (rc != 0) LOG_ERR("wifi: connect request failed (%d)", rc);
}

} // namespace

int main(void)
{
	LOG_INF("uchttp WebSocket sample (%d server threads)", NUM_SERVERS);

	if (sizeof(CONFIG_APP_WIFI_SSID) > 1) {
		/* The WiFi interface needs a moment after boot before it takes requests. */
		k_sleep(K_SECONDS(3));
		wifi_autoconnect();
	} else {
		LOG_INF("wifi: connect from the console: "
			"wifi connect -s <SSID> -k 1 -p <PASSWORD>");
	}

	char ip[NET_IPV4_ADDR_LEN];
	while (!have_ipv4(ip, sizeof(ip))) {
		k_sleep(K_MSEC(500));
	}
	LOG_INF("net: IPv4 address %s", ip);

	if (!acceptor.start()) {
		LOG_ERR("cannot listen on port %u", HTTP_PORT);
		return 0;
	}
	LOG_INF("ready: http://%s:%u/   ws://%s:%u/ws", ip, HTTP_PORT, ip, HTTP_PORT);

	for (int i = 0; i < NUM_SERVERS; ++i) {
		k_thread_create(&workers[i].thread, server_stacks[i], SERVER_STACK_SIZE,
				server_thread, &workers[i], nullptr, nullptr,
				K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
		k_thread_name_set(&workers[i].thread, "uchttp_ws");
	}
	return 0;          /* the shell keeps the console */
}
