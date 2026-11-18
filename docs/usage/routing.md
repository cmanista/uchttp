<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Routing and request parsing

[← Usage index](README.md)

## Path parameters

Register dynamic segments with `{name}` and read them zero-copy from the request.
No heap allocation: the value is a `string_view` into the RX buffer.

```cpp
server.get("/api/sensors/{id}", [](const HttpRequest& req, HttpResponse& res) {
    std::string_view id = req.path_param("id");   // e.g. "42"
    // ... look up sensor by id ...
    res.set_status(200);
});
```

Rules:

- Literal routes always win over patterns, so `/api/sensors/all` can be
  registered next to `/api/sensors/{id}`.
- A `{name}` segment matches exactly one non-empty path segment.
- A route holds at most `UCHTTP_MAX_PATH_PARAMS` parameters (default 8).
- `req.path_param("missing")` returns an empty `string_view`.

## Query string

The query string (everything after `?`) is parsed lazily by an allocation-free
iterator:

```cpp
server.get("/api/search", [](const HttpRequest& req, HttpResponse& res) {
    std::string_view q     = req.query_param("q");      // ?q=temp
    std::string_view limit = req.query_param("limit");  // &limit=5

    // Or iterate over every pair:
    for (auto [key, value] : req.query()) { /* ... */ }

    res.set_status(200);
});
```

- `req.path` always contains the path only. The query string is kept separately
  in `req.raw_query`.
- `req.query()` returns a `QueryString` with `begin()` / `end()`, `get(key)`,
  `has(key)`, `empty()` and `raw()`. `has("flag")` is true for `?flag` (a key
  with no value).
- Values are returned as they appear on the wire. There is no percent-decoding.

## HTTP methods

`GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD` and `OPTIONS` are supported.
`OPTIONS` is needed for CORS preflight:

```cpp
server.get    ("/api/data", handler);
server.post   ("/api/data", handler);
server.put    ("/api/data", handler);
server.del    ("/api/data", handler);
server.patch  ("/api/data", handler);
server.head   ("/api/data", handler);
server.options("/api/data", handler);  // CORS preflight
```

A request to a known path with an unregistered method returns
`405 Method Not Allowed`. An unknown path returns `404 Not Found`. A method that
uchttp does not support (for example `TRACE` or `CONNECT`) returns
`501 Not Implemented`.

Each registration function returns `false` when the route table
(`UCHTTP_MAX_ROUTES`, default 16) is full.

## Handlers

A handler is a plain function pointer. Lambdas without captures convert to it.

```cpp
void handler(const HttpRequest& req, HttpResponse& res);
```

Reading the request:

| Member | Meaning |
|---|---|
| `req.method` | `HttpMethod` enum |
| `req.path`, `req.raw_query` | Path without query, and the part after `?` |
| `req.body` | Body as a view into the RX buffer |
| `req.get_header(name)` | Value of a request header, or empty |
| `req.host()` | `Host` header |
| `req.content_length`, `req.chunked`, `req.keep_alive` | Protocol metadata filled by the parser |

`get_header` compares names exactly, including letter case. Pass the spelling the
client uses (for example `"X-API-Key"`). `ws::find_header(req, name)` is the
case-insensitive variant.

Building the response:

| Call | Effect |
|---|---|
| `res.set_status(code)` | Status code. The reason phrase is filled in for common codes. |
| `res.set_content_type(ct)` | `Content-Type` |
| `res.set_content_encoding(enc)` | `Content-Encoding` |
| `res.set_body(sv)` | Body (not copied) |
| `res.add_header(name, value)` | Extra header (not copied, up to `UCHTTP_MAX_HEADERS`) |

`set_body` and `add_header` keep views, not copies. The data must stay valid
until the response is sent: use string literals, static storage, or
`res.scratch()` (32 bytes) for short values generated at run time. Request views
such as `req.body` stay valid for the duration of the handler.
