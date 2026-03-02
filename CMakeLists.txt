cmake_minimum_required(VERSION 3.20)
project(AethonAgent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Agent Binary ─────────────────────────────────────────────────────────────
add_executable(agent src/main.cpp)

# We do NOT link nvml.lib — the whole point is runtime dynamic loading.
# We DO link winhttp.lib for the heartbeat client (Issue #6).
if(WIN32)
    target_link_libraries(agent PRIVATE winhttp)
    # kernel32 is linked implicitly by MSVC; nothing extra needed.
endif()

# Treat warnings as errors during development.
if(MSVC)
    target_compile_definitions(agent PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(agent PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(agent PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()