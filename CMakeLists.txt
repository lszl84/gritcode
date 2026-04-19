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

# --- cmark ---
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

# --- Platform dependencies ---
find_package(OpenGL REQUIRED)

if(LINUX)
    # Native Wayland + X11 backends (no GLFW dependency)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL IMPORTED_TARGET wayland-egl)
    pkg_check_modules(WAYLAND_CURSOR IMPORTED_TARGET wayland-cursor)
    pkg_check_modules(X11 IMPORTED_TARGET x11)
    pkg_check_modules(XEXT IMPORTED_TARGET xext)
    pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

    find_program(WAYLAND_SCANNER wayland-scanner)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

    set(PROTOCOLS
        unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
        stable/xdg-shell/xdg-shell.xml
    )

    foreach(PROTO ${PROTOCOLS})
        get_filename_component(BASE ${PROTO} NAME_WE)
        set(XML ${WAYLAND_PROTOCOLS_DIR}/${PROTO})
        set(HEADER ${PROTO_GEN_DIR}/${BASE}-client-protocol.h)
        set(SOURCE ${PROTO_GEN_DIR}/${BASE}-protocol.c)
        add_custom_command(OUTPUT ${HEADER}
            COMMAND ${WAYLAND_SCANNER} client-header ${XML} ${HEADER}
            DEPENDS ${XML} VERBATIM)
        add_custom_command(OUTPUT ${SOURCE}
            COMMAND ${WAYLAND_SCANNER} private-code ${XML} ${SOURCE}
            DEPENDS ${XML} VERBATIM)
        list(APPEND PROTO_GENERATED ${HEADER} ${SOURCE})
    endforeach()

else()
    # macOS / Windows: build from source
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW ON
    )

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

    FetchContent_MakeAvailable(glfw freetype harfbuzz curl)
endif()

# --- Executable ---
set(GRIT_SOURCES
    src/main.cpp
    src/app.cpp
    src/window.cpp
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
    list(APPEND GRIT_SOURCES ${PROTO_GENERATED})
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
    ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/src
    src
    ${PROTO_GEN_DIR}
)

target_link_libraries(grit PRIVATE
    OpenGL::GL
    nlohmann_json::nlohmann_json
    cmark
)

if(LINUX)
    target_link_libraries(grit PRIVATE
        PkgConfig::EGL
        PkgConfig::FREETYPE
        PkgConfig::HARFBUZZ
        PkgConfig::FONTCONFIG
        PkgConfig::LIBCURL
        PkgConfig::LIBSECRET
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::WAYLAND_EGL
        PkgConfig::WAYLAND_CURSOR
        PkgConfig::X11
        PkgConfig::XEXT
        PkgConfig::XKBCOMMON
        OpenGL::GL
    )
else()
    target_link_libraries(grit PRIVATE
        glfw
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
    target_compile_definitions(grit PRIVATE
        GRIT_LINUX
        HAVE_WAYLAND=1
        HAVE_X11=1
    )
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

# --- Native Wayland/X11 backend (smooth resize, no GLFW) ---
# Build alongside grit as a separate target.
# Wayland + X11 backends adapted from opengl-smooth.
if(LINUX)
    find_program(WAYLAND_SCANNER wayland-scanner)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

    set(PROTOCOLS
        unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
        stable/xdg-shell/xdg-shell.xml
    )

    foreach(PROTO ${PROTOCOLS})
        get_filename_component(BASE ${PROTO} NAME_WE)
        set(XML ${WAYLAND_PROTOCOLS_DIR}/${PROTO})
        set(HEADER ${PROTO_GEN_DIR}/${BASE}-client-protocol.h)
        set(SOURCE ${PROTO_GEN_DIR}/${BASE}-protocol.c)
        add_custom_command(OUTPUT ${HEADER}
            COMMAND ${WAYLAND_SCANNER} client-header ${XML} ${HEADER}
            DEPENDS ${XML} VERBATIM)
        add_custom_command(OUTPUT ${SOURCE}
            COMMAND ${WAYLAND_SCANNER} private-code ${XML} ${SOURCE}
            DEPENDS ${XML} VERBATIM)
        list(APPEND PROTO_GENERATED ${HEADER} ${SOURCE})
    endforeach()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)

    add_executable(grit-smooth
        src/smooth_main.cpp
        ${PROTO_GENERATED}
    )

    target_include_directories(grit-smooth PRIVATE ${PROTO_GEN_DIR})
    target_compile_definitions(grit-smooth PRIVATE HAVE_WAYLAND=1)
    target_link_libraries(grit-smooth PRIVATE
        PkgConfig::EGL
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::WAYLAND_EGL
        PkgConfig::WAYLAND_CURSOR
    )
    target_compile_options(grit-smooth PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(grit-smooth PRIVATE OpenGL::GL)
endif()
