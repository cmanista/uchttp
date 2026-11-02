/*
 * uchttp on Zephyr — Stage 2 example (HTTPS/TLS) with WiFi + shell.
 *
 * Same structure as the Stage 1 HTTP sample, but the server runs over TLS:
 * credentials are registered with uchttp::ZephyrTlsCredentials and the server
 * listens through uchttp::ZephyrTlsAcceptor. The crypto is Zephyr's native TLS
 * (Mbed TLS / PSA Crypto); HttpServer<ZephyrSocket> is unchanged because TLS is
 * transparent at the socket layer.
 *
 * Console session on FRDM-RW612:
 *
 *   uart:~$ wifi connect -s <SSID> -k 1 -p <PASSWORD>
 *   uart:~$ net iface                 # note the DHCP IPv4 address
 *   $ curl -k https://<device-ip>:8443/api/hello
 *
 * -k tells curl to accept the self-signed demo certificate in certificate.h.
 */

#include <cstdio>

#include "uchttp.h"
#include "uchttp_app_middleware.h"
#include "zephyr_socket.h"
#include "zephyr_tls.h"

/* Embedded self-signed server certificate + private key (DEMO ONLY). */
#include "certificate.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

LOG_MODULE_REGISTER(uchttp_https_sample, LOG_LEVEL_INF);

namespace {

constexpr uint16_t HTTPS_PORT = 8443;

constexpr size_t  SERVER_STACK_SIZE = 8192;   /* TLS handshake needs headroom */
K_THREAD_STACK_DEFINE(server_stack, SERVER_STACK_SIZE);
k_thread server_thread_data;

void handle_root(const uchttp::HttpRequest & /*req*/, uchttp::HttpResponse &res) {
	res.set_status(200);
	res.set_content_type("text/html");
	res.set_body("<html><body><h1>uchttp on Zephyr (HTTPS)</h1></body></html>");
}

void handle_hello(const uchttp::HttpRequest & /*req*/, uchttp::HttpResponse &res) {
	res.set_status(200);
	res.set_content_type("application/json");
	res.set_body(R"({"message":"Hello over TLS from uchttp on Zephyr!"})");
}

void handle_sensor(const uchttp::HttpRequest &req, uchttp::HttpResponse &res) {
	std::string_view id = req.path_param("id");
	static char buf[96];
	int n = snprintf(buf, sizeof(buf),
			 R"({"sensor":"%.*s","value":23.5})",
			 static_cast<int>(id.size()), id.data());
	res.set_status(200);
	res.set_content_type("application/json");
	res.set_body(std::string_view(buf, static_cast<size_t>(n)));
}

/* Block until the default interface has an IPv4 address (DHCP after the user
 * runs `wifi connect` from the shell). */
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

bool register_tls_credentials(void)
{
	/* The credential store keeps pointers to these buffers, so they must
	 * outlive every TLS socket — here they are `static const` in flash. */
	if (!uchttp::ZephyrTlsCredentials::add_server_cert(
		    UCHTTP_TLS_SERVER_CERT_TAG,
		    server_certificate, server_certificate_len,
		    private_key,        private_key_len)) {
		LOG_ERR("Failed to register TLS server certificate/key");
		return false;
	}
	return true;
}

void server_thread(void *, void *, void *)
{
	LOG_INF("uchttp: waiting for WiFi/IPv4 "
		"(use `wifi connect -s <SSID> -k 1 -p <PASS>`)");
	wait_for_ipv4();

	if (!register_tls_credentials()) {
		return;
	}

	static uchttp::HttpServer<uchttp::ZephyrSocket> server;
	static uchttp::ZephyrTlsAcceptor acceptor(HTTPS_PORT,
						  UCHTTP_TLS_SERVER_CERT_TAG);

	server.use(uchttp::app::request_id);
	server.get("/",                 handle_root);
	server.get("/api/hello",        handle_hello);
	server.get("/api/sensors/{id}", handle_sensor);

	if (!acceptor.start()) {
		LOG_ERR("Failed to bind/listen (TLS) on port %u", HTTPS_PORT);
		return;
	}
	LOG_INF("uchttp: HTTPS listening on port %u — "
		"curl -k https://<device-ip>:%u/api/hello",
		HTTPS_PORT, HTTPS_PORT);

	server.run(acceptor);   /* blocking accept/serve loop, now over TLS */
}

} // namespace

int main(void)
{
	LOG_INF("uchttp HTTPS WiFi sample. Configure WiFi from the console, then "
		"the TLS server starts automatically.");

	k_thread_create(&server_thread_data, server_stack, SERVER_STACK_SIZE,
			server_thread, nullptr, nullptr, nullptr,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&server_thread_data, "uchttp-tls");

	return 0;
}
