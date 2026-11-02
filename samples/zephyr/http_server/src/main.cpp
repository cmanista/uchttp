/*
 * uchttp on Zephyr — Stage 1 example (HTTP, no TLS).
 *
 * Instantiates HttpServer<ZephyrSocket> and serves a few routes over the
 * Zephyr BSD-socket stack. The core engine is unchanged from the host build;
 * only the socket/acceptor port differs.
 */

/* uchttp headers first: llhttp.h declares a field named `_current`, which
 * collides with Zephyr's `_current` macro from <zephyr/kernel.h>. Including the
 * parser before the kernel headers avoids the macro clash. */
#include <cstdio>

#include "uchttp.h"
#include "uchttp_app_middleware.h"
#include "zephyr_socket.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uchttp_sample, LOG_LEVEL_INF);

namespace {

constexpr uint16_t HTTP_PORT = 8080;

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

} // namespace

int main(void)
{
	LOG_INF("Starting uchttp HTTP server on port %u", HTTP_PORT);

	static uchttp::HttpServer<uchttp::ZephyrSocket> server;
	static uchttp::ZephyrAcceptor acceptor(HTTP_PORT);

	/* Optional application middleware (requirement #5). */
	server.use(uchttp::app::request_id);

	server.get("/",                  handle_root);
	server.get("/api/hello",         handle_hello);
	server.get("/api/sensors/{id}",  handle_sensor);

	if (!acceptor.start()) {
		LOG_ERR("Failed to bind/listen on port %u", HTTP_PORT);
		return -1;
	}

	LOG_INF("Listening — try: curl http://<device-ip>:%u/api/hello", HTTP_PORT);

	/* Blocking accept/serve loop. On an RTOS this typically runs in its own
	 * thread; main() is fine for a single-purpose sample. */
	server.run(acceptor);

	return 0;
}
