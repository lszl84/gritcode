cmake_minimum_required(VERSION 3.24)
project(fastcode-native CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# --- Dear ImGui ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(imgui)

# Build ImGui as a library
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
# ImGui needs GL function declarations
target_compile_definitions(imgui_lib PUBLIC GL_GLEXT_PROTOTYPES)

# --- System libraries ---
find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

target_link_libraries(imgui_lib PUBLIC PkgConfig::GLFW OpenGL::GL)

# --- Executable ---
add_executable(fastcode-native
    src/main.cpp
    src/app.cpp
    src/glfw_window.cpp
    src/gl_renderer.cpp
    src/font.cpp
    src/scroll_view.cpp
    src/markdown_renderer.cpp
    src/curl_http.cpp
    src/keychain.cpp
    src/fake_data.cpp
)

target_include_directories(fastcode-native PRIVATE
    ${cmark_BINARY_DIR}/src
    ${cmark_SOURCE_DIR}/src
    src
)

target_link_libraries(fastcode-native PRIVATE
    imgui_lib
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

target_compile_options(fastcode-native PRIVATE -Wall -Wextra -Wpedantic)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET fastcode-native PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
