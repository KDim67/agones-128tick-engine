cmake_minimum_required(VERSION 3.20)
project(agones-128tick-engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable ASan and UBSan instrumentation" OFF)
option(ENGINE_WITH_AGONES "Link the Agones C++ SDK for GameServer lifecycle control" OFF)
option(ENGINE_BUILD_TOOLS "Build the headless load generator" ON)

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
)
FetchContent_MakeAvailable(enet)

# Warnings as errors; STRICT adds GCC conversion and shadowing checks and MSVC conformance flags for shipped binaries
function(engine_configure_target target)
    cmake_parse_arguments(ARG "STRICT" "" "" ${ARGN})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
        if(ARG_STRICT)
            target_compile_options(${target} PRIVATE /Zc:inline /Zc:preprocessor)
        endif()
        if(ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(ARG_STRICT)
            target_compile_options(${target} PRIVATE -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wunused)
        endif()
        if(ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()

    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32 winmm)
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
endfunction()

add_executable(game_server
    src/main.cpp
    src/TickLoop.cpp
    src/NetServer.cpp
    src/NetMemoryPool.cpp
    src/WorldState.cpp
    src/LagCompensation.cpp
    src/Metrics.cpp
    src/MetricsServer.cpp
)
engine_configure_target(game_server STRICT)
target_include_directories(game_server PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(game_server PRIVATE enet)

if(ENGINE_WITH_AGONES)
    find_package(agones CONFIG REQUIRED)
    target_sources(game_server PRIVATE src/AgonesBridge.cpp)
    target_link_libraries(game_server PRIVATE agones)
    if(MSVC)
        # std::getenv for AGONES_SDK_GRPC_PORT trips C4996 under /WX
        target_compile_definitions(game_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
else()
    target_sources(game_server PRIVATE src/AgonesBridgeNull.cpp)
endif()

if(ENGINE_BUILD_TOOLS)
    add_executable(load_generator
        tools/loadgen/main.cpp
        tools/loadgen/BotClient.cpp
        tools/loadgen/MetricsScraper.cpp
        src/WorldState.cpp
        src/LagCompensation.cpp
    )
    engine_configure_target(load_generator STRICT)
    target_include_directories(load_generator PRIVATE ${enet_SOURCE_DIR}/include)
    target_link_libraries(load_generator PRIVATE enet)
    if(MSVC)
        # fopen for the JSON report trips C4996 under /WX
        target_compile_definitions(load_generator PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

add_executable(test_simulation
    tests/test_simulation.cpp
    src/WorldState.cpp
    src/LagCompensation.cpp
)
engine_configure_target(test_simulation)

add_executable(test_telemetry
    tests/test_telemetry.cpp
    src/Metrics.cpp
    src/MetricsServer.cpp
)
engine_configure_target(test_telemetry)

add_executable(test_orchestration
    tests/test_orchestration.cpp
)
engine_configure_target(test_orchestration)

add_executable(test_protocol
    tests/test_protocol.cpp
)
engine_configure_target(test_protocol)

enable_testing()
add_test(NAME test_simulation COMMAND test_simulation)
add_test(NAME test_telemetry COMMAND test_telemetry)
add_test(NAME test_orchestration COMMAND test_orchestration)
add_test(NAME test_protocol COMMAND test_protocol)

install(TARGETS game_server RUNTIME DESTINATION bin COMPONENT runtime)
if(ENGINE_BUILD_TOOLS)
    install(TARGETS game_server load_generator RUNTIME DESTINATION bin COMPONENT bench)
endif()
