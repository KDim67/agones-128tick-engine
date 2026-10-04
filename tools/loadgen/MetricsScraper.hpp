#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace loadgen {

// One Prometheus text exposition, keyed by full series name including labels
struct MetricsSample {
    bool ok{false};
    std::chrono::steady_clock::time_point taken_at{};
    std::unordered_map<std::string, double> series;

    [[nodiscard]] double Value(const std::string& key) const noexcept {
        const auto it = series.find(key);
        return it == series.end() ? 0.0 : it->second;
    }
};

[[nodiscard]] MetricsSample ScrapeMetrics(const std::string& host, uint16_t port, std::chrono::milliseconds timeout);

// Upper bound of the bucket containing quantile q over the counts accrued between two samples; -1 means past the last finite bucket
[[nodiscard]] double HistogramQuantileUpperBound(const MetricsSample& before, const MetricsSample& after,
                                                 std::string_view histogram, double quantile);

} // namespace loadgen
