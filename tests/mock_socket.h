// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Cezary Manista

#pragma once

// mock_socket.h – in-memory socket for unit tests.
//
// RxData  – raw bytes pushed into read()
// TxData  – collects everything written via write()
//
// Usage:
//   std::string request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
//   MockSocket sock(request);
//   ... process ...
//   EXPECT_EQ(sock.tx_as_string().substr(0,15), "HTTP/1.1 200 OK");

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <cstring>
#include <algorithm>

namespace uchttp_test {

class MockSocket {
public:
    explicit MockSocket(std::string_view rx_data = {})
        : rx_(rx_data), rx_pos_(0) {}

    // SocketConcept interface --------------------------------------------------

    int read(std::span<uint8_t> buf) noexcept {
        if (rx_pos_ >= rx_.size()) return 0;  // EOF
        size_t avail = rx_.size() - rx_pos_;
        size_t n     = std::min(avail, buf.size());
        std::memcpy(buf.data(), rx_.data() + rx_pos_, n);
        rx_pos_ += n;
        return static_cast<int>(n);
    }

    int write(std::span<const uint8_t> buf) noexcept {
        tx_.append(reinterpret_cast<const char*>(buf.data()), buf.size());
        return static_cast<int>(buf.size());
    }

    void close() noexcept { closed_ = true; }

    // Test helpers -------------------------------------------------------------

    bool        is_valid()     const noexcept { return true; }
    bool        was_closed()   const noexcept { return closed_; }
    std::string_view tx_view() const noexcept { return tx_; }
    const std::string& tx_string() const noexcept { return tx_; }

    // Returns just the status line (first line of the response)
    std::string_view tx_status_line() const noexcept {
        auto pos = tx_.find("\r\n");
        if (pos == std::string::npos) return tx_;
        return std::string_view(tx_).substr(0, pos);
    }

    // Returns the response body (everything after the double CRLF)
    std::string_view tx_body() const noexcept {
        auto pos = tx_.find("\r\n\r\n");
        if (pos == std::string::npos) return {};
        return std::string_view(tx_).substr(pos + 4);
    }

    void reset_tx() noexcept { tx_.clear(); }

    void push_rx(std::string_view more) { rx_ += more; }

private:
    std::string rx_;
    size_t      rx_pos_ { 0 };
    std::string tx_;
    bool        closed_ { false };
};

// ---------------------------------------------------------------------------
// MockAcceptor – feeds a single MockSocket into the server run loop,
// then signals stop.
// ---------------------------------------------------------------------------

class MockAcceptor {
public:
    explicit MockAcceptor(std::string_view rx_data)
        : rx_data_(rx_data) {}

    MockSocket accept() noexcept {
        accepted_ = true;
        return MockSocket(rx_data_);
    }

    bool is_running() noexcept {
        // Run exactly once: return true before the first accept, false after
        if (!accepted_) return true;
        return false;
    }

    bool was_accepted() const noexcept { return accepted_; }

private:
    std::string rx_data_;
    bool        accepted_ { false };
};

} // namespace uchttp_test
