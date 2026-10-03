#include "engine/MetricsServer.hpp"
#include <cstdio>
#include <cstring>
#include <array>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace engine {

namespace {

constexpr int kClientIoTimeoutMs = 1000;

#if defined(_WIN32)
using SocketType = SOCKET;
using IoResult = int;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
inline void CloseSocket(SocketType s) { closesocket(s); }
inline IoResult SocketSend(SocketType s, const char* data, size_t len) { return send(s, data, static_cast<int>(len), 0); }
inline IoResult SocketRecv(SocketType s, char* data, size_t len) { return recv(s, data, static_cast<int>(len), 0); }

inline void SetClientTimeouts(SocketType s) {
    const DWORD timeout = kClientIoTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
#else
using SocketType = int;
using IoResult = ssize_t;
constexpr SocketType kInvalidSocket = -1;
inline void CloseSocket(SocketType s) { close(s); }
// No SIGPIPE on scraper disconnect
inline IoResult SocketSend(SocketType s, const char* data, size_t len) { return send(s, data, len, MSG_NOSIGNAL); }
inline IoResult SocketRecv(SocketType s, char* data, size_t len) { return recv(s, data, len, 0); }

inline void SetClientTimeouts(SocketType s) {
    const timeval timeout{kClientIoTimeoutMs / 1000, (kClientIoTimeoutMs % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
#endif

bool SendAll(SocketType s, const char* data, size_t len) {
    while (len > 0) {
        const IoResult sent = SocketSend(s, data, len);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

} // namespace

MetricsServer::MetricsServer() noexcept = default;

MetricsServer::~MetricsServer() {
    Stop();
}

bool MetricsServer::Start(uint16_t port, const Metrics& metrics) noexcept {
    if (is_running_.load(std::memory_order_relaxed)) {
        return false;
    }

    metrics_ = &metrics;
    port_ = port;

#if defined(_WIN32)
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
#endif

    const SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) {
        return false;
    }

    // Enable address reuse for immediate binding after restarts
    int opt = 1;
#if defined(_WIN32)
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocket(sock);
        return false;
    }

    if (listen(sock, 16) != 0) {
        CloseSocket(sock);
        return false;
    }

    listen_socket_ = static_cast<decltype(listen_socket_)>(sock);
    is_running_.store(true, std::memory_order_release);

    worker_thread_ = std::thread(&MetricsServer::RunServer, this);
    return true;
}

void MetricsServer::Stop() noexcept {
    if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (listen_socket_ != static_cast<decltype(listen_socket_)>(kInvalidSocket)) {
        CloseSocket(static_cast<SocketType>(listen_socket_));
        listen_socket_ = static_cast<decltype(listen_socket_)>(kInvalidSocket);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

#if defined(_WIN32)
    WSACleanup();
#endif
}

void MetricsServer::RunServer() noexcept {
    const auto sock = static_cast<SocketType>(listen_socket_);
    std::array<char, 2048> request_buf{};
    std::array<char, 16384> response_body{};
    std::array<char, 512> header_buf{};

    while (is_running_.load(std::memory_order_relaxed)) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);

        // 200ms select timeout enables prompt thread termination
        timeval tv{0, 200000};
        const int sel = select(static_cast<int>(sock + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            continue;
        }

        sockaddr_in client_addr{};
#if defined(_WIN32)
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        const SocketType client_sock = accept(sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == kInvalidSocket) {
            continue;
        }

        // Bound client I/O against stalled scrapers
        SetClientTimeouts(client_sock);

        const IoResult bytes_read = SocketRecv(client_sock, request_buf.data(), request_buf.size() - 1);
        if (bytes_read > 0) {
            request_buf[static_cast<size_t>(bytes_read)] = '\0';

            if (std::strncmp(request_buf.data(), "GET /metrics", 12) == 0 && metrics_) {
                const size_t body_len = metrics_->Serialize(response_body.data(), response_body.size());

                const int header_len = std::snprintf(
                    header_buf.data(), header_buf.size(),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n",
                    body_len);

                if (header_len > 0 && SendAll(client_sock, header_buf.data(), static_cast<size_t>(header_len))) {
                    SendAll(client_sock, response_body.data(), body_len);
                }
            } else if (std::strncmp(request_buf.data(), "GET /healthz", 12) == 0 ||
                       std::strncmp(request_buf.data(), "GET / ", 6) == 0) {
                const char* kHealthResponse =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 2\r\n"
                    "Connection: close\r\n\r\nOK";
                SendAll(client_sock, kHealthResponse, std::strlen(kHealthResponse));
            } else {
                const char* kNotFound =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                SendAll(client_sock, kNotFound, std::strlen(kNotFound));
            }
        }

        CloseSocket(client_sock);
    }
}

} // namespace engine
