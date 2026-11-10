// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#include "uchttp.h"

namespace uchttp::ws {

namespace {

char lower(char c) noexcept
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ci(std::string_view a, std::string_view b) noexcept
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (lower(a[i]) != lower(b[i])) return false;
	return true;
}

std::string_view trim(std::string_view s) noexcept
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

} // namespace

std::string_view find_header(const HttpRequest &req, std::string_view name) noexcept
{
	for (uint8_t i = 0; i < req.header_count; ++i) {
		if (equals_ci(req.headers[i].name, name))
			return trim(req.headers[i].value);
	}
	return {};
}

bool header_has_token(std::string_view list, std::string_view token,
		      bool ignore_case) noexcept
{
	while (!list.empty()) {
		size_t comma = list.find(',');
		std::string_view item = trim(list.substr(0, comma));
		if (ignore_case ? equals_ci(item, token) : item == token)
			return true;
		if (comma == std::string_view::npos) break;
		list.remove_prefix(comma + 1);
	}
	return false;
}

int validate_handshake(const HttpRequest &req, std::string_view &key) noexcept
{
	/* RFC 6455 4.2.1. A plain GET to the endpoint (no Upgrade) and a wrong
	 * version are answered with 426 so that the client learns what to do;
	 * a malformed key is a plain 400. */
	if (req.method != HttpMethod::GET) return 400;
	if (req.http_major < 1 || (req.http_major == 1 && req.http_minor < 1))
		return 400;                                  /* needs HTTP/1.1 */

	if (!header_has_token(find_header(req, "Upgrade"), "websocket", true) ||
	    !header_has_token(find_header(req, "Connection"), "upgrade", true))
		return 426;

	if (find_header(req, "Sec-WebSocket-Version") != "13") return 426;

	key = find_header(req, "Sec-WebSocket-Key");
	if (!key_valid(key)) return 400;

	return 0;
}

} // namespace uchttp::ws
