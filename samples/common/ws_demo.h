// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

/*
 * ws_demo.h - pieces shared by the WebSocket examples (Linux and Zephyr):
 * a tiny browser client served at "/" and the telemetry message format.
 *
 * Platform independent and header only; the platform specific parts (sockets,
 * logging, threads) live in the examples themselves.
 */

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace uchttp::demo {

/* Single-page client: connects to /ws on the host it was loaded from, prints
 * every message and sends the text box content on demand. Kept small - on an
 * MCU it is served from flash through the TX buffer. */
inline constexpr std::string_view ws_page =
	"<!doctype html><meta charset=utf-8><title>uchttp ws</title>"
	"<h3>uchttp WebSocket</h3>"
	"<input id=m value=hello> <button onclick=s()>send</button>"
	"<pre id=l></pre>"
	"<script>"
	"var l=document.getElementById('l'),"
	"w=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');"
	"function p(t){l.textContent=t+'\\n'+l.textContent.slice(0,1500)}"
	"w.onopen=function(){p('open')};"
	"w.onclose=function(){p('closed')};"
	"w.onmessage=function(e){p(e.data)};"
	"function s(){w.send(document.getElementById('m').value)}"
	"</script>";

/* Format one fake sensor reading as JSON. Returns the length written. */
inline size_t format_telemetry(char *buf, size_t cap, unsigned seq) noexcept
{
	int n = std::snprintf(buf, cap, "{\"seq\":%u,\"temp\":%u.%u}", seq,
			      20u + (seq % 7), (seq * 3) % 10);
	return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

} // namespace uchttp::demo
