#include "engine/Metrics.hpp"
#include "engine/MetricsServer.hpp"

#include <cassert>
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace {

void TestAtomicMetricsCollection() {
    std::printf("[Test] Running Atomic Prometheus Metrics Collection...\n");
    engine::Metrics metrics;

    // Record various ticks
    metrics.RecordTick(25, 2, false);   // Bucket <= 50
    metrics.RecordTick(120, -1, false); // Bucket <= 250
    metrics.RecordTick(8000, 5, true);  // Bucket <= 10000 + Overrun

    metrics.RecordCombat(true, true, false, false); // Confirmed hit
    metrics.RecordCombat(true, false, true, false); // Occluded shot
    metrics.RecordCombat(false, false, false, true); // Rejected rewind
    metrics.SetActivePlayers(4);

    assert(metrics.GetTotalTicks() == 3);
    assert(metrics.GetOverruns() == 1);

    std::array<char, 4096> text_buf{};
    const size_t bytes = metrics.Serialize(text_buf.data(), text_buf.size());
    assert(bytes > 0);
    static_cast<void>(bytes);

    // Verify key Prometheus metrics exist in serialization
    assert(std::strstr(text_buf.data(), "game_tick_duration_us_bucket{le=\"50\"} 1") != nullptr);
    assert(std::strstr(text_buf.data(), "game_tick_duration_us_bucket{le=\"+Inf\"} 3") != nullptr);
    assert(std::strstr(text_buf.data(), "game_active_players 4") != nullptr);
    assert(std::strstr(text_buf.data(), "game_total_ticks 3") != nullptr);
    assert(std::strstr(text_buf.data(), "game_tick_overruns 1") != nullptr);
    assert(std::strstr(text_buf.data(), "game_confirmed_hits 1") != nullptr);
    assert(std::strstr(text_buf.data(), "game_occluded_shots 1") != nullptr);
    assert(std::strstr(text_buf.data(), "game_rejected_rewinds 1") != nullptr);

    std::printf("  -> Atomic metrics collection and Prometheus text format verified.\n");
}

void TestHttpExporterEndpoint() {
    std::printf("[Test] Running HTTP Prometheus Exporter (/metrics, /healthz)...\n");
    engine::Metrics metrics;
    metrics.RecordTick(45, 1, false);
    metrics.SetActivePlayers(2);

    engine::MetricsServer server;
    const uint16_t test_port = 19090;
    const bool started = server.Start(test_port, metrics);
    assert(started && server.IsRunning());
    static_cast<void>(started);

#if defined(_WIN32)
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client != INVALID_SOCKET);
#else
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client >= 0);
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(test_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    const int connect_res = connect(client, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    assert(connect_res == 0);
    static_cast<void>(connect_res);

    const char* request = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(client, request, static_cast<int>(std::strlen(request)), 0);

    std::array<char, 4096> response{};
    const int read_bytes = recv(client, response.data(), static_cast<int>(response.size() - 1), 0);
    assert(read_bytes > 0);
    response[static_cast<size_t>(read_bytes > 0 ? read_bytes : 0)] = '\0';

    assert(std::strstr(response.data(), "HTTP/1.1 200 OK") != nullptr);
    assert(std::strstr(response.data(), "version=0.0.4") != nullptr);
    assert(std::strstr(response.data(), "game_active_players 2") != nullptr);

#if defined(_WIN32)
    closesocket(client);
#else
    close(client);
#endif

    server.Stop();
    assert(!server.IsRunning());

    std::printf("  -> HTTP exporter endpoint (/metrics) responded with 200 OK.\n");
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf(" Agones-128 Engine: Telemetry Test Verification\n");
    std::printf("====================================================\n");

    TestAtomicMetricsCollection();
    TestHttpExporterEndpoint();

    std::printf("\nAll Telemetry Verification Checks PASSED (100%%).\n");
    return 0;
}
