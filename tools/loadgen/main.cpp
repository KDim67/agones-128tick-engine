#include "BotClient.hpp"
#include "MetricsScraper.hpp"
#include "engine/Metrics.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using loadgen::Clock;

constexpr auto kScrapeTimeout = std::chrono::milliseconds(2000);
constexpr uint32_t kScrapeAttempts = 3;
constexpr int kExitUsage = 64;
constexpr int kExitInternal = 70;
constexpr int kExitChecksFailed = 1;
constexpr double kMetresToMm = 1000.0;

struct Endpoint {
    std::string host;
    uint16_t port{0};
};

struct Options {
    std::vector<Endpoint> servers;
    std::vector<Endpoint> metrics;
    uint32_t clients{0};
    uint32_t warmup_s{5};
    uint32_t duration_s{60};
    uint32_t drain_s{4};
    uint32_t redundancy{static_cast<uint32_t>(engine::kInputRedundancy)};
    uint32_t fire_interval_ms{500};
    uint32_t threads{0};
    uint64_t seed{1};
    loadgen::AimMode aim{loadgen::AimMode::Mixed};
    std::string label{"benchmark"};
    std::string report_path;
    double max_tick_p99_us{7812.0};
    double max_overrun_ratio{0.001};
    double min_tps{127.5};
    double max_lateness_p99_us{7812.0};
    double min_hit_confirmation{0.98};
    double max_hit_error_p99_mm{1.0};
    double max_input_loss{0.001};
};

enum class RunStage : int { Warmup, Measure, Drain, Stop };

constexpr auto kLateFrameThreshold = std::chrono::milliseconds(2);

// Harness wake-up lateness: stalls shared with the servers point at the host, not the engine
struct WorkerTiming {
    uint64_t frames{0};
    uint64_t late_frames{0};
    double max_late_ms{0.0};
};

struct ServerSummary {
    std::string endpoint;
    bool scraped{false};
    double window_s{0.0};
    double ticks{0.0};
    double tps{0.0};
    double tick_p50_us{0.0};
    double tick_p99_us{0.0};
    double tick_p999_us{0.0};
    double overruns{0.0};
    double overrun_ratio{0.0};
    double lateness_p99_us{0.0};
    double shots{0.0};
    double hits{0.0};
    double rejected_rewinds{0.0};
    double inputs_accepted{0.0};
    double inputs_missed{0.0};
    double inputs_redundant{0.0};
    double inputs_overflow{0.0};
    double malformed{0.0};
    double net_pool_failures{0.0};
    double net_pool_peak_bytes{0.0};
    double throttled_shots{0.0};
};

struct Totals {
    uint32_t connected{0};
    uint32_t dropped{0};
    double connect_ms_max{0.0};
    double rtt_ms_sum{0.0};
    uint32_t rtt_ms_max{0};
    double rtt_variance_ms_sum{0.0};
    uint64_t snapshots_received{0};
    uint64_t snapshots_reordered{0};
    uint64_t snapshot_ticks_missing{0};
    uint64_t reconcile_checks{0};
    uint64_t mispredictions{0};
    uint64_t shots_fired{0};
    uint64_t shots_confirmed{0};
    uint64_t shots_mismatched{0};
    uint64_t shots_unconfirmed{0};
    uint64_t shots_rejected{0};
    uint64_t shots_throttled{0};
    uint64_t shots_invalidated{0};
    uint64_t shots_gap_interpolated{0};
    uint64_t unexpected_results{0};
    uint64_t hits_taken{0};
    uint64_t deaths{0};
    double interp_delay_tick_sum{0.0};
    uint64_t interp_delay_samples{0};
    std::vector<float> misprediction_error_m;
    std::vector<float> hit_error_m;
};

struct Check {
    std::string name;
    bool passed{false};
    std::string detail;
};

template <typename T>
bool ParseNumber(std::string_view text, T& out) {
    const char* end = text.data() + text.size();
    const auto [parsed_end, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc{} && parsed_end == end;
}

bool ParseEndpoint(std::string_view text, Endpoint& out) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    uint16_t port = 0;
    if (!ParseNumber(text.substr(colon + 1), port) || port == 0) {
        return false;
    }
    out.host = std::string(text.substr(0, colon));
    out.port = port;
    return true;
}

void PrintUsage() {
    std::fprintf(stderr,
        "usage: load_generator --server HOST:PORT [--server ...] [--metrics HOST:PORT ...] [options]\n"
        "  --clients N              simulated players, at most %zu per server (default: fill every server)\n"
        "  --warmup S --duration S --drain S   window timing in seconds (default 5 / 60 / 4)\n"
        "  --redundancy N           inputs per packet, 1..%zu (default %zu)\n"
        "  --fire-interval MS       minimum time between shots per bot (default 500)\n"
        "  --aim head|torso|mixed   hitbox per engagement, mixed is 1 in 4 head (default mixed)\n"
        "  --threads N              worker threads (default: half the hardware threads)\n"
        "  --seed N                 behavior seed (default 1)\n"
        "  --label NAME --report FILE.json\n"
        "  --max-tick-p99-us X --max-overrun-ratio X --min-tps X --max-lateness-p99-us X\n"
        "  --min-hit-confirmation X --max-hit-error-p99-mm X --max-input-loss X\n",
        engine::kMaxPlayers, engine::kInputRedundancy, engine::kInputRedundancy);
}

bool ParseOptions(int argc, char* argv[], Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        const std::string_view value = has_value ? std::string_view(argv[i + 1]) : std::string_view{};
        bool ok = has_value;

        if (arg == "--server") {
            Endpoint endpoint;
            ok = ok && ParseEndpoint(value, endpoint);
            opts.servers.push_back(endpoint);
        } else if (arg == "--metrics") {
            Endpoint endpoint;
            ok = ok && ParseEndpoint(value, endpoint);
            opts.metrics.push_back(endpoint);
        } else if (arg == "--clients") {
            ok = ok && ParseNumber(value, opts.clients);
        } else if (arg == "--warmup") {
            ok = ok && ParseNumber(value, opts.warmup_s);
        } else if (arg == "--duration") {
            ok = ok && ParseNumber(value, opts.duration_s) && opts.duration_s > 0;
        } else if (arg == "--drain") {
            ok = ok && ParseNumber(value, opts.drain_s);
        } else if (arg == "--redundancy") {
            ok = ok && ParseNumber(value, opts.redundancy) && opts.redundancy >= 1 && opts.redundancy <= engine::kInputRedundancy;
        } else if (arg == "--fire-interval") {
            ok = ok && ParseNumber(value, opts.fire_interval_ms) && opts.fire_interval_ms > 0;
        } else if (arg == "--aim") {
            ok = ok && (value == "head" || value == "torso" || value == "mixed");
            opts.aim = value == "head" ? loadgen::AimMode::Head : (value == "torso" ? loadgen::AimMode::Torso : loadgen::AimMode::Mixed);
        } else if (arg == "--threads") {
            ok = ok && ParseNumber(value, opts.threads);
        } else if (arg == "--seed") {
            ok = ok && ParseNumber(value, opts.seed);
        } else if (arg == "--label") {
            opts.label = std::string(value);
        } else if (arg == "--report") {
            opts.report_path = std::string(value);
        } else if (arg == "--max-tick-p99-us") {
            ok = ok && ParseNumber(value, opts.max_tick_p99_us);
        } else if (arg == "--max-overrun-ratio") {
            ok = ok && ParseNumber(value, opts.max_overrun_ratio);
        } else if (arg == "--min-tps") {
            ok = ok && ParseNumber(value, opts.min_tps);
        } else if (arg == "--max-lateness-p99-us") {
            ok = ok && ParseNumber(value, opts.max_lateness_p99_us);
        } else if (arg == "--min-hit-confirmation") {
            ok = ok && ParseNumber(value, opts.min_hit_confirmation);
        } else if (arg == "--max-hit-error-p99-mm") {
            ok = ok && ParseNumber(value, opts.max_hit_error_p99_mm);
        } else if (arg == "--max-input-loss") {
            ok = ok && ParseNumber(value, opts.max_input_loss);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }

        if (!ok) {
            std::fprintf(stderr, "Invalid value for %s\n", argv[i]);
            return false;
        }
        ++i;
    }

    if (opts.servers.empty()) {
        std::fprintf(stderr, "At least one --server is required\n");
        return false;
    }
    if (!opts.metrics.empty() && opts.metrics.size() != opts.servers.size()) {
        std::fprintf(stderr, "--metrics must be given once per --server, in the same order\n");
        return false;
    }
    if (opts.clients == 0) {
        opts.clients = static_cast<uint32_t>(opts.servers.size() * engine::kMaxPlayers);
    }
    if (opts.clients > opts.servers.size() * engine::kMaxPlayers) {
        std::fprintf(stderr, "%u clients exceed %zu servers x %zu slots\n",
                     opts.clients, opts.servers.size(), engine::kMaxPlayers);
        return false;
    }
    // An idle server would pass its tick checks without being benchmarked
    if (opts.clients < opts.servers.size()) {
        std::fprintf(stderr, "%u clients cannot load %zu servers\n", opts.clients, opts.servers.size());
        return false;
    }
    return true;
}

double Percentile(std::vector<float> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    const auto index = std::min(values.size() - 1, static_cast<size_t>(quantile * static_cast<double>(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return static_cast<double>(values[index]);
}

double Ratio(double numerator, double denominator) {
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

void RunWorker(const std::vector<loadgen::BotClient*>& bots, const std::atomic<RunStage>& stage,
               std::atomic<uint32_t>& start_failures, WorkerTiming& timing) {
    const auto start = Clock::now();
    for (auto* bot : bots) {
        if (!bot->Start(start)) {
            start_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    RunStage seen = RunStage::Warmup;
    auto next_tick = start;
    for (;;) {
        const RunStage current = stage.load(std::memory_order_acquire);
        if (current != seen) {
            if (seen < RunStage::Measure && current >= RunStage::Measure) {
                for (auto* bot : bots) {
                    bot->BeginMeasurement();
                }
            }
            if (seen < RunStage::Drain && current >= RunStage::Drain) {
                for (auto* bot : bots) {
                    bot->EndMeasurement();
                }
            }
            seen = current;
        }
        if (current == RunStage::Stop) {
            break;
        }

        const auto now = Clock::now();
        for (auto* bot : bots) {
            bot->Tick(now);
        }

        // 128Hz client frame; resynchronize after a stall instead of bursting
        next_tick += engine::kTickDurationNs;
        if (now - next_tick > engine::kTickDurationNs * engine::kMaxCatchupTicks) {
            next_tick = now + engine::kTickDurationNs;
        }
        std::this_thread::sleep_until(next_tick);

        if (current == RunStage::Measure) {
            const auto late = Clock::now() - next_tick;
            ++timing.frames;
            timing.late_frames += late > kLateFrameThreshold ? 1u : 0u;
            timing.max_late_ms = std::max(timing.max_late_ms, std::chrono::duration<double, std::milli>(late).count());
        }
    }

    for (auto* bot : bots) {
        bot->FinalizeShots();
        bot->Stop();
    }
}

Totals Aggregate(const std::vector<std::unique_ptr<loadgen::BotClient>>& bots) {
    Totals totals;
    for (const auto& bot : bots) {
        const loadgen::BotStats& s = bot->Stats();
        totals.connected += s.connected ? 1u : 0u;
        totals.dropped += s.dropped ? 1u : 0u;
        totals.connect_ms_max = std::max(totals.connect_ms_max, s.connect_ms);
        totals.rtt_ms_sum += static_cast<double>(s.rtt_ms);
        totals.rtt_ms_max = std::max(totals.rtt_ms_max, s.rtt_ms);
        totals.rtt_variance_ms_sum += static_cast<double>(s.rtt_variance_ms);
        totals.snapshots_received += s.snapshots_received;
        totals.snapshots_reordered += s.snapshots_reordered;
        totals.snapshot_ticks_missing += s.snapshot_ticks_missing;
        totals.reconcile_checks += s.reconcile_checks;
        totals.mispredictions += s.mispredictions;
        totals.shots_fired += s.shots_fired;
        totals.shots_confirmed += s.shots_confirmed;
        totals.shots_mismatched += s.shots_mismatched;
        totals.shots_unconfirmed += s.shots_unconfirmed;
        totals.shots_rejected += s.shots_rejected;
        totals.shots_throttled += s.shots_throttled;
        totals.shots_invalidated += s.shots_invalidated;
        totals.shots_gap_interpolated += s.shots_gap_interpolated;
        totals.unexpected_results += s.unexpected_results;
        totals.hits_taken += s.hits_taken;
        totals.deaths += s.deaths;
        totals.interp_delay_tick_sum += s.interp_delay_tick_sum;
        totals.interp_delay_samples += s.interp_delay_samples;
        totals.misprediction_error_m.insert(totals.misprediction_error_m.end(),
                                            s.misprediction_error_m.begin(), s.misprediction_error_m.end());
        totals.hit_error_m.insert(totals.hit_error_m.end(), s.hit_error_m.begin(), s.hit_error_m.end());
    }
    return totals;
}

ServerSummary SummarizeServer(const Endpoint& endpoint, const loadgen::MetricsSample& before,
                              const loadgen::MetricsSample& after) {
    ServerSummary summary;
    summary.endpoint = endpoint.host + ":" + std::to_string(endpoint.port);
    summary.scraped = before.ok && after.ok;
    if (!summary.scraped) {
        return summary;
    }

    auto delta = [&](const char* key) { return after.Value(key) - before.Value(key); };

    // Server clock first: impaired scrapes add seconds of variable latency
    const double uptime_delta = delta("game_uptime_seconds");
    summary.window_s = uptime_delta > 0.0 ? uptime_delta
                                          : std::chrono::duration<double>(after.taken_at - before.taken_at).count();
    summary.ticks = delta("game_total_ticks");
    summary.tps = Ratio(summary.ticks, summary.window_s);
    summary.tick_p50_us = loadgen::HistogramQuantileUpperBound(before, after, "game_tick_duration_us", 0.50);
    summary.tick_p99_us = loadgen::HistogramQuantileUpperBound(before, after, "game_tick_duration_us", 0.99);
    summary.tick_p999_us = loadgen::HistogramQuantileUpperBound(before, after, "game_tick_duration_us", 0.999);
    summary.lateness_p99_us = loadgen::HistogramQuantileUpperBound(before, after, "game_tick_lateness_us", 0.99);
    summary.overruns = delta("game_tick_overruns");
    summary.overrun_ratio = Ratio(summary.overruns, summary.ticks);
    summary.shots = delta("game_ballistic_shots");
    summary.hits = delta("game_confirmed_hits");
    summary.rejected_rewinds = delta("game_rejected_rewinds");
    summary.inputs_accepted = delta("game_client_inputs{outcome=\"accepted\"}");
    summary.inputs_missed = delta("game_client_inputs{outcome=\"missed\"}");
    summary.inputs_redundant = delta("game_client_inputs{outcome=\"redundant\"}");
    summary.inputs_overflow = delta("game_client_inputs{outcome=\"overflow\"}");
    summary.malformed = delta("game_malformed_packets");
    summary.net_pool_failures = delta("game_net_pool_failures");
    summary.throttled_shots = delta("game_throttled_shots");
    summary.net_pool_peak_bytes = after.Value("game_net_pool_peak_bytes");
    return summary;
}

// Concurrent scrapes keep every server's window aligned; a failed or truncated scrape is retried
std::vector<loadgen::MetricsSample> ScrapeAll(const std::vector<Endpoint>& endpoints) {
    std::vector<loadgen::MetricsSample> samples(endpoints.size());
    std::vector<std::thread> scrapers;
    scrapers.reserve(endpoints.size());
    for (size_t i = 0; i < endpoints.size(); ++i) {
        scrapers.emplace_back([&samples, &endpoints, i] {
            for (uint32_t attempt = 0; attempt < kScrapeAttempts && !samples[i].ok; ++attempt) {
                samples[i] = loadgen::ScrapeMetrics(endpoints[i].host, endpoints[i].port, kScrapeTimeout);
            }
        });
    }
    for (auto& scraper : scrapers) {
        scraper.join();
    }
    return samples;
}

std::string FormatBucket(double upper_bound_us) {
    char text[32];
    if (upper_bound_us < 0.0) {
        std::snprintf(text, sizeof(text), "> %lld us", static_cast<long long>(engine::kHistogramBuckets.back()));
    } else {
        std::snprintf(text, sizeof(text), "<= %.0f us", upper_bound_us);
    }
    return text;
}

// RFC 8259 string body: quotes, backslashes and control characters escaped
std::string JsonEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
            escaped.push_back(c);
        } else if (byte < 0x20) {
            char unicode[8];
            std::snprintf(unicode, sizeof(unicode), "\\u%04x", byte);
            escaped.append(unicode);
        } else {
            escaped.push_back(c);
        }
    }
    return escaped;
}

} // namespace

int main(int argc, char* argv[]) {
    Options opts;
    if (!ParseOptions(argc, argv, opts)) {
        PrintUsage();
        return kExitUsage;
    }

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "Fatal: enet_initialize failed\n");
        return kExitInternal;
    }

    // Round-robin keeps per-server load within one bot
    std::vector<std::unique_ptr<loadgen::BotClient>> bots;
    bots.reserve(opts.clients);
    for (uint32_t i = 0; i < opts.clients; ++i) {
        const Endpoint& endpoint = opts.servers[i % opts.servers.size()];
        loadgen::BotConfig config;
        if (enet_address_set_host(&config.server, endpoint.host.c_str()) != 0) {
            std::fprintf(stderr, "Fatal: cannot resolve %s\n", endpoint.host.c_str());
            enet_deinitialize();
            return kExitUsage;
        }
        config.server.port = endpoint.port;
        config.seed = opts.seed * 0x9E3779B97F4A7C15ULL + i;
        config.redundancy = opts.redundancy;
        config.fire_interval = std::chrono::milliseconds(opts.fire_interval_ms);
        config.aim = opts.aim;
        config.sample_reserve = static_cast<size_t>(opts.duration_s) * engine::kTickRate;
        bots.push_back(std::make_unique<loadgen::BotClient>(config));
    }

    const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t thread_count = std::clamp(opts.threads ? opts.threads : hardware / 2, 1u, opts.clients);
    std::vector<std::vector<loadgen::BotClient*>> assignments(thread_count);
    for (uint32_t i = 0; i < opts.clients; ++i) {
        assignments[i % thread_count].push_back(bots[i].get());
    }

    std::printf("====================================================================\n");
    std::printf(" Agones-128 Load Generator: %s\n", opts.label.c_str());
    std::printf(" %u clients -> %zu server(s) | %u worker threads | redundancy %u | aim %s\n",
                opts.clients, opts.servers.size(), thread_count, opts.redundancy,
                loadgen::AimModeName(opts.aim));
    std::printf(" Warmup %u s | Window %u s | Drain %u s\n", opts.warmup_s, opts.duration_s, opts.drain_s);
    std::printf("====================================================================\n");
    std::fflush(stdout);

    std::atomic<RunStage> stage{RunStage::Warmup};
    std::atomic<uint32_t> start_failures{0};
    std::vector<std::thread> workers;
    std::vector<WorkerTiming> timings(thread_count);
    workers.reserve(thread_count);
    for (uint32_t w = 0; w < thread_count; ++w) {
        workers.emplace_back(RunWorker, std::cref(assignments[w]), std::cref(stage), std::ref(start_failures),
                             std::ref(timings[w]));
    }

    std::this_thread::sleep_for(std::chrono::seconds(opts.warmup_s));

    const std::vector<loadgen::MetricsSample> before = ScrapeAll(opts.metrics);
    stage.store(RunStage::Measure, std::memory_order_release);
    std::printf(" Measuring...\n");
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::seconds(opts.duration_s));

    stage.store(RunStage::Drain, std::memory_order_release);
    const std::vector<loadgen::MetricsSample> after = ScrapeAll(opts.metrics);

    std::this_thread::sleep_for(std::chrono::seconds(opts.drain_s));
    stage.store(RunStage::Stop, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const Totals totals = Aggregate(bots);
    WorkerTiming harness;
    for (const auto& timing : timings) {
        harness.frames += timing.frames;
        harness.late_frames += timing.late_frames;
        harness.max_late_ms = std::max(harness.max_late_ms, timing.max_late_ms);
    }
    const double harness_late_share = Ratio(static_cast<double>(harness.late_frames), static_cast<double>(harness.frames));
    std::vector<ServerSummary> servers;
    for (size_t i = 0; i < opts.metrics.size(); ++i) {
        servers.push_back(SummarizeServer(opts.metrics[i], before[i], after[i]));
    }

    // Derived client-side ratios
    const double snapshot_expected = static_cast<double>(totals.snapshots_received + totals.snapshot_ticks_missing);
    const double snapshot_loss = Ratio(static_cast<double>(totals.snapshot_ticks_missing), snapshot_expected);
    const double snapshot_reorder = Ratio(static_cast<double>(totals.snapshots_reordered), static_cast<double>(totals.snapshots_received));
    const double misprediction_rate = Ratio(static_cast<double>(totals.mispredictions), static_cast<double>(totals.reconcile_checks));
    // Trades are excluded; rejected, mismatched and unanswered shots all count against confirmation
    const uint64_t shots_evaluated = totals.shots_fired - totals.shots_invalidated;
    const double hit_confirmation = Ratio(static_cast<double>(totals.shots_confirmed), static_cast<double>(shots_evaluated));
    const double gap_share = Ratio(static_cast<double>(totals.shots_gap_interpolated), static_cast<double>(totals.shots_fired));
    const double interp_delay_ticks = Ratio(totals.interp_delay_tick_sum, static_cast<double>(totals.interp_delay_samples));
    const double rtt_avg_ms = Ratio(totals.rtt_ms_sum, static_cast<double>(opts.clients));
    const double rtt_variance_avg_ms = Ratio(totals.rtt_variance_ms_sum, static_cast<double>(opts.clients));

    double inputs_accepted = 0.0;
    double inputs_missed = 0.0;
    double inputs_overflow = 0.0;
    double malformed = 0.0;
    double net_pool_failures = 0.0;
    double throttled_shots = 0.0;
    for (const auto& server : servers) {
        inputs_accepted += server.inputs_accepted;
        inputs_missed += server.inputs_missed;
        inputs_overflow += server.inputs_overflow;
        malformed += server.malformed;
        net_pool_failures += server.net_pool_failures;
        throttled_shots += server.throttled_shots;
    }
    // Overflow was accepted then pushed out of the window; missed never arrived
    const double inputs_lost = inputs_missed + inputs_overflow;
    const double inputs_total = inputs_accepted + inputs_missed;
    const double input_loss = Ratio(inputs_lost, inputs_total);
    const double hit_error_p99_mm = Percentile(totals.hit_error_m, 0.99) * kMetresToMm;

    std::vector<Check> checks;
    checks.push_back({"clients connected", totals.connected == opts.clients && totals.dropped == 0 && start_failures.load() == 0,
                      std::to_string(totals.connected) + "/" + std::to_string(opts.clients) + " connected, " +
                      std::to_string(totals.dropped) + " dropped"});
    for (const auto& server : servers) {
        // A frozen tick thread still answers scrapes; TPS catches it, the histogram checks need ticks to mean anything
        const bool ticked = server.scraped && server.ticks > 0.0;
        const bool tick_ok = ticked && server.tick_p99_us >= 0.0 && server.tick_p99_us <= opts.max_tick_p99_us;
        checks.push_back({server.endpoint + " tick p99", tick_ok, FormatBucket(server.tick_p99_us)});
        char detail[64];
        std::snprintf(detail, sizeof(detail), "%.0f of %.0f ticks (%.4f%%)", server.overruns, server.ticks, server.overrun_ratio * 100.0);
        checks.push_back({server.endpoint + " overruns", ticked && server.overrun_ratio <= opts.max_overrun_ratio, detail});
        std::snprintf(detail, sizeof(detail), "%.2f TPS over %.1f s", server.tps, server.window_s);
        checks.push_back({server.endpoint + " tick rate", ticked && server.tps >= opts.min_tps, detail});
        const bool lateness_ok = ticked && server.lateness_p99_us >= 0.0 && server.lateness_p99_us <= opts.max_lateness_p99_us;
        checks.push_back({server.endpoint + " start lateness p99", lateness_ok, FormatBucket(server.lateness_p99_us)});
    }
    {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "%.4f%% (%.0f of %.0f)", input_loss * 100.0, inputs_lost, inputs_total);
        checks.push_back({"input loss after redundancy", !servers.empty() && input_loss <= opts.max_input_loss, detail});
    }
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "%.3f%% (%llu of %llu evaluated shots)", hit_confirmation * 100.0,
                      static_cast<unsigned long long>(totals.shots_confirmed), static_cast<unsigned long long>(shots_evaluated));
        checks.push_back({"rewind hit confirmation", shots_evaluated > 0 && hit_confirmation >= opts.min_hit_confirmation, detail});
        std::snprintf(detail, sizeof(detail), "p99 %.3f mm over %zu confirmed hits", hit_error_p99_mm, totals.hit_error_m.size());
        checks.push_back({"rewind hit position error", !totals.hit_error_m.empty() && hit_error_p99_mm <= opts.max_hit_error_p99_mm, detail});
    }
    {
        // netem never corrupts or duplicates, and the pool is sized for full load, so any of these is a fault
        char detail[128];
        std::snprintf(detail, sizeof(detail), "%.0f malformed packets, %llu unexpected shot results, %.0f pool refusals",
                      malformed, static_cast<unsigned long long>(totals.unexpected_results), net_pool_failures);
        checks.push_back({"transport integrity", malformed == 0.0 && totals.unexpected_results == 0 && net_pool_failures == 0.0,
                          detail});

        // Bots honor the cooldown in client ticks, so any throttle means client and server disagree on fire timing
        std::snprintf(detail, sizeof(detail), "%.0f fire inputs throttled by the server", throttled_shots);
        checks.push_back({"weapon cooldown agreement", throttled_shots == 0.0, detail});
    }
    const bool all_passed = std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.passed; });

    std::printf("\n Network (client view)\n");
    std::printf("   RTT (ENet)              avg %.1f ms, max %u ms, variance avg %.1f ms\n", rtt_avg_ms, totals.rtt_ms_max, rtt_variance_avg_ms);
    std::printf("   Interpolation delay     avg %.2f ticks (%.1f ms)\n", interp_delay_ticks, interp_delay_ticks * 1000.0 / engine::kTickRate);
    std::printf("   Snapshots               %llu received, %.2f%% missing, %.2f%% reordered\n",
                static_cast<unsigned long long>(totals.snapshots_received), snapshot_loss * 100.0, snapshot_reorder * 100.0);
    std::printf("   Connect latency         max %.1f ms\n", totals.connect_ms_max);
    std::printf("   Harness scheduling      %.3f%% of frames woke > 2 ms late, worst %.1f ms\n",
                harness_late_share * 100.0, harness.max_late_ms);
    std::printf("\n Prediction\n");
    std::printf("   Reconcile checks        %llu, mispredicted %.3f%%, error p50 %.2f mm, p99 %.2f mm\n",
                static_cast<unsigned long long>(totals.reconcile_checks), misprediction_rate * 100.0,
                Percentile(totals.misprediction_error_m, 0.50) * kMetresToMm, Percentile(totals.misprediction_error_m, 0.99) * kMetresToMm);
    std::printf("\n Hit registration\n");
    std::printf("   Shots                   %llu fired, %llu invalidated by trades, %llu evaluated\n",
                static_cast<unsigned long long>(totals.shots_fired), static_cast<unsigned long long>(totals.shots_invalidated),
                static_cast<unsigned long long>(shots_evaluated));
    std::printf("   Confirmed               %.3f%% | mismatched %llu | rejected %llu | throttled %llu | unconfirmed %llu\n",
                hit_confirmation * 100.0, static_cast<unsigned long long>(totals.shots_mismatched),
                static_cast<unsigned long long>(totals.shots_rejected), static_cast<unsigned long long>(totals.shots_throttled),
                static_cast<unsigned long long>(totals.shots_unconfirmed));
    std::printf("   Gap-interpolated shots  %.2f%%\n", gap_share * 100.0);
    std::printf("   Hit position error      p50 %.3f mm, p99 %.3f mm, max %.3f mm\n",
                Percentile(totals.hit_error_m, 0.50) * kMetresToMm, hit_error_p99_mm,
                Percentile(totals.hit_error_m, 1.0) * kMetresToMm);
    std::printf("   Deaths / hits taken     %llu / %llu\n",
                static_cast<unsigned long long>(totals.deaths), static_cast<unsigned long long>(totals.hits_taken));

    std::printf("\n Servers\n");
    for (const auto& server : servers) {
        if (!server.scraped) {
            std::printf("   %-21s metrics scrape failed\n", server.endpoint.c_str());
            continue;
        }
        std::printf("   %-21s %.2f TPS | tick p50 %s, p99 %s, p99.9 %s | lateness p99 %s\n",
                    server.endpoint.c_str(), server.tps, FormatBucket(server.tick_p50_us).c_str(),
                    FormatBucket(server.tick_p99_us).c_str(), FormatBucket(server.tick_p999_us).c_str(),
                    FormatBucket(server.lateness_p99_us).c_str());
        std::printf("   %-21s inputs %.0f accepted, %.0f missed, %.0f redundant, %.0f overflow | shots %.0f, hits %.0f, rejected rewinds %.0f\n",
                    "", server.inputs_accepted, server.inputs_missed, server.inputs_redundant, server.inputs_overflow,
                    server.shots, server.hits, server.rejected_rewinds);
        std::printf("   %-21s net pool peak %.1f KiB, %.0f refusals\n", "", server.net_pool_peak_bytes / 1024.0,
                    server.net_pool_failures);
    }

    std::printf("\n Checks\n");
    for (const auto& check : checks) {
        std::printf("   [%s] %-34s %s\n", check.passed ? "PASS" : "FAIL", check.name.c_str(), check.detail.c_str());
    }
    std::printf("\n RESULT: %s\n", all_passed ? "PASS" : "FAIL");
    std::printf("====================================================================\n");

    bool report_ok = true;
    if (!opts.report_path.empty()) {
        FILE* report = std::fopen(opts.report_path.c_str(), "w");
        if (!report) {
            std::fprintf(stderr, "Error: cannot open report %s\n", opts.report_path.c_str());
            report_ok = false;
        } else {
            std::fprintf(report, "{\n  \"label\": \"%s\",\n  \"passed\": %s,\n", JsonEscape(opts.label).c_str(), all_passed ? "true" : "false");
            std::fprintf(report, "  \"config\": {\"clients\": %u, \"servers\": %zu, \"window_s\": %u, \"redundancy\": %u, \"fire_interval_ms\": %u, \"aim\": \"%s\"},\n",
                         opts.clients, opts.servers.size(), opts.duration_s, opts.redundancy, opts.fire_interval_ms,
                         loadgen::AimModeName(opts.aim));
            std::fprintf(report, "  \"network\": {\"rtt_avg_ms\": %.2f, \"rtt_max_ms\": %u, \"rtt_variance_avg_ms\": %.2f, \"interp_delay_ticks\": %.3f, "
                                 "\"snapshots_received\": %llu, \"snapshot_loss\": %.6f, \"snapshot_reorder\": %.6f},\n",
                         rtt_avg_ms, totals.rtt_ms_max, rtt_variance_avg_ms, interp_delay_ticks,
                         static_cast<unsigned long long>(totals.snapshots_received), snapshot_loss, snapshot_reorder);
            std::fprintf(report, "  \"prediction\": {\"checks\": %llu, \"misprediction_rate\": %.6f, \"error_p50_m\": %.6f, \"error_p99_m\": %.6f},\n",
                         static_cast<unsigned long long>(totals.reconcile_checks), misprediction_rate,
                         Percentile(totals.misprediction_error_m, 0.50), Percentile(totals.misprediction_error_m, 0.99));
            std::fprintf(report, "  \"hit_registration\": {\"fired\": %llu, \"invalidated\": %llu, \"evaluated\": %llu, \"confirmed\": %llu, "
                                 "\"mismatched\": %llu, \"rejected\": %llu, \"throttled\": %llu, \"unconfirmed\": %llu, \"unexpected\": %llu, \"confirmation_rate\": %.6f, "
                                 "\"gap_interpolated_share\": %.6f, \"hit_error_p50_m\": %.6f, \"hit_error_p99_m\": %.6f, \"hit_error_max_m\": %.6f},\n",
                         static_cast<unsigned long long>(totals.shots_fired), static_cast<unsigned long long>(totals.shots_invalidated),
                         static_cast<unsigned long long>(shots_evaluated), static_cast<unsigned long long>(totals.shots_confirmed),
                         static_cast<unsigned long long>(totals.shots_mismatched), static_cast<unsigned long long>(totals.shots_rejected),
                         static_cast<unsigned long long>(totals.shots_throttled),
                         static_cast<unsigned long long>(totals.shots_unconfirmed), static_cast<unsigned long long>(totals.unexpected_results),
                         hit_confirmation, gap_share,
                         Percentile(totals.hit_error_m, 0.50), Percentile(totals.hit_error_m, 0.99), Percentile(totals.hit_error_m, 1.0));
            std::fprintf(report, "  \"harness\": {\"frames\": %llu, \"late_frame_share\": %.6f, \"max_late_ms\": %.3f},\n",
                         static_cast<unsigned long long>(harness.frames), harness_late_share, harness.max_late_ms);
            std::fprintf(report, "  \"input_loss\": %.6f,\n  \"servers\": [\n", input_loss);
            for (size_t i = 0; i < servers.size(); ++i) {
                const auto& s = servers[i];
                std::fprintf(report, "    {\"endpoint\": \"%s\", \"scraped\": %s, \"tps\": %.3f, \"tick_p50_us\": %.0f, \"tick_p99_us\": %.0f, "
                                     "\"tick_p999_us\": %.0f, \"lateness_p99_us\": %.0f, \"overruns\": %.0f, \"ticks\": %.0f, "
                                     "\"inputs_accepted\": %.0f, \"inputs_missed\": %.0f, \"inputs_redundant\": %.0f, "
                                     "\"inputs_overflow\": %.0f, \"shots\": %.0f, \"hits\": %.0f, \"rejected_rewinds\": %.0f, \"malformed\": %.0f, "
                                     "\"net_pool_peak_bytes\": %.0f, \"net_pool_failures\": %.0f, \"throttled_shots\": %.0f}%s\n",
                             JsonEscape(s.endpoint).c_str(), s.scraped ? "true" : "false", s.tps, s.tick_p50_us, s.tick_p99_us,
                             s.tick_p999_us, s.lateness_p99_us, s.overruns, s.ticks, s.inputs_accepted, s.inputs_missed,
                             s.inputs_redundant, s.inputs_overflow, s.shots, s.hits, s.rejected_rewinds, s.malformed,
                             s.net_pool_peak_bytes, s.net_pool_failures, s.throttled_shots, i + 1 < servers.size() ? "," : "");
            }
            std::fprintf(report, "  ],\n  \"checks\": [\n");
            for (size_t i = 0; i < checks.size(); ++i) {
                std::fprintf(report, "    {\"name\": \"%s\", \"passed\": %s, \"detail\": \"%s\"}%s\n",
                             JsonEscape(checks[i].name).c_str(), checks[i].passed ? "true" : "false",
                             JsonEscape(checks[i].detail).c_str(), i + 1 < checks.size() ? "," : "");
            }
            std::fprintf(report, "  ]\n}\n");
            report_ok = std::ferror(report) == 0;
            report_ok = std::fclose(report) == 0 && report_ok;
            if (!report_ok) {
                std::fprintf(stderr, "Error: failed writing report %s\n", opts.report_path.c_str());
            }
        }
    }

    bots.clear();
    enet_deinitialize();
    if (!report_ok) {
        return kExitInternal;
    }
    return all_passed ? 0 : kExitChecksFailed;
}
