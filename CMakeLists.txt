cmake_minimum_required(VERSION 3.24)
project(grit VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version string from git state.
# Tagged commit:        v0.1.0
# After tag:            v0.1.0-14-g3a7f2c1
# Uncommitted changes:  v0.1.0-14-g3a7f2c1-dirty
# No tags yet:          0.1.0-g3a7f2c1 or 0.1.0-g3a7f2c1-dirty
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
# If git describe returned a tag-based version (starts with v), use it directly.
# Otherwise (no tags), prepend the project version.
if(GIT_DESCRIBE MATCHES "^v")
    string(SUBSTRING "${GIT_DESCRIBE}" 1 -1 VERSION_STRING)
else()
    set(VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.0-${GIT_DESCRIBE}")
endif()
if(NOT GIT_DESCRIBE)
    set(VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.0")
endif()
message(STATUS "Version: ${VERSION_STRING}")

include(FetchContent)

# --- cmark-gfm (drop-in superset: adds tables, strikethrough, task lists) ---
FetchContent_Declare(cmark-gfm
    GIT_REPOSITORY https://github.com/github/cmark-gfm.git
    GIT_TAG 0.29.0.gfm.13
    GIT_SHALLOW ON
)
set(CMARK_GFM_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_GFM_STATIC ON CACHE BOOL "" FORCE)
set(CMARK_GFM_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cmark-gfm)

# --- nlohmann/json ---
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# --- Platform dependencies ---
find_package(OpenGL REQUIRED)

# --- SDL3 (all platforms) ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.18
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(sdl3)

if(LINUX)
    # Linux: use system packages
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

    # Generate xdg-shell and xdg-decoration protocol headers for SSD detection
    find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
    set(WL_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
    file(MAKE_DIRECTORY "${WL_PROTO_DIR}")

    macro(WaylandProtocolGen _XML _PROTO)
        set(_H "${WL_PROTO_DIR}/${_PROTO}-client-protocol.h")
        set(_C "${WL_PROTO_DIR}/${_PROTO}-protocol.c")
        add_custom_command(OUTPUT "${_H}"
            DEPENDS "${_XML}"
            COMMAND "${WAYLAND_SCANNER}" ARGS client-header "${_XML}" "${_H}")
        add_custom_command(OUTPUT "${_C}"
            DEPENDS "${_H}"
            COMMAND "${WAYLAND_SCANNER}" ARGS private-code "${_XML}" "${_C}")
        target_sources(grit PRIVATE "${_C}")
    endmacro()
elseif(NOT LINUX)
    # macOS / Windows: build from source
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW ON
    )

    set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_CORETEXT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 10.4.0
        GIT_SHALLOW ON
    )

    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBRTMP OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_UNIX_SOCKETS OFF CACHE BOOL "" FORCE)
    set(ENABLE_LDAP OFF CACHE BOOL "" FORCE)
    set(ENABLE_LDAPS OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
    elseif(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    endif()
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_12_1
        GIT_SHALLOW ON
    )

    FetchContent_MakeAvailable(freetype harfbuzz curl)
endif()

# --- Executable ---
set(GRIT_SOURCES
    src/main.cpp
    src/app.cpp
    src/glfw_window.cpp
    src/gl_renderer.cpp
    src/font.cpp
    src/scroll_view.cpp
    src/markdown_renderer.cpp
    src/widgets.cpp
    src/curl_http.cpp
    src/keychain.cpp
    src/session.cpp
    src/clipboard.cpp
)

if(LINUX)
    set(GRIT_GTK4_SOURCES
        src/gtk4_main.cpp
        src/session.cpp
        src/curl_http.cpp
        src/keychain.cpp
        src/tool_exec.cpp
    )
    # Same agent-automation rule as the main grit target: Debug builds
    # ship the MCP server so scripts/mcp_client.py can drive the UI,
    # Release builds compile it out entirely.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND GRIT_GTK4_SOURCES src/mcp_server.cpp)
    endif()

    add_executable(grit-gtk4 ${GRIT_GTK4_SOURCES})
    target_include_directories(grit-gtk4 PRIVATE
        ${cmark-gfm_BINARY_DIR}/src
        ${cmark-gfm_SOURCE_DIR}/src
        ${cmark-gfm_SOURCE_DIR}/extensions
        ${cmark-gfm_BINARY_DIR}/extensions
    )
    target_link_libraries(grit-gtk4 PRIVATE
        PkgConfig::GTK4
        PkgConfig::LIBCURL
        PkgConfig::LIBSECRET
        nlohmann_json::nlohmann_json
        libcmark-gfm-extensions_static
        libcmark-gfm_static
    )
    target_compile_definitions(grit-gtk4 PRIVATE GRIT_LINUX)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_definitions(grit-gtk4 PRIVATE GRIT_ENABLE_MCP)
    endif()
    target_compile_options(grit-gtk4 PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MCP is the dev/agent automation channel — it lets scripts/mcp_client.py
# drive a running grit for testing. Release builds don't need it (and end
# users aren't expected to script their editor), so we compile it out of
# Release entirely: smaller binary, no listening TCP socket, no attack
# surface. Any non-Release config (Debug, RelWithDebInfo) keeps it in.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    list(APPEND GRIT_SOURCES src/mcp_server.cpp)
    set(GRIT_ENABLE_MCP TRUE)
endif()

if(APPLE)
    # Generate grit.icns from grit.svg using built-in sips + iconutil.
    set(GRIT_ICNS ${CMAKE_BINARY_DIR}/grit.icns)
    add_custom_command(
        OUTPUT ${GRIT_ICNS}
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen-icns.sh
                ${CMAKE_SOURCE_DIR}/grit.svg ${GRIT_ICNS}
        DEPENDS ${CMAKE_SOURCE_DIR}/grit.svg ${CMAKE_SOURCE_DIR}/scripts/gen-icns.sh
        COMMENT "Generating grit.icns from grit.svg"
    )
    set_source_files_properties(${GRIT_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND GRIT_SOURCES src/macos_chrome.mm ${GRIT_ICNS})

    add_executable(grit MACOSX_BUNDLE ${GRIT_SOURCES})

    set_target_properties(grit PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "Gritcode"
        MACOSX_BUNDLE_GUI_IDENTIFIER "ai.gritcode.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_STRING}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
        MACOSX_BUNDLE_ICON_FILE grit.icns
    )
else()
    add_executable(grit ${GRIT_SOURCES})
endif()

target_include_directories(grit PRIVATE
    ${cmark-gfm_BINARY_DIR}/src
    ${cmark-gfm_SOURCE_DIR}/src
    src
)

if(LINUX)
    target_include_directories(grit PRIVATE "${WL_PROTO_DIR}")
    WaylandProtocolGen(
        /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
        xdg-shell)
    WaylandProtocolGen(
        /usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
        xdg-decoration-unstable-v1)
endif()

target_link_libraries(grit PRIVATE
    OpenGL::GL
    SDL3::SDL3-static
    nlohmann_json::nlohmann_json
    libcmark-gfm_static
)

if(LINUX)
    target_link_libraries(grit PRIVATE
        PkgConfig::FREETYPE
        PkgConfig::HARFBUZZ
        PkgConfig::FONTCONFIG
        PkgConfig::LIBCURL
        PkgConfig::LIBSECRET
        PkgConfig::WAYLAND_CLIENT
    )
else()
    target_link_libraries(grit PRIVATE
        freetype
        harfbuzz
        CURL::libcurl
    )
    if(APPLE)
        target_link_libraries(grit PRIVATE
            "-framework Security"
            "-framework CoreText"
            "-framework CoreFoundation"
            "-framework Cocoa"
        )
    endif()
endif()

target_compile_definitions(grit PRIVATE
    GRIT_VERSION="${VERSION_STRING}"
)

if(GRIT_ENABLE_MCP)
    target_compile_definitions(grit PRIVATE GRIT_ENABLE_MCP)
endif()

if(LINUX)
    target_compile_definitions(grit PRIVATE GRIT_LINUX)
elseif(APPLE)
    target_compile_definitions(grit PRIVATE GRIT_MACOS)
elseif(WIN32)
    target_compile_definitions(grit PRIVATE GRIT_WINDOWS)
endif()

if(MSVC)
    target_compile_options(grit PRIVATE /W4)
else()
    target_compile_options(grit PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET grit PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Install
install(TARGETS grit DESTINATION bin)
if(LINUX)
    install(FILES grit.desktop DESTINATION share/applications)
    install(FILES grit.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()
