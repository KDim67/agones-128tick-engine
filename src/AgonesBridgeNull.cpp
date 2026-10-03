#include "engine/AgonesBridge.hpp"
#include <cstdio>

// Backend for builds without the Agones SDK; Start() always refuses
namespace engine {

struct AgonesBridge::Impl {};

AgonesBridge::AgonesBridge() noexcept = default;

AgonesBridge::~AgonesBridge() = default;

bool AgonesBridge::Start(const AgonesConfig&) noexcept {
    std::fprintf(stderr, "[Agones] SDK not linked into this build, reconfigure with -DENGINE_WITH_AGONES=ON\n");
    return false;
}

void AgonesBridge::Stop() noexcept {}

} // namespace engine
