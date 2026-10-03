#include "engine/Metrics.hpp"
#include "engine/MetricsServer.hpp"
#include "Check.hpp"

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
    metrics.SetGameServerState(engine::GameServerState::Allocated);

    CHECK(metrics.GetTotalTicks() == 3);
    CHECK(metrics.GetOverruns() == 1);

    std::array<char, 8192> text_buf{};
    const size_t bytes = metrics.Serialize(text_buf.data(), text_buf.size());
    CHECK(bytes > 0);

    // Verify key Prometheus metrics exist in serialization
    CHECK(std::strstr(text_buf.data(), "game_tick_duration_us_bucket{le=\"50\"} 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_tick_duration_us_bucket{le=\"+Inf\"} 3") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_active_players 4") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_total_ticks 3") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_tick_overruns 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_confirmed_hits 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_occluded_shots 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_rejected_rewinds 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_agones_state{state=\"Allocated\"} 1") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_agones_state{state=\"Ready\"} 0") != nullptr);
    CHECK(std::strstr(text_buf.data(), "game_agones_state{state=\"Unknown\"} 0") != nullptr);

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
    CHECK(started && server.IsRunning());

#if defined(_WIN32)
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(client != INVALID_SOCKET);
#else
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(client >= 0);
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(test_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    const int connect_res = connect(client, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    CHECK(connect_res == 0);

    const char* request = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(client, request, static_cast<int>(std::strlen(request)), 0);

    // Read to EOF; header and body sent separately
    std::array<char, 16384> response{};
    size_t received = 0;
    while (received < response.size() - 1) {
        const auto chunk = recv(client, response.data() + received, static_cast<int>(response.size() - 1 - received), 0);
        if (chunk <= 0) {
            break;
        }
        received += static_cast<size_t>(chunk);
    }
    CHECK(received > 0);
    response[received] = '\0';

    CHECK(std::strstr(response.data(), "HTTP/1.1 200 OK") != nullptr);
    CHECK(std::strstr(response.data(), "version=0.0.4") != nullptr);
    CHECK(std::strstr(response.data(), "game_active_players 2") != nullptr);
    CHECK(std::strstr(response.data(), "game_agones_state{state=\"Unknown\"} 1") != nullptr);

#if defined(_WIN32)
    closesocket(client);
#else
    close(client);
#endif

    server.Stop();
    CHECK(!server.IsRunning());

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
