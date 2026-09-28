cmake_minimum_required(VERSION 3.20)
project(agones-128tick-engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable ASan and UBSan instrumentation" OFF)

include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)

add_executable(game_server
    src/main.cpp
    src/TickLoop.cpp
    src/NetServer.cpp
)

target_include_directories(game_server PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${enet_SOURCE_DIR}/include
)

target_link_libraries(game_server PRIVATE enet)

if(MSVC)
    target_compile_options(game_server PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:inline
        /Zc:preprocessor
    )
    target_link_libraries(game_server PRIVATE ws2_32 winmm)

    if(ENABLE_SANITIZERS)
        target_compile_options(game_server PRIVATE /fsanitize=address)
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

    if(WIN32)
        target_link_libraries(game_server PRIVATE ws2_32 winmm)
    else()
        target_link_libraries(game_server PRIVATE pthread)
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
