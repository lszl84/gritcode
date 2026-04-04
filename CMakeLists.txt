cmake_minimum_required(VERSION 3.24)
project(fastcode-native CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- cmark (CommonMark markdown parser) ---
FetchContent_Declare(cmark
    GIT_REPOSITORY https://github.com/commonmark/cmark.git
    GIT_TAG 0.31.2
    GIT_SHALLOW ON
)
set(CMARK_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cmark)

# --- nlohmann/json ---
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# --- System libraries ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# --- Generate Wayland protocol bindings ---
pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WL_SCANNER wayland-scanner REQUIRED)

set(PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_DIR})

macro(wl_proto name xml)
    add_custom_command(OUTPUT ${PROTO_DIR}/${name}.h
        COMMAND ${WL_SCANNER} client-header ${xml} ${PROTO_DIR}/${name}.h DEPENDS ${xml})
    add_custom_command(OUTPUT ${PROTO_DIR}/${name}.c
        COMMAND ${WL_SCANNER} private-code ${xml} ${PROTO_DIR}/${name}.c DEPENDS ${xml})
endmacro()

wl_proto(xdg-shell-protocol "${WL_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
wl_proto(viewporter-protocol "${WL_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
wl_proto(fractional-scale-protocol "${WL_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")

# --- Executable ---
add_executable(fastcode-native
    src/main.cpp
    src/app.cpp
    src/wayland_window.cpp
    src/font.cpp
    src/renderer.cpp
    src/scroll_view.cpp
    src/markdown_renderer.cpp
    src/widgets.cpp
    src/curl_http.cpp
    src/keychain.cpp
    src/fake_data.cpp
    ${PROTO_DIR}/xdg-shell-protocol.c
    ${PROTO_DIR}/xdg-shell-protocol.h
    ${PROTO_DIR}/viewporter-protocol.c
    ${PROTO_DIR}/viewporter-protocol.h
    ${PROTO_DIR}/fractional-scale-protocol.c
    ${PROTO_DIR}/fractional-scale-protocol.h
)

target_include_directories(fastcode-native PRIVATE
    ${PROTO_DIR}
    ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/src
    src
)

target_link_libraries(fastcode-native PRIVATE
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::WAYLAND_CURSOR
    PkgConfig::XKBCOMMON
    PkgConfig::FREETYPE
    PkgConfig::HARFBUZZ
    PkgConfig::FONTCONFIG
    PkgConfig::LIBCURL
    PkgConfig::LIBSECRET
    nlohmann_json::nlohmann_json
    cmark
    rt
)

# Warnings
target_compile_options(fastcode-native PRIVATE -Wall -Wextra -Wpedantic)

# LTO for Release
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fastcode-native PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
