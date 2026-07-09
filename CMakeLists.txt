cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(gritcode VERSION 0.1.4 LANGUAGES CXX)

# Human-facing version string (used for packaging, bundle metadata, etc.)
set(GRITCODE_VERSION "${PROJECT_VERSION}-alpha")

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

# ---- wxWidgets ----
# macOS: always build static from source — portable .app with no Homebrew dep.
# Linux: try system package first, fall back to FetchContent.
if(APPLE)
    message(STATUS "macOS: building wxWidgets from source (static)")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(wxBUILD_SHARED OFF)
    FetchContent_Declare(
        wxWidgets
        GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
        GIT_TAG v3.2.8
        GIT_SHALLOW ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(wxWidgets)
    set(WX_LIBS wxcore)
else()
    find_package(wxWidgets QUIET COMPONENTS core base)
    if(NOT wxWidgets_FOUND)
        message(STATUS "wxWidgets not found — building from source (FetchContent)")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
        set(wxBUILD_SHARED OFF)
        FetchContent_Declare(
            wxWidgets
            GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
            GIT_TAG v3.2.8
            GIT_SHALLOW ON
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(wxWidgets)
        set(WX_LIBS wxcore)
    else()
        message(STATUS "Using system wxWidgets ${wxWidgets_VERSION_STRING}")
        include(${wxWidgets_USE_FILE})
        set(WX_LIBS ${wxWidgets_LIBRARIES})
    endif()
endif()

# libcurl backs StreamingWebRequest. We use it directly — wxnet/wxWebRequest
# is no longer linked.
find_package(CURL REQUIRED)

# SQLite FTS5 backs the cross-project memory index (memory.cpp).
# macOS ships SQLite in the SDK (use find_package). Linux uses pkg-config.
if(APPLE)
    find_package(SQLite3 REQUIRED)
    set(SQLITE_LIB SQLite::SQLite3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    set(SQLITE_LIB PkgConfig::SQLITE3)
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
    src/mcp_stdio.cpp
    src/memory.cpp
    src/session_store.cpp
    src/shell_env.cpp
    src/preferences.cpp
    src/settings_dialog.cpp
    src/streaming_web_request.cpp
    src/run_config_store.cpp
)

if(APPLE)
    # Copy each asset SVG into Contents/Resources/icons inside the .app bundle.
    # wxStandardPaths::GetResourcesDir() resolves to that path on macOS, so the
    # runtime asset loader works without any extra logic.
    file(GLOB ASSET_ICON_FILES "${CMAKE_SOURCE_DIR}/assets/icons/*.svg")
    set_source_files_properties(${ASSET_ICON_FILES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/icons"
    )

    # Compile the app icon from the iconset into a single .icns at build time.
    set(GRITCODE_ICNS ${CMAKE_CURRENT_BINARY_DIR}/gritcode.icns)
    file(GLOB GRITCODE_ICONSET_PNGS
         ${CMAKE_CURRENT_SOURCE_DIR}/packaging/gritcode.iconset/*.png)
    add_custom_command(
        OUTPUT ${GRITCODE_ICNS}
        COMMAND iconutil -c icns
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/gritcode.iconset
                -o ${GRITCODE_ICNS}
        DEPENDS ${GRITCODE_ICONSET_PNGS}
        COMMENT "Generating gritcode.icns")
    set_source_files_properties(${GRITCODE_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    add_executable(gritcode MACOSX_BUNDLE ${SRCS} ${ASSET_ICON_FILES} ${GRITCODE_ICNS})

    set_target_properties(gritcode PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME       "gritcode"
        MACOSX_BUNDLE_GUI_IDENTIFIER    "com.gritcode.app"
        MACOSX_BUNDLE_BUNDLE_VERSION    "${GRITCODE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${GRITCODE_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT         "Copyright (c) Gritcode contributors"
        MACOSX_BUNDLE_ICON_FILE         "gritcode.icns"
    )
else()
    add_executable(gritcode ${SRCS})
endif()

target_link_libraries(gritcode PRIVATE
    ${WX_LIBS}
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${SQLITE_LIB}
)

# On GTK builds we touch GtkSettings directly to disable theme animations on
# the canvas. This is the smallest GTK dep we can take.
if(UNIX AND NOT APPLE)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(gritcode PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(gritcode PRIVATE ${GTK3_LIBRARIES})

    # libsecret backs the API-key keyring fallback when wxUSE_SECRETSTORE is 0
    # (the system wx package on Ubuntu 24.04 is built without it).
    pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
    target_include_directories(gritcode PRIVATE ${LIBSECRET_INCLUDE_DIRS})
    target_link_libraries(gritcode PRIVATE ${LIBSECRET_LIBRARIES})
endif()

target_compile_options(gritcode PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Source-tree asset path baked in for dev builds. At runtime the loader prefers
# wxStandardPaths::GetResourcesDir() (which derives <prefix>/share/gritcode
# from the executable's actual location) and falls back to this if that path
# isn't populated — i.e., when running straight out of the build tree.
target_compile_definitions(gritcode PRIVATE
    GRITCODE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)

include(GNUInstallDirs)

# ---- Install rules ----
# POSIX layout:
#   <prefix>/bin/gritcode
#   <prefix>/share/gritcode/icons/*.svg                       (toolbar SVGs)
#   <prefix>/share/applications/gritcode.desktop              (launcher)
#   <prefix>/share/icons/hicolor/scalable/apps/gritcode.svg   (app icon)
install(TARGETS gritcode
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE  DESTINATION .
)

if(UNIX AND NOT APPLE)
    install(DIRECTORY assets/icons
        DESTINATION ${CMAKE_INSTALL_DATADIR}/gritcode
        FILES_MATCHING PATTERN "*.svg"
    )
    install(FILES packaging/gritcode.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES packaging/gritcode.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )
    install(FILES packaging/gritcode.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )

    # GTK/freedesktop icon loaders skip an icon theme directory that lacks an
    # index.theme file. System packages (hicolor-icon-theme) ship one to
    # /usr/share/icons/hicolor/, but a fresh per-user prefix (~/.local) won't
    # have one, which makes our installed app icon invisible to launchers.
    # Install our minimal index.theme only when the destination doesn't already
    # have one, so we never clobber a system-managed file.
    install(CODE "
        set(_dest \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor/index.theme\")
        if(NOT EXISTS \"\${_dest}\")
            file(INSTALL \"${CMAKE_SOURCE_DIR}/packaging/hicolor-index.theme\"
                DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor\"
                RENAME index.theme
            )
        else()
            message(STATUS \"hicolor index.theme already present at \${_dest} — skipping\")
        endif()
    ")
endif()

# ---- CPack ----
set(CPACK_PACKAGE_NAME "gritcode")
set(CPACK_PACKAGE_VERSION "${GRITCODE_VERSION}")
set(CPACK_PACKAGE_VENDOR "DevMindscape")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native desktop AI coding assistant")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/lszl84/gritcode")
set(CPACK_PACKAGE_CONTACT "luke@devmindscape.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "gritcode")
set(CPACK_STRIP_FILES ON)

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "gritcode")
else()
    # Linux: DEB only.
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "DevMindscape <luke@devmindscape.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
        " Native desktop AI coding assistant — no Electron, no browser tab.
 Streaming markdown, multi-session history, bash tool execution,
 one-click Play button that builds and runs your project without
 an AI roundtrip.
 .
 Built with C++20 and wxWidgets. Uses ~10 MB on disk and ~50 MB
 RAM at rest, vs hundreds of MB for Electron alternatives.
 Supports DeepSeek and the free OpenCode Zen tier.")
endif()

include(CPack)
