cmake_minimum_required(VERSION 3.24)
project(fcn VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build number from git commit count
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BUILD_NUMBER)
    set(BUILD_NUMBER "0")
endif()

set(VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${BUILD_NUMBER}")
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
    # Linux: use system packages
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
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
if(APPLE)
    set(FCN_BUNDLE_FLAG MACOSX_BUNDLE)
else()
    set(FCN_BUNDLE_FLAG "")
endif()

add_executable(fcn ${FCN_BUNDLE_FLAG}
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
    src/mcp_server.cpp
)

if(APPLE)
    set_target_properties(fcn PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "FastCode Native"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.devmindscape.fastcode-native"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_STRING}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
    )
endif()

target_include_directories(fcn PRIVATE
    ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/src
    src
)

target_link_libraries(fcn PRIVATE
    OpenGL::GL
    nlohmann_json::nlohmann_json
    cmark
)

if(LINUX)
    target_link_libraries(fcn PRIVATE
        PkgConfig::GLFW
        PkgConfig::FREETYPE
        PkgConfig::HARFBUZZ
        PkgConfig::FONTCONFIG
        PkgConfig::LIBCURL
        PkgConfig::LIBSECRET
    )
else()
    target_link_libraries(fcn PRIVATE
        glfw
        freetype
        harfbuzz
        CURL::libcurl
    )
    if(APPLE)
        target_link_libraries(fcn PRIVATE
            "-framework Security"
            "-framework CoreText"
            "-framework CoreFoundation"
        )
    endif()
endif()

target_compile_definitions(fcn PRIVATE
    FCN_VERSION="${VERSION_STRING}"
)

if(LINUX)
    target_compile_definitions(fcn PRIVATE FCN_LINUX)
elseif(APPLE)
    target_compile_definitions(fcn PRIVATE FCN_MACOS)
elseif(WIN32)
    target_compile_definitions(fcn PRIVATE FCN_WINDOWS)
endif()

if(MSVC)
    target_compile_options(fcn PRIVATE /W4)
else()
    target_compile_options(fcn PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fcn PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Install
install(TARGETS fcn DESTINATION bin)
if(LINUX)
    install(FILES fcn.desktop DESTINATION share/applications)
    install(FILES fcn.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()
