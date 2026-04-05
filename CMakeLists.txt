cmake_minimum_required(VERSION 3.24)
project(fastcode-native VERSION 0.1.0 LANGUAGES CXX C)

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

# --- System libraries ---
find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# --- Executable ---
add_executable(fastcode-native
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
    src/fake_data.cpp
)

target_include_directories(fastcode-native PRIVATE
    ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/src
    src
)

target_link_libraries(fastcode-native PRIVATE
    PkgConfig::GLFW
    OpenGL::GL
    PkgConfig::FREETYPE
    PkgConfig::HARFBUZZ
    PkgConfig::FONTCONFIG
    PkgConfig::LIBCURL
    PkgConfig::LIBSECRET
    nlohmann_json::nlohmann_json
    cmark
)

target_compile_definitions(fastcode-native PRIVATE
    FCN_VERSION="${VERSION_STRING}"
)

target_compile_options(fastcode-native PRIVATE -Wall -Wextra -Wpedantic)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fastcode-native PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Install to ~/.local/bin/
install(TARGETS fastcode-native DESTINATION bin)
