cmake_minimum_required(VERSION 3.24)
project(zencode VERSION 1.0.0 LANGUAGES CXX)

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
  GIT_SHALLOW ON
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

# Source files - explicitly listed (no GLOB)
set(SOURCES
  src/core/application.cpp
  src/core/application.h
  src/network/http_client.cpp
  src/network/http_client.h
  src/network/zen_client.cpp
  src/network/zen_client.h
  src/ui/main_frame.cpp
  src/ui/main_frame.h
  src/mcp/mcp_server.cpp
  src/mcp/mcp_server.h
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src)

# Link wxWidgets and libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
  wxcore
  wxnet
  nlohmann_json::nlohmann_json
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
