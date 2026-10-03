cmake_minimum_required(VERSION 3.20)
project(agones-128tick-engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable ASan and UBSan instrumentation" OFF)
option(ENGINE_WITH_AGONES "Link the Agones C++ SDK for GameServer lifecycle control" OFF)

include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
)
FetchContent_MakeAvailable(enet)

add_executable(game_server
    src/main.cpp
    src/TickLoop.cpp
    src/NetServer.cpp
    src/WorldState.cpp
    src/LagCompensation.cpp
    src/Metrics.cpp
    src/MetricsServer.cpp
)

target_include_directories(game_server PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${enet_SOURCE_DIR}/include
)

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

add_executable(test_simulation
    tests/test_simulation.cpp
    src/WorldState.cpp
    src/LagCompensation.cpp
)

target_include_directories(test_simulation PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

add_executable(test_telemetry
    tests/test_telemetry.cpp
    src/Metrics.cpp
    src/MetricsServer.cpp
)

target_include_directories(test_telemetry PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

add_executable(test_orchestration
    tests/test_orchestration.cpp
)

target_include_directories(test_orchestration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(game_server PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:inline
        /Zc:preprocessor
    )
    target_compile_options(test_simulation PRIVATE
        /W4
        /WX
        /permissive-
    )
    target_compile_options(test_telemetry PRIVATE
        /W4
        /WX
        /permissive-
    )
    target_compile_options(test_orchestration PRIVATE
        /W4
        /WX
        /permissive-
    )
    target_link_libraries(game_server PRIVATE ws2_32 winmm)
    target_link_libraries(test_telemetry PRIVATE ws2_32)

    if(ENABLE_SANITIZERS)
        target_compile_options(game_server PRIVATE /fsanitize=address)
        target_compile_options(test_simulation PRIVATE /fsanitize=address)
        target_compile_options(test_telemetry PRIVATE /fsanitize=address)
        target_compile_options(test_orchestration PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(game_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wunused
        -Werror
    )
    target_compile_options(test_simulation PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
    target_compile_options(test_telemetry PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
    target_compile_options(test_orchestration PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )

    if(WIN32)
        target_link_libraries(game_server PRIVATE ws2_32 winmm)
        target_link_libraries(test_telemetry PRIVATE ws2_32)
    else()
        target_link_libraries(game_server PRIVATE pthread)
        target_link_libraries(test_telemetry PRIVATE pthread)
    endif()

    if(ENABLE_SANITIZERS)
        target_compile_options(game_server PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(game_server PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endif()

enable_testing()
add_test(NAME test_simulation COMMAND test_simulation)
add_test(NAME test_telemetry COMMAND test_telemetry)
add_test(NAME test_orchestration COMMAND test_orchestration)

install(TARGETS game_server RUNTIME DESTINATION bin COMPONENT runtime)
