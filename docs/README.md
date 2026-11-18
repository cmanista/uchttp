<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# uchttp documentation

[← Project README](../README.md)

## Architecture

How the server is built: layers, request lifecycle, memory and concurrency model,
compile-time configuration, source layout.

- [Architecture overview](architecture/README.md)
- [Class diagrams](architecture/class_diagrams/README.md) (core types, engine,
  middleware, WebSocket, TLS, platform ports)

## Usage

Examples and reference for building an application.

| Page | Topics |
|---|---|
| [Overview and a minimal server](usage/README.md) | Index of the pages below |
| [Routing and request parsing](usage/routing.md) | Path parameters, query string, HTTP methods, handlers |
| [Middleware](usage/middleware.md) | Chain of Responsibility, Cache-Control, ETag, X-Request-ID |
| [Protocol headers](usage/protocol.md) | Engine-owned headers, keep-alive, error responses |
| [Security](usage/security.md) | DoS limits, authentication, security headers, TLS |
| [IoT edge capabilities](usage/iot.md) | Streaming / OTA upload, Server-Sent Events |
| [WebSocket](usage/websocket.md) | RFC 6455 endpoints and platform hooks |
| [Reliability](usage/reliability.md) | Connection pool, timeouts, shutdown |
| [Porting](usage/porting.md) | RTOS and bare-metal targets |
| [Zephyr](usage/zephyr.md) | Module, Kconfig, west, HTTPS, WebSocket |

## Testing

- [Testing](testing.md): running the suite, test doubles, what each suite
  covers, writing a test, manual checks, CI.

## Project

- [Releasing](RELEASING.md): versioning, changelog, release workflow.
- [llhttp provenance](../third_party/llhttp/UPSTREAM.md): vendored parser version
  and how to update it.
- [Changelog](../CHANGELOG.md)

## Versioning

uchttp follows [Semantic Versioning](https://semver.org/). While in the `0.x`
series the public API is considered unstable. Releases are tagged `vX.Y.Z` and
published on the [Releases page](https://github.com/cmanista/uchttp/releases).
See [RELEASING.md](RELEASING.md) for the process and the
[changelog](../CHANGELOG.md) for the history.

## License

uchttp is licensed under the [MIT License](../LICENSE), Copyright (c) 2026 Cezary
Manista. Every source file carries an `SPDX-License-Identifier: MIT` header.

### Third-party

| Component | Location | License |
|---|---|---|
| [llhttp](https://github.com/nodejs/llhttp) v9.4.3 | `third_party/llhttp/` | MIT (see `third_party/llhttp/LICENSE-MIT`) |

TLS backends (OpenSSL, wolfSSL) and the Zephyr RTOS are external dependencies
linked at build time under their own respective licenses. They are not
distributed as part of this repository.
