# uchttp — Microcontroller HTTP Server

[![CI](https://github.com/cmanista/uchttp/actions/workflows/ci.yml/badge.svg)](https://github.com/cmanista/uchttp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/cmanista/uchttp?include_prereleases&sort=semver)](https://github.com/cmanista/uchttp/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](https://en.cppreference.com/w/cpp/23)

A lightweight C++23 HTTP/1.1 server for microcontrollers. Ports for Zephyr and
Linux are included, and any other RTOS or bare-metal target needs only a socket
and an acceptor class. Zero heap allocations, zero-copy parsing, concept-based
socket abstraction.

## Key properties

| Property | Detail |
|---|---|
| **Zero heap** | All buffers, tables and connection slots are stack or statically allocated; no `new` / `malloc` |
| **Zero-copy parsing** | `HttpRequest` fields are `string_view`s into the RX buffer |
| **Concepts, no virtuals** | Socket, acceptor, TLS session, SHA-1 and allocator are template parameters checked by C++ concepts: fully inlined, no vtable |
| **Protocol** | HTTP/1.0 and HTTP/1.1, keep-alive, chunked bodies; GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS |
| **llhttp** | Battle-tested incremental parser from Node.js (v9.4.3, vendored) |
| **Static routing** | `RouteTable<N>` is a fixed array sized at compile time; `{name}` path parameters, query-string iterator, literal routes win over patterns |
| **Handler signature** | `void(const HttpRequest&, HttpResponse&)`: a plain function pointer |
| **Middleware** | Chain of Responsibility; `void(req, res, MiddlewareChain&)` function pointers in a fixed array |
| **Bundled middleware** | API key and bearer auth, security headers (CORS, CSP, ...), Cache-Control, ETag, X-Request-ID |
| **DoS protection** | Header and body size limits, idle read timeout (Slowloris) |
| **TLS** | Pluggable: OpenSSL (host), wolfSSL (embedded), native Mbed TLS / PSA Crypto on Zephyr; certificates or PSK |
| **IoT edge** | Streaming body for OTA upload straight to flash without buffering it in RAM, Server-Sent Events |
| **WebSocket** | RFC 6455 endpoints as routes; callback API; pluggable SHA-1 and session-buffer allocator |
| **Reliability** | Fixed connection pool, keep-alive and idle timeouts, graceful shutdown |
| **Configurable** | Every limit is a compile-time macro; Kconfig symbols on Zephyr |
| **Portability** | The core (`include/uchttp*.h`, `src/uchttp*.cpp`) has no OS or POSIX dependencies; ports for Linux and Zephyr (module with Kconfig, tested on `native_sim` and NXP FRDM-RW612), a short contract for other targets |
| **Tested** | 222 unit tests run on the host against in-memory sockets; CI builds with GCC and Clang |

## Quick start (Linux)

Requires CMake 3.20+ and a C++23 compiler (CI builds with GCC and Clang).
OpenSSL is optional and enables the HTTPS sample.

```bash
cmake -B build && cmake --build build
./build/uchttp_server          # listens on http://localhost:8080

curl http://localhost:8080/api/hello
curl -X POST http://localhost:8080/api/echo -d 'hello'
curl -X PUT  http://localhost:8080/api/item -d '{"id":1}'
curl http://localhost:8080/api/sensors/42              # path parameter
curl 'http://localhost:8080/api/search?q=temp&limit=5' # query string
curl -X DELETE http://localhost:8080/api/sensors/42    # DELETE method
curl -X OPTIONS http://localhost:8080/api/data -i      # CORS preflight
```

## Documentation

Architecture, usage guides, testing and the release process are in
**[docs/](docs/README.md)**.

## AI-assisted development

This project was developed with the help of an AI coding tool
([Kiro](https://kiro.dev)). The code, tests and documentation were written with
AI assistance under my direction.

I started this project to test the productivity of the tool. The results were so
good that I decided to take it all the way to production-ready code.

Key takeaways:

- I could prototype many proofs of concept quickly and pick the best solution.
- Documenting used to be tedious. It is now easy, so I can afford to maintain
  comprehensive documentation.
- Porting to a given hardware platform or OS is fast, so I can test everything on
  dev boards and on `native_sim`.

Released under the [MIT License](LICENSE).
