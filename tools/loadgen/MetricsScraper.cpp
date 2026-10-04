#include "MetricsScraper.hpp"
#include "engine/Metrics.hpp"

#include <charconv>
#include <cstdlib>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace loadgen {

namespace {

#if defined(_WIN32)
using SocketType = SOCKET;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketType s) { closesocket(s); }
int ConnectAddress(SocketType s, const addrinfo* addr) { return connect(s, addr->ai_addr, static_cast<int>(addr->ai_addrlen)); }
long long SendBytes(SocketType s, const char* data, size_t len) { return send(s, data, static_cast<int>(len), 0); }
long long RecvBytes(SocketType s, char* data, size_t len) { return recv(s, data, static_cast<int>(len), 0); }
void SetTimeout(SocketType s, std::chrono::milliseconds timeout) {
    const auto ms = static_cast<DWORD>(timeout.count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}
#else
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
void CloseSocket(SocketType s) { close(s); }
int ConnectAddress(SocketType s, const addrinfo* addr) { return connect(s, addr->ai_addr, addr->ai_addrlen); }
long long SendBytes(SocketType s, const char* data, size_t len) { return send(s, data, len, MSG_NOSIGNAL); }
long long RecvBytes(SocketType s, char* data, size_t len) { return recv(s, data, len, 0); }
void SetTimeout(SocketType s, std::chrono::milliseconds timeout) {
    const auto ms = timeout.count();
    const timeval tv{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

SocketType Connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0 || !resolved) {
        return kInvalidSocket;
    }

    SocketType sock = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (sock != kInvalidSocket) {
        SetTimeout(sock, timeout);
        if (ConnectAddress(sock, resolved) != 0) {
            CloseSocket(sock);
            sock = kInvalidSocket;
        }
    }
    freeaddrinfo(resolved);
    return sock;
}

// Body length from the response headers, or npos when absent or unparsable
size_t ContentLength(std::string_view headers) {
    constexpr std::string_view kField = "\r\nContent-Length:";
    const size_t at = headers.find(kField);
    if (at == std::string_view::npos) {
        return std::string_view::npos;
    }
    std::string_view value = headers.substr(at + kField.size());
    value = value.substr(0, value.find("\r\n"));
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    size_t length = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
    return (ec == std::errc{} && end == value.data() + value.size()) ? length : std::string_view::npos;
}

void ParseExposition(std::string_view body, MetricsSample& sample) {
    while (!body.empty()) {
        const size_t eol = body.find('\n');
        std::string_view line = body.substr(0, eol);
        body = (eol == std::string_view::npos) ? std::string_view{} : body.substr(eol + 1);

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const size_t split = line.rfind(' ');
        if (split == std::string_view::npos) {
            continue;
        }
        const std::string value(line.substr(split + 1));
        sample.series[std::string(line.substr(0, split))] = std::strtod(value.c_str(), nullptr);
    }
}

} // namespace

MetricsSample ScrapeMetrics(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    MetricsSample sample;
    sample.taken_at = std::chrono::steady_clock::now();

    const SocketType sock = Connect(host, port, timeout);
    if (sock == kInvalidSocket) {
        return sample;
    }

    const std::string request = "GET /metrics HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (SendBytes(sock, request.data(), request.size()) < 0) {
        CloseSocket(sock);
        return sample;
    }

    std::string response;
    char chunk[4096];
    for (;;) {
        const long long received = RecvBytes(sock, chunk, sizeof(chunk));
        if (received <= 0) {
            break;
        }
        response.append(chunk, static_cast<size_t>(received));
    }
    CloseSocket(sock);

    const size_t body_start = response.find("\r\n\r\n");
    if (response.rfind("HTTP/1.1 200", 0) != 0 || body_start == std::string::npos) {
        return sample;
    }

    // A recv timeout mid-body looks like EOF; only a complete body with the core series counts
    const std::string_view view(response);
    const size_t declared = ContentLength(view.substr(0, body_start));
    const std::string_view body = view.substr(body_start + 4);
    if (declared == std::string_view::npos || body.size() != declared) {
        return sample;
    }

    ParseExposition(body, sample);
    sample.ok = sample.series.count("game_uptime_seconds") != 0 && sample.series.count("game_total_ticks") != 0;
    return sample;
}

double HistogramQuantileUpperBound(const MetricsSample& before, const MetricsSample& after,
                                   std::string_view histogram, double quantile) {
    const std::string prefix = std::string(histogram) + "_bucket{le=\"";
    const double total = after.Value(prefix + "+Inf\"}") - before.Value(prefix + "+Inf\"}");
    if (total <= 0.0) {
        return 0.0;
    }

    const double rank = quantile * total;
    for (const int64_t bound : engine::kHistogramBuckets) {
        const std::string key = prefix + std::to_string(bound) + "\"}";
        if (after.Value(key) - before.Value(key) >= rank) {
            return static_cast<double>(bound);
        }
    }
    return -1.0;
}

} // namespace loadgen
