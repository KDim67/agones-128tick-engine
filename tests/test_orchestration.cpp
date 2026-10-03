#include "engine/GameServerLifecycle.hpp"
#include "Check.hpp"

#include <chrono>
#include <cstdio>

namespace {

using engine::GameServerState;
using engine::LifecycleAction;
using engine::SessionLifecyclePolicy;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kEmptyGrace = 60s;
constexpr std::chrono::milliseconds kRetry = 2s;
constexpr SessionLifecyclePolicy::Clock::time_point kT0{};

static_assert(engine::ParseGameServerState("Allocated") == GameServerState::Allocated);
static_assert(engine::ParseGameServerState("Creating") == GameServerState::Unknown);
static_assert(engine::GameServerStateName(GameServerState::Ready) == "Ready");

void TestStateParsing() {
    std::printf("[Test] Running Agones GameServer State Parsing...\n");

    for (size_t i = 0; i < engine::kGameServerStateCount; ++i) {
        const auto state = static_cast<GameServerState>(i);
        CHECK(engine::ParseGameServerState(engine::GameServerStateName(state)) == state);
    }

    CHECK(engine::ParseGameServerState("") == GameServerState::Unknown);
    CHECK(engine::ParseGameServerState("PortAllocation") == GameServerState::Unknown);
    CHECK(engine::ParseGameServerState("allocated") == GameServerState::Unknown);
    CHECK(engine::GameServerStateName(GameServerState::Count) == "Unknown");

    std::printf("  -> All %zu states round-trip; transitional states map to Unknown.\n",
                engine::kGameServerStateCount);
}

void TestSelfAllocation() {
    std::printf("[Test] Running Self-Allocation of Occupied Ready Servers...\n");
    SessionLifecyclePolicy policy(kEmptyGrace, kRetry);

    CHECK(policy.Evaluate(GameServerState::Ready, 0, kT0) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Ready, 2, kT0 + 1s) == LifecycleAction::Allocate);

    // Watch lag: still Ready inside the retry window
    CHECK(policy.Evaluate(GameServerState::Ready, 2, kT0 + 1s + 250ms) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Ready, 2, kT0 + 2s + 999ms) == LifecycleAction::None);

    // Failed RPC: reissued once the retry window elapses
    CHECK(policy.Evaluate(GameServerState::Ready, 2, kT0 + 3s) == LifecycleAction::Allocate);

    CHECK(policy.Evaluate(GameServerState::Allocated, 2, kT0 + 4s) == LifecycleAction::None);
    CHECK(!policy.IsShutdownPending());

    std::printf("  -> Allocate() issued on first player, retried after %lld ms, quiet once Allocated.\n",
                static_cast<long long>(kRetry.count()));
}

void TestEmptySessionRecycle() {
    std::printf("[Test] Running Empty Allocated Session Recycling...\n");
    SessionLifecyclePolicy policy(kEmptyGrace, kRetry);

    // Abandoned allocation: nobody connects
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, kT0) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, kT0 + 30s) == LifecycleAction::None);

    // Player join resets the grace window
    CHECK(policy.Evaluate(GameServerState::Allocated, 1, kT0 + 40s) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, kT0 + 50s) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, kT0 + 50s + kEmptyGrace - 1ms) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, kT0 + 50s + kEmptyGrace) == LifecycleAction::Shutdown);
    CHECK(policy.IsShutdownPending());

    // Pending Shutdown survives late joiners
    const auto issued = kT0 + 50s + kEmptyGrace;
    CHECK(policy.Evaluate(GameServerState::Allocated, 3, issued + 500ms) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 3, issued + kRetry) == LifecycleAction::Shutdown);
    CHECK(policy.Evaluate(GameServerState::Shutdown, 0, issued + 10s) == LifecycleAction::None);

    std::printf("  -> Shutdown() after %lld s of continuous emptiness, latched and retried until confirmed.\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kEmptyGrace).count()));
}

void TestPassiveStates() {
    std::printf("[Test] Running Passive State Handling (Unknown, Scheduled, RequestReady, Reserved, Unhealthy, Error)...\n");
    SessionLifecyclePolicy policy(kEmptyGrace, kRetry);

    constexpr GameServerState kPassive[] = {
        GameServerState::Unknown, GameServerState::Scheduled, GameServerState::RequestReady,
        GameServerState::Reserved, GameServerState::Unhealthy, GameServerState::Error
    };

    auto now = kT0;
    for (const GameServerState state : kPassive) {
        CHECK(policy.Evaluate(state, 0, now) == LifecycleAction::None);
        CHECK(policy.Evaluate(state, 4, now + 1s) == LifecycleAction::None);
        CHECK(policy.Evaluate(state, 0, now + 10min) == LifecycleAction::None);
        now += 1h;
    }

    // Reserved resets the grace window
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, now) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Reserved, 0, now + 59s) == LifecycleAction::None);
    CHECK(policy.Evaluate(GameServerState::Allocated, 0, now + 61s) == LifecycleAction::None);
    CHECK(!policy.IsShutdownPending());

    std::printf("  -> No lifecycle calls issued outside Ready and Allocated.\n");
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf(" Agones-128 Engine: Orchestration Test Verification\n");
    std::printf("====================================================\n");

    TestStateParsing();
    TestSelfAllocation();
    TestEmptySessionRecycle();
    TestPassiveStates();

    std::printf("\nAll Orchestration Verification Checks PASSED (100%%).\n");
    return 0;
}
