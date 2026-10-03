#include "engine/AgonesBridge.hpp"

#include <agones/sdk.h>
#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace engine {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kControlPeriod{250};
constexpr std::chrono::milliseconds kRetryInterval{2000};
constexpr std::chrono::milliseconds kConnectProbe{250};
constexpr std::chrono::milliseconds kShutdownDeadline{3000};
constexpr int kMaxReconnectBackoffMs = 1000;

std::string SidecarAddress() {
    const char* port = std::getenv("AGONES_SDK_GRPC_PORT");
    return std::string("localhost:") + ((port && *port) ? port : "9357");
}

// 1s reconnect backoff cap for late sidecar start
std::shared_ptr<grpc::Channel> CreateSidecarChannel(const std::string& address) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, kMaxReconnectBackoffMs);
    return grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
}

void LogStatus(const char* call, const grpc::Status& status) {
    std::fflush(stdout);
    if (status.ok()) {
        std::printf("[Agones] %s() accepted\n", call);
        std::fflush(stdout);
    } else {
        std::fprintf(stderr, "[Agones] %s() failed (%d): %s\n",
                     call, static_cast<int>(status.error_code()), status.error_message().c_str());
    }
}

} // namespace

struct AgonesBridge::Impl {
    Impl(AgonesBridge& owner, const AgonesConfig& cfg)
        : bridge(owner),
          config(cfg),
          policy(cfg.empty_grace, kRetryInterval),
          address(SidecarAddress()),
          channel(CreateSidecarChannel(address)),
          stub(agones::dev::sdk::SDK::NewStub(channel)) {}

    // False once Stop() has been requested
    bool Idle(std::chrono::milliseconds duration) {
        std::unique_lock lock(mutex);
        return !wake.wait_for(lock, duration, [this] { return stopping; });
    }

    bool IsStopping() {
        std::lock_guard lock(mutex);
        return stopping;
    }

    // Interruptible stand-in for the SDK 30s handshake wait
    bool AwaitSidecar() {
        while (!IsStopping()) {
            if (channel->WaitForConnected(std::chrono::system_clock::now() + kConnectProbe)) {
                return true;
            }
        }
        return false;
    }

    // New SDK per handshake; health stream is not reopenable
    bool ConnectSdk() {
        sdk.reset();
        if (!AwaitSidecar()) {
            return false;
        }
        auto candidate = std::make_unique<agones::SDK>();
        if (!candidate->Connect()) {
            return false;
        }
        sdk = std::move(candidate);
        return true;
    }

    // Raw stub: SDK::Shutdown() hardcodes a 30s deadline, past the 15s pod grace period
    grpc::Status SendShutdown() {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + kShutdownDeadline);
        agones::dev::sdk::Empty request;
        agones::dev::sdk::Empty response;
        return stub->Shutdown(&context, request, &response);
    }

    void RunControl();
    void RunWatch();
    void Execute(LifecycleAction action, uint32_t active_players);
    void Finalize();

    AgonesBridge& bridge;
    AgonesConfig config;
    SessionLifecyclePolicy policy;
    std::string address;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<agones::dev::sdk::SDK::Stub> stub;
    std::unique_ptr<agones::SDK> sdk;
    bool shutdown_sent{false};

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping{false};
    grpc::ClientContext* watch_context{nullptr};

    std::thread control_thread;
    std::thread watch_thread;
};

void AgonesBridge::Impl::RunControl() {
    while (!ConnectSdk()) {
        if (IsStopping()) {
            return;
        }
        std::fprintf(stderr, "[Agones] SDK handshake with %s failed, retrying\n", address.c_str());
        if (!Idle(kRetryInterval)) {
            return;
        }
    }
    std::printf("[Agones] Connected to SDK sidecar at %s\n", address.c_str());
    std::fflush(stdout);

    // Ready() gated on first completed tick
    while (bridge.heartbeat_tick_.load(std::memory_order_relaxed) == 0) {
        if (!Idle(kControlPeriod)) {
            Finalize();
            return;
        }
    }

    for (;;) {
        const grpc::Status status = sdk->Ready();
        LogStatus("Ready", status);
        if (status.ok()) {
            break;
        }
        if (!Idle(kRetryInterval)) {
            Finalize();
            return;
        }
    }

    uint64_t last_heartbeat = 0;
    auto next_health = SteadyClock::now();

    while (Idle(kControlPeriod)) {
        const auto now = SteadyClock::now();

        if (now >= next_health) {
            next_health = now + config.health_interval;
            const uint64_t heartbeat = bridge.heartbeat_tick_.load(std::memory_order_relaxed);

            // Withhold Health() while tick loop is stalled
            if (heartbeat == last_heartbeat) {
                std::fprintf(stderr, "[Agones] Tick loop stalled at tick %llu, withholding Health()\n",
                             static_cast<unsigned long long>(heartbeat));
            } else if (!sdk || !sdk->Health()) {
                std::fprintf(stderr, "[Agones] Health stream broken, reconnecting\n");
                ConnectSdk();
            }
            last_heartbeat = heartbeat;
        }

        if (sdk && !shutdown_sent) {
            const uint32_t players = bridge.active_players_.load(std::memory_order_relaxed);
            Execute(policy.Evaluate(bridge.GetState(), players, now), players);
        }
    }

    Finalize();
}

void AgonesBridge::Impl::Execute(LifecycleAction action, uint32_t active_players) {
    switch (action) {
        case LifecycleAction::Allocate:
            std::printf("[Agones] %u player(s) on Ready server, self-allocating\n", active_players);
            LogStatus("Allocate", sdk->Allocate());
            break;

        case LifecycleAction::Shutdown: {
            std::printf("[Agones] Allocated session empty for %lld ms, recycling\n",
                        static_cast<long long>(config.empty_grace.count()));
            const grpc::Status status = SendShutdown();
            LogStatus("Shutdown", status);
            if (status.ok()) {
                shutdown_sent = true;
                bridge.shutdown_requested_.store(true, std::memory_order_release);
            }
            break;
        }

        case LifecycleAction::None:
        default:
            break;
    }
}

// Shutdown() on clean exit
void AgonesBridge::Impl::Finalize() {
    if (!sdk || shutdown_sent) {
        return;
    }
    const grpc::Status status = SendShutdown();
    LogStatus("Shutdown", status);
    shutdown_sent = status.ok();
}

// Raw stub watch; SDK::WatchGameServer is not cancellable
void AgonesBridge::Impl::RunWatch() {
    while (AwaitSidecar()) {
        grpc::ClientContext context;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            watch_context = &context;
        }

        agones::dev::sdk::Empty request;
        agones::dev::sdk::GameServer game_server;
        const auto reader = stub->WatchGameServer(&context, request);

        while (reader->Read(&game_server)) {
            const std::string& raw_state = game_server.status().state();
            const GameServerState next = ParseGameServerState(raw_state);
            const auto prev = static_cast<GameServerState>(
                bridge.state_.exchange(static_cast<uint8_t>(next), std::memory_order_relaxed));

            if (prev != next) {
                const std::string_view prev_name = GameServerStateName(prev);
                std::printf("[Agones] GameServer %s: %.*s -> %s\n",
                            game_server.object_meta().name().c_str(),
                            static_cast<int>(prev_name.size()), prev_name.data(),
                            raw_state.c_str());
                std::fflush(stdout);
            }
        }

        const grpc::Status status = reader->Finish();
        {
            std::lock_guard lock(mutex);
            watch_context = nullptr;
        }

        if (IsStopping()) {
            return;
        }
        std::fprintf(stderr, "[Agones] Watch stream closed (%d): %s, resubscribing\n",
                     static_cast<int>(status.error_code()), status.error_message().c_str());
        if (!Idle(kRetryInterval)) {
            return;
        }
    }
}

AgonesBridge::AgonesBridge() noexcept = default;

AgonesBridge::~AgonesBridge() {
    Stop();
}

bool AgonesBridge::Start(const AgonesConfig& config) noexcept {
    if (impl_) {
        return false;
    }

    try {
        impl_ = std::make_unique<Impl>(*this, config);
        impl_->control_thread = std::thread(&Impl::RunControl, impl_.get());
        impl_->watch_thread = std::thread(&Impl::RunWatch, impl_.get());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Agones] Failed to start control plane: %s\n", e.what());
        Stop();
        return false;
    }

    std::printf("[Agones] Control plane started (sidecar %s, health %lld ms, empty grace %lld ms)\n",
                impl_->address.c_str(),
                static_cast<long long>(config.health_interval.count()),
                static_cast<long long>(config.empty_grace.count()));
    std::fflush(stdout);
    return true;
}

void AgonesBridge::Stop() noexcept {
    if (!impl_) {
        return;
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        if (impl_->watch_context) {
            impl_->watch_context->TryCancel();
        }
    }
    impl_->wake.notify_all();

    if (impl_->control_thread.joinable()) {
        impl_->control_thread.join();
    }
    if (impl_->watch_thread.joinable()) {
        impl_->watch_thread.join();
    }
    impl_.reset();
}

} // namespace engine
