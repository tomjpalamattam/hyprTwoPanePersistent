cmake_minimum_required(VERSION 3.19)
project(hyprTwoPanePersistent VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build as a shared library (.so) — this is what Hyprland loads
add_library(hyprTwoPanePersistent SHARED
    main.cpp
    TwoPanePersistent.cpp
)

# Find Hyprland headers (installed by your package manager or source build)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED hyprland)

target_include_directories(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_INCLUDE_DIRS}
)

target_compile_options(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_CFLAGS_OTHER}
    -fPIC
    -Wall
    -Wextra
)

target_link_libraries(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_LIBRARIES}
)

# Embed the Hyprland git hash for ABI verification at load time
execute_process(
    COMMAND hyprctl version
    OUTPUT_VARIABLE HYPRLAND_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "commit ([a-f0-9]+)" _ ${HYPRLAND_VERSION_OUTPUT})
set(GIT_COMMIT_HASH ${CMAKE_MATCH_1})

if(NOT GIT_COMMIT_HASH)
    # Fallback: grab from installed header if hyprctl not available at build time
    execute_process(
        COMMAND bash -c "pkg-config --variable=commitHash hyprland 2>/dev/null || echo unknown"
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

message(STATUS "Building against Hyprland commit: ${GIT_COMMIT_HASH}")

target_compile_definitions(hyprTwoPanePersistent PRIVATE
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

install(TARGETS hyprTwoPanePersistent
    LIBRARY DESTINATION lib/hyprland/plugins
)