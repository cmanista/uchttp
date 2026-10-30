#pragma once

// uchttp_security.h — optional security middleware (requirement #4).
//
// Everything here is non-template and lives in uchttp_security.cpp; the header
// only declares the public surface. These helpers plug into the engine as
// ordinary MiddlewareFn function pointers (no std::function, no heap).
//
//   server.use(uchttp::security::api_key_auth);
//   server.use(uchttp::security::security_headers);
//
// Configuration (keys, CORS origin, CSP policy) is set once at startup via the
// setters below and read by the middleware at request time.

#include <cstddef>
#include <string_view>

#include "uchttp.h"

namespace uchttp::security {

// ---------------------------------------------------------------------------
// Constant-time comparison
// ---------------------------------------------------------------------------

// Compares two byte ranges without early-out, so the running time does not leak
// how many leading bytes matched. Returns true iff equal. Use for secrets
// (API keys, tokens, HMACs) to defeat timing side-channels.
bool secure_compare(std::string_view a, std::string_view b) noexcept;

// ---------------------------------------------------------------------------
// Authentication configuration
// ---------------------------------------------------------------------------

// API key expected in the "X-API-Key" request header. Empty key => the
// api_key_auth middleware rejects every request (fail closed).
void set_api_key(std::string_view key) noexcept;

// Bearer token expected in "Authorization: Bearer <token>". Empty => reject.
void set_bearer_token(std::string_view token) noexcept;

// ---------------------------------------------------------------------------
// Auth middleware (MiddlewareFn-compatible)
// ---------------------------------------------------------------------------

// Rejects with 401 Unauthorized (does NOT call chain.next) unless the request
// carries the configured X-API-Key, compared in constant time.
void api_key_auth(const HttpRequest& req, HttpResponse& res,
                  MiddlewareChain& chain) noexcept;

// Rejects with 401 Unauthorized unless "Authorization: Bearer <token>" matches
// the configured token in constant time.
void bearer_auth(const HttpRequest& req, HttpResponse& res,
                 MiddlewareChain& chain) noexcept;

// ---------------------------------------------------------------------------
// Security-headers configuration + middleware
// ---------------------------------------------------------------------------

// Override the defaults injected by security_headers. Pass {} to disable a
// particular header. Defaults:
//   CORS origin : "*"
//   frame opts  : "DENY"
//   CSP         : "default-src 'self'"
//   nosniff     : always "nosniff"
void set_cors_origin(std::string_view origin) noexcept;
void set_frame_options(std::string_view v)   noexcept;
void set_csp(std::string_view policy)         noexcept;

// Injects CORS + hardening headers into every response (runs after the handler
// via the onion model): Access-Control-Allow-Origin, X-Content-Type-Options,
// X-Frame-Options and Content-Security-Policy.
void security_headers(const HttpRequest& req, HttpResponse& res,
                      MiddlewareChain& chain) noexcept;

} // namespace uchttp::security
