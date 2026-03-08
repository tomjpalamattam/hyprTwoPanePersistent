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

message(STATUS "Using GIT_COMMIT_HASH from Hyprland version.h")

install(TARGETS hyprTwoPanePersistent
    LIBRARY DESTINATION lib/hyprland/plugins
)