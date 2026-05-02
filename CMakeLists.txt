cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(wx_gritcode LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- nlohmann/json (always FetchContent) ----
message(STATUS "Fetching nlohmann/json...")
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- wxWidgets: try system first, fall back to FetchContent ----
find_package(wxWidgets QUIET COMPONENTS net core base)

if(NOT wxWidgets_FOUND)
    message(STATUS "wxWidgets not found in system — building from source (FetchContent)")

    # wxWidgets 3.2.6's CMakeLists declares cmake_minimum_required(VERSION 3.0),
    # which CMake 4 rejects. Override the policy floor for the fetched build.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

    set(wxBUILD_SHARED OFF)
    FetchContent_Declare(
        wxWidgets
        GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
        GIT_TAG v3.2.6
        GIT_SHALLOW ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(wxWidgets)

    # When built from source the targets are plain library names.
    set(WX_LIBS wxcore wxnet)

    # wxWidgets 3.2 built from source via CMake doesn't propagate libcurl
    # as a transitive link dep on Linux even though wxWebRequest uses it
    # at runtime. https://github.com/wxWidgets/wxWidgets/issues/22860
    if(UNIX AND NOT APPLE)
        find_package(CURL REQUIRED)
        list(APPEND WX_LIBS CURL::libcurl)
    endif()
else()
    message(STATUS "Using system wxWidgets ${wxWidgets_VERSION_STRING}")
    include(${wxWidgets_USE_FILE})
    # System wxWidgets provides ${wxWidgets_LIBRARIES} with full paths,
    # including transitive deps like libcurl resolved via wx-config.
    set(WX_LIBS ${wxWidgets_LIBRARIES})
endif()

# ---- Sources ----
set(SRCS
    src/main.cpp
    src/chat_frame.cpp
    src/chat_canvas.cpp
    src/md_parser.cpp
    src/inline_parser.cpp
    src/tools.cpp
    src/mcp_server.cpp
    src/session_store.cpp
    src/shell_env.cpp
    src/preferences.cpp
    src/settings_dialog.cpp
)

if(APPLE)
    # Copy each asset SVG into Contents/Resources/icons inside the .app bundle.
    # wxStandardPaths::GetResourcesDir() resolves to that path on macOS, so the
    # runtime asset loader works without any extra logic.
    file(GLOB ASSET_ICON_FILES "${CMAKE_SOURCE_DIR}/assets/icons/*.svg")
    set_source_files_properties(${ASSET_ICON_FILES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/icons"
    )
    add_executable(wx_gritcode MACOSX_BUNDLE ${SRCS} ${ASSET_ICON_FILES})

    set_target_properties(wx_gritcode PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME       "wx_gritcode"
        MACOSX_BUNDLE_GUI_IDENTIFIER    "com.gritcode.wx"
        MACOSX_BUNDLE_BUNDLE_VERSION    "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_COPYRIGHT         "Copyright (c) Gritcode contributors"
    )

    # Bundle the app icon if a .icns is present in packaging/. macOS doesn't
    # render SVG app icons, so we only wire this when the icns exists.
    if(EXISTS "${CMAKE_SOURCE_DIR}/packaging/wx_gritcode.icns")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/packaging/wx_gritcode.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(wx_gritcode PRIVATE
            "${CMAKE_SOURCE_DIR}/packaging/wx_gritcode.icns"
        )
        set_target_properties(wx_gritcode PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "wx_gritcode.icns"
        )
    endif()
elseif(WIN32)
    # Embed the Windows app icon if both packaging/wx_gritcode.rc and the .ico
    # it references are present. Without this the binary ships with the default
    # executable icon.
    set(WIN_RC  "${CMAKE_SOURCE_DIR}/packaging/wx_gritcode.rc")
    set(WIN_ICO "${CMAKE_SOURCE_DIR}/packaging/wx_gritcode.ico")
    if(EXISTS "${WIN_RC}" AND EXISTS "${WIN_ICO}")
        add_executable(wx_gritcode WIN32 ${SRCS} "${WIN_RC}")
    else()
        add_executable(wx_gritcode WIN32 ${SRCS})
    endif()
else()
    add_executable(wx_gritcode ${SRCS})
endif()

target_link_libraries(wx_gritcode PRIVATE
    ${WX_LIBS}
    nlohmann_json::nlohmann_json
)

# On GTK builds we touch GtkSettings directly to disable theme animations on
# the canvas. This is the smallest GTK dep we can take.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(wx_gritcode PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(wx_gritcode PRIVATE ${GTK3_LIBRARIES})
endif()

target_compile_options(wx_gritcode PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Source-tree asset path baked in for dev builds. At runtime the loader prefers
# wxStandardPaths::GetResourcesDir() (which derives <prefix>/share/wx_gritcode
# from the executable's actual location) and falls back to this if that path
# isn't populated — i.e., when running straight out of the build tree.
target_compile_definitions(wx_gritcode PRIVATE
    WXG_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)

include(GNUInstallDirs)

# ---- Install rules ----
# POSIX layout:
#   <prefix>/bin/wx_gritcode
#   <prefix>/share/wx_gritcode/icons/*.svg                       (toolbar SVGs)
#   <prefix>/share/applications/wx_gritcode.desktop              (launcher)
#   <prefix>/share/icons/hicolor/scalable/apps/wx_gritcode.svg   (app icon)
install(TARGETS wx_gritcode
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE  DESTINATION .
)

if(UNIX AND NOT APPLE)
    install(DIRECTORY assets/icons
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wx_gritcode
        FILES_MATCHING PATTERN "*.svg"
    )
    install(FILES packaging/wx_gritcode.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES packaging/wx_gritcode.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )
endif()
