cmake_minimum_required(VERSION 3.19)
project(hyprTwoPanePersistent VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hyprTwoPanePersistent SHARED
    main.cpp
    TwoPanePersistent.cpp
)

find_package(PkgConfig REQUIRED)

# hyprpm sets PKG_CONFIG_PATH at build time — use it to find hyprland.pc
pkg_check_modules(HYPRLAND REQUIRED hyprland)

# The hyprpm headersRoot is the canonical include location.
# Derive it from the pkg-config prefix (works when PKG_CONFIG_PATH is set by hyprpm),
# and also hardcode the known path as a fallback.
execute_process(
    COMMAND pkg-config --variable=prefix hyprland
    OUTPUT_VARIABLE HYPRLAND_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Hyprland prefix: ${HYPRLAND_PREFIX}")
message(STATUS "Hyprland include dirs: ${HYPRLAND_INCLUDE_DIRS}")

target_include_directories(hyprTwoPanePersistent PRIVATE
    # From pkg-config (set correctly when hyprpm invokes cmake)
    ${HYPRLAND_INCLUDE_DIRS}
    # Derived from pkg-config prefix
    ${HYPRLAND_PREFIX}/include
    # Hardcoded fallback — the path hyprpm uses on this machine
    /var/cache/hyprpm/tom/headersRoot/include
)

target_compile_options(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_CFLAGS_OTHER}
    -fPIC
    -Wall
    -Wextra
    # Suppress warnings from Hyprland internals we don't control
    -Wno-unused-parameter
)

target_link_libraries(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_LIBRARIES}
)

# Embed the Hyprland git hash for ABI verification at runtime.
# hyprpm passes PKG_CONFIG_PATH so pkg-config finds the right hyprland.pc
execute_process(
    COMMAND pkg-config --variable=commitHash hyprland
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Fallback: parse from hyprctl
if(NOT GIT_COMMIT_HASH OR GIT_COMMIT_HASH STREQUAL "")
    execute_process(
        COMMAND hyprctl version
        OUTPUT_VARIABLE HYPRLAND_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "commit ([a-f0-9]+)" _ ${HYPRLAND_VERSION_OUTPUT})
    set(GIT_COMMIT_HASH ${CMAKE_MATCH_1})
endif()

message(STATUS "Building against Hyprland commit: ${GIT_COMMIT_HASH}")

target_compile_definitions(hyprTwoPanePersistent PRIVATE
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

install(TARGETS hyprTwoPanePersistent
    LIBRARY DESTINATION lib/hyprland/plugins
)