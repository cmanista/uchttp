# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

While the version is `0.x`, the public API is considered unstable and may change
in a minor release.

## [Unreleased]

<!-- Add changes here as they land on master. Move them under a new version
     heading when cutting a release (see docs/RELEASING.md). -->

### Added

### Changed

### Fixed

## [0.1.0] - 2026-11-10

Initial public release.

### Added
- Header/zero-copy HTTP/1.1 server core (`HttpServer`) with static routing,
  path parameters, query-string parsing, and full method support
  (GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS).
- CRTP socket/acceptor abstraction (no virtuals); llhttp v9.4.3 parser.
- Middleware chain (Chain of Responsibility) with bundled application and
  security middleware (Cache-Control, ETag, X-Request-ID, API-Key/Bearer auth,
  security headers, `secure_compare`).
- Protocol-header handling in the engine (Host, Content-Length,
  Transfer-Encoding: chunked, Connection/keep-alive) and protocol error
  responses (400/405/501/404).
- DoS/Slowloris protection: header/body size caps and idle read timeout.
- TLS/HTTPS via pluggable `TlsSessionConcept` with OpenSSL (host) and wolfSSL
  (embedded) backends.
- Connection pool, graceful shutdown, streaming body / OTA upload, and
  Server-Sent Events (SSE).
- WebSocket (RFC 6455) endpoints with pluggable SHA-1 and allocator.
- Linux port (`LinuxSocket`/`LinuxAcceptor`) and Zephyr module
  (HTTP, HTTPS/TLS, WebSocket) with samples.
- Generated `uchttp_version.h` exposing `UCHTTP_VERSION_*` macros.
- 215 unit tests (GTest) covering the HTTP engine and WebSocket.

[Unreleased]: https://github.com/cmanista/uchttp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/cmanista/uchttp/releases/tag/v0.1.0
