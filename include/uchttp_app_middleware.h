#pragma once

// uchttp_app_middleware.h — optional application middleware (requirement #5).
//
// "Headers handled by middleware" — caching, compression negotiation and
// diagnostics. These are NOT owned by the engine; the user opts in per server:
//
//   uchttp::app::set_cache_control("public, max-age=3600");
//   server.use(uchttp::app::cache_control);
//   server.use(uchttp::app::etag);          // adds ETag + handles If-None-Match
//   server.use(uchttp::app::request_id);    // X-Request-ID for tracing
//
// All non-template; bodies live in uchttp_app_middleware.cpp (no heap).

#include <cstdint>
#include <string_view>

#include "uchttp.h"

namespace uchttp::app {

// ---------------------------------------------------------------------------
// Cache-Control
// ---------------------------------------------------------------------------

// Set the Cache-Control value injected by the cache_control middleware.
// Default: "no-cache". Pass {} to disable.
void set_cache_control(std::string_view value) noexcept;

// Injects the configured Cache-Control header into every response.
void cache_control(const HttpRequest& req, HttpResponse& res,
                   MiddlewareChain& chain) noexcept;

// ---------------------------------------------------------------------------
// ETag + conditional GET (If-None-Match -> 304 Not Modified)
// ---------------------------------------------------------------------------

// Computes a weak-ish ETag from the response body (FNV-1a) and sets the ETag
// header. If the request carries a matching If-None-Match, the response is
// turned into 304 Not Modified with an empty body (bandwidth saving).
void etag(const HttpRequest& req, HttpResponse& res,
          MiddlewareChain& chain) noexcept;

// Compute the ETag string for a body into `out` (must hold >= 18 bytes).
// Returns the written length. Exposed for testing / manual use.
size_t compute_etag(std::string_view body, char* out, size_t cap) noexcept;

// ---------------------------------------------------------------------------
// Accept-Encoding negotiation
// ---------------------------------------------------------------------------

// True if the request's Accept-Encoding advertises `coding` (e.g. "gzip").
bool client_accepts_encoding(const HttpRequest& req,
                             std::string_view coding) noexcept;

// ---------------------------------------------------------------------------
// X-Request-ID (tracing / diagnostics)
// ---------------------------------------------------------------------------

// Echoes an incoming X-Request-ID, or generates a new one when absent, and
// sets it on the response so clients/log pipelines can correlate the request.
void request_id(const HttpRequest& req, HttpResponse& res,
                MiddlewareChain& chain) noexcept;

} // namespace uchttp::app
