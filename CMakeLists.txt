cmake_minimum_required(VERSION 3.24)
project(main VERSION 1.0.0 LANGUAGES CXX)

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use Ninja for faster builds
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(WARNING "Consider using Ninja generator: cmake -G Ninja")
endif()

include(FetchContent)

set(wxBUILD_SHARED OFF)

message(STATUS "Fetching wxWidgets...")

FetchContent_Declare(
  wxWidgets
  GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
  GIT_TAG v3.2.10
  GIT_SHALLOW ON
  PATCH_COMMAND git apply --check "${CMAKE_CURRENT_SOURCE_DIR}/patches/wx-overlay-scrollbar-fix.patch" && git apply "${CMAKE_CURRENT_SOURCE_DIR}/patches/wx-overlay-scrollbar-fix.patch" || true
)
FetchContent_MakeAvailable(wxWidgets)

# Fetch nlohmann/json for JSON parsing
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(json)

# Fetch cmark for markdown parsing
message(STATUS "Fetching cmark...")
set(CMARK_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE) 
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    cmark
    GIT_REPOSITORY https://github.com/commonmark/cmark.git
    GIT_TAG 0.31.2
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(cmark)

# Source files - explicitly listed (no GLOB)
set(SOURCES
  application.cpp
  application.h
  chat_provider.h
  zen_provider.cpp
  zen_provider.h
  claude_provider.cpp
  claude_provider.h
  http_client.cpp
  http_client.h
  zen_client.cpp
  zen_client.h
  main_frame.cpp
  main_frame.h
  block_manager.cpp
  block_manager.h
  streaming_text_ctrl.cpp
  streaming_text_ctrl.h
  markdown_renderer.cpp
  markdown_renderer.h
  mcp_server.cpp
  mcp_server.h
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories for cmark
target_include_directories(${PROJECT_NAME} PRIVATE
    ${cmark_SOURCE_DIR}/src
    ${cmark_BINARY_DIR}/src
)

# Link wxWidgets and libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
  wxcore
  wxnet
  nlohmann_json::nlohmann_json
  cmark
)

# Platform-specific settings
if(WIN32)
  set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE TRUE)
endif()

# Compiler warnings
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
