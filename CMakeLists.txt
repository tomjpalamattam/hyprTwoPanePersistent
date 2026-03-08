cmake_minimum_required(VERSION 3.19)
project(hyprTwoPanePersistent VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hyprTwoPanePersistent SHARED
    main.cpp
    TwoPanePersistent.cpp
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED hyprland)

execute_process(
    COMMAND pkg-config --variable=prefix hyprland
    OUTPUT_VARIABLE HYPRLAND_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_include_directories(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_INCLUDE_DIRS}
    ${HYPRLAND_PREFIX}/include
    /var/cache/hyprpm/tom/headersRoot/include
)

target_compile_options(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_CFLAGS_OTHER}
    -fPIC
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

target_link_libraries(hyprTwoPanePersistent PRIVATE
    ${HYPRLAND_LIBRARIES}
)

# Use the full Hyprland ABI string (not just commit hash)
# This matches what __hyprland_api_get_hash() returns at runtime
execute_process(
    COMMAND bash -c "hyprctl version 2>/dev/null | grep -oP 'Version ABI string: \\K\\S+'"
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Fallback: just the commit hash (plugin will warn but still load)
if(NOT GIT_COMMIT_HASH OR GIT_COMMIT_HASH STREQUAL "")
    set(GIT_COMMIT_HASH "4b07770b9ef1cceb2e6f56d33538aaffb9186b9c_aq_0.10_hu_0.11_hg_0.5_hc_0.1_hlg_0.6")
    message(WARNING "Could not detect ABI string, using hardcoded v0.54.1 value")
endif()

message(STATUS "Building against Hyprland commit: ${GIT_COMMIT_HASH}")

target_compile_definitions(hyprTwoPanePersistent PRIVATE
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

install(TARGETS hyprTwoPanePersistent
    LIBRARY DESTINATION lib/hyprland/plugins
)