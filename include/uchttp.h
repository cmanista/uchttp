#include <cstddef>
#include <cstdint>
#include <span>
#include <concepts>
#include <string_view>
#include <flat_map> // C++23 lub własna/stała struktura na nagłówki
#include <functional>

template <typename Derived>
class ISocket {
public:
    int read(std::span<uint8_t> buffer) {
        return static_cast<Derived*>(this)->read_impl(buffer);
    }

    int write(std::span<const uint8_t> buffer) {
        return static_cast<Derived*>(this)->write_impl(buffer);
    }

    void close() {
        static_cast<Derived*>(this)->close_impl();
    }

protected:
    ISocket() = default;
};

template <typename T>
concept SocketConcept = requires(T socket, std::span<uint8_t> rx_buf, std::span<const uint8_t> tx_buf) {
    { socket.read(rx_buf) } -> std::same_as<int>;
    { socket.write(tx_buf) } -> std::same_as<int>;
    { socket.close() } -> std::same_as<void>;
};

// Klasa połączenia sparametryzowana typem gniazda
template <SocketConcept SocketImpl>
class HttpConnection {
public:
    explicit HttpConnection(SocketImpl socket) : socket_(socket) {}

    void process() {
        uint8_t buffer[256];
        int bytes = socket_.read(buffer); // Wywołanie bezpośrednie, w pełni inlinowane!
        if (bytes > 0) {
            // Logika parsowania llhttp...
        }
    }

private:
    SocketImpl socket_;
};


enum class HttpMethod { GET, POST, PUT, DELETE, UNKNOWN };

struct HttpRequest {
    HttpMethod method{HttpMethod::UNKNOWN};
    std::string_view path;
    std::string_view body;
    // TODO: dodać kontener na nagłówki
};

class HttpResponse {
public:
    void set_status(int code) { status_code_ = code; }
    void set_header(std::string_view key, std::string_view value);
    void set_body(std::string_view body) { body_ = body; }

    bool send(ISocket& socket);

private:
    int status_code_{200};
    std::string_view body_;
};


using HttpHandler = std::move_only_function<void(const HttpRequest&, HttpResponse&)>;

struct Route {
    HttpMethod method;
    std::string_view path;
    HttpHandler handler;
};

class HttpServer {
public:
    explicit HttpServer(size_t max_connections = 4);

    void add_route(HttpMethod method, std::string_view path, HttpHandler handler);

    // Główna pętla lub funkcja przetwarzająca (wywoływana w wątku RTOS)
    void process();

private:
    // Pula połączeń i buforów odbiorczych
    // Parser llhttp zintegrowany z buforem połączenia
};
