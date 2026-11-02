/*
 * uchttp on Zephyr — Stage 1 example (HTTP, no TLS) with WiFi + shell.
 *
 * The HTTP server runs in its own thread so the shell stays responsive: you
 * configure WiFi interactively from the console, and the server begins serving
 * once the interface comes up (DHCP). Example console session on FRDM-RW612:
 *
 *   uart:~$ wifi connect -s <SSID> -k 1 -p <PASSWORD>
 *   uart:~$ net iface                 # note the DHCP IPv4 address
 *   $ curl http://<device-ip>:8080/api/hello
 */

/* uchttp headers first: llhttp.h declares a field named `_current`, which
 * collides with Zephyr's `_current` macro from <zephyr/kernel.h>. */
#include <cstdio>

#include "uchttp.h"
#include "uchttp_app_middleware.h"
#include "zephyr_socket.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

LOG_MODULE_REGISTER(uchttp_sample, LOG_LEVEL_INF);

namespace {

constexpr uint16_t HTTP_PORT = 8080;

constexpr size_t  SERVER_STACK_SIZE = 4096;
K_THREAD_STACK_DEFINE(server_stack, SERVER_STACK_SIZE);
k_thread server_thread_data;

void handle_root(const uchttp::HttpRequest& /*req*/, uchttp::HttpResponse& res) {
	res.set_status(200);
	res.set_content_type("text/html");
	res.set_body("<html><body><h1>uchttp on Zephyr</h1></body></html>");
}

void handle_hello(const uchttp::HttpRequest& /*req*/, uchttp::HttpResponse& res) {
	res.set_status(200);
	res.set_content_type("application/json");
	res.set_body(R"({"message":"Hello from uchttp on Zephyr!"})");
}

void handle_sensor(const uchttp::HttpRequest& req, uchttp::HttpResponse& res) {
	std::string_view id = req.path_param("id");
	static char buf[96];
	int n = snprintf(buf, sizeof(buf),
			 R"({"sensor":"%.*s","value":23.5})",
			 static_cast<int>(id.size()), id.data());
	res.set_status(200);
	res.set_content_type("application/json");
	res.set_body(std::string_view(buf, static_cast<size_t>(n)));
}

/* Block until the default interface has an IPv4 address (assigned by DHCP
 * after the user runs `wifi connect` from the shell). */
void wait_for_ipv4(void)
{
	while (true) {
		net_if *iface = net_if_get_default();
		if (iface != nullptr &&
		    net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != nullptr) {
			return;
		}
		k_sleep(K_MSEC(500));
	}
}

void server_thread(void *, void *, void *)
{
	LOG_INF("uchttp: waiting for WiFi/IPv4 "
		"(use `wifi connect -s <SSID> -k 1 -p <PASS>`)");
	wait_for_ipv4();

	static uchttp::HttpServer<uchttp::ZephyrSocket> server;
	static uchttp::ZephyrAcceptor acceptor(HTTP_PORT);

	server.use(uchttp::app::request_id);
	server.get("/",                 handle_root);
	server.get("/api/hello",        handle_hello);
	server.get("/api/sensors/{id}", handle_sensor);

	if (!acceptor.start()) {
		LOG_ERR("Failed to bind/listen on port %u", HTTP_PORT);
		return;
	}
	LOG_INF("uchttp: listening on port %u — curl http://<device-ip>:%u/api/hello",
		HTTP_PORT, HTTP_PORT);

	server.run(acceptor);   /* blocking accept/serve loop */
}

} // namespace

int main(void)
{
	LOG_INF("uchttp WiFi sample. Configure WiFi from the console, then the "
		"HTTP server starts automatically.");

	k_thread_create(&server_thread_data, server_stack, SERVER_STACK_SIZE,
			server_thread, nullptr, nullptr, nullptr,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&server_thread_data, "uchttp");

	/* main returns; the shell owns the console for WiFi configuration. */
	return 0;
}
