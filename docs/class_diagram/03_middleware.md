# Middleware

Źródło: `include/uchttp.h`, `include/uchttp_security.h`, `include/uchttp_app_middleware.h`.
[← indeks](../../class_diagram.md)

```mermaid
classDiagram
    class MiddlewareFn {
        <<function pointer>>
        void(const HttpRequest&, HttpResponse&, MiddlewareChain&)
    }

    class MiddlewareChain {
        +MiddlewareChain(mws, count, handler)
        +next(req, res) void
        +run(req, res) void
        -mws_ : const MiddlewareFn*
        -count_ : size_t
        -handler_ : HandlerFn
        -index_ : size_t
        -handler_called_ : bool
    }

    class HandlerFn {
        <<function pointer>>
        void(const HttpRequest&, HttpResponse&)
    }

    class security {
        <<namespace uchttp::security>>
        +secure_compare(a, b) bool
        +set_api_key(key) void
        +set_bearer_token(token) void
        +api_key_auth(req, res, chain) void
        +bearer_auth(req, res, chain) void
        +set_cors_origin(origin) void
        +set_frame_options(v) void
        +set_csp(policy) void
        +security_headers(req, res, chain) void
    }

    class app {
        <<namespace uchttp::app>>
        +set_cache_control(value) void
        +cache_control(req, res, chain) void
        +etag(req, res, chain) void
        +compute_etag(body, out, cap) size_t
        +client_accepts_encoding(req, coding) bool
        +request_id(req, res, chain) void
    }

    class HttpServer {
        <<see 02_engine>>
        +use(mw) bool
    }

    HttpServer o-- MiddlewareFn : "0..MaxMiddleware (use)"
    MiddlewareChain o-- MiddlewareFn : iterates
    MiddlewareChain o-- HandlerFn : final target
    security ..> MiddlewareFn : provides api_key_auth / bearer_auth / security_headers
    app ..> MiddlewareFn : provides cache_control / etag / request_id
    MiddlewareFn ..> MiddlewareChain : next()
```

## Uwagi

- Model "cebuli": kod po `chain.next(req, res)` wykonuje się w drodze powrotnej (nagłówki bezpieczeństwa, ETag, czas, logi). Middleware niewywołujące `next()` przerywa łańcuch (np. `401`).
- Middleware wykonują się w kolejności rejestracji: `A → B → handler → B → A`. Tablica ma stały rozmiar `UCHTTP_MAX_MIDDLEWARE` (domyślnie 8).
- Żądanie WebSocket upgrade również przechodzi przez łańcuch (auth, security headers); końcowym "handlerem" jest `ws::upgrade_handler<Sha1>`.
- `uchttp::security`: `api_key_auth` i `bearer_auth` działają *fail closed* (brak konfiguracji = odrzucenie), porównanie przez `secure_compare` (stały czas).
- `uchttp::app::etag`: FNV-1a z body; zgodny `If-None-Match` → `304` z pustym body.
- Nagłówki własne silnika (Host, Content-Length, Transfer-Encoding, Connection) nie należą do middleware.
