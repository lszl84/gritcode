cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(wx_gritcode VERSION 0.0.2 LANGUAGES CXX)

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
find_package(wxWidgets QUIET COMPONENTS core base)

if(NOT wxWidgets_FOUND)
    message(STATUS "wxWidgets not found — building from source (FetchContent)")

    # wxWidgets 3.2.x's CMakeLists declares cmake_minimum_required < 3.5,
    # which CMake 4+ rejects. Override the policy floor for the fetched build.
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

# libcurl backs StreamingWebRequest. We use it directly — wxnet/wxWebRequest
# is no longer linked.
find_package(CURL REQUIRED)

# SQLite FTS5 backs the cross-project memory index (memory.cpp). Use the
# system package via pkg-config — every supported distro ships it, and we
# need FTS5 enabled (which the Linux system builds always have).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

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
    set(WXG_ICNS ${CMAKE_CURRENT_BINARY_DIR}/wx_gritcode.icns)
    file(GLOB WXG_ICONSET_PNGS
         ${CMAKE_CURRENT_SOURCE_DIR}/packaging/wx_gritcode.iconset/*.png)
    add_custom_command(
        OUTPUT ${WXG_ICNS}
        COMMAND iconutil -c icns
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/wx_gritcode.iconset
                -o ${WXG_ICNS}
        DEPENDS ${WXG_ICONSET_PNGS}
        COMMENT "Generating wx_gritcode.icns")
    set_source_files_properties(${WXG_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

    add_executable(wx_gritcode MACOSX_BUNDLE ${SRCS} ${ASSET_ICON_FILES} ${WXG_ICNS})

    set_target_properties(wx_gritcode PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME       "wx_gritcode"
        MACOSX_BUNDLE_GUI_IDENTIFIER    "com.gritcode.wx"
        MACOSX_BUNDLE_BUNDLE_VERSION    "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_COPYRIGHT         "Copyright (c) Gritcode contributors"
        MACOSX_BUNDLE_ICON_FILE         "wx_gritcode.icns"
    )
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
    CURL::libcurl
    PkgConfig::SQLITE3
)

# On GTK builds we touch GtkSettings directly to disable theme animations on
# the canvas. This is the smallest GTK dep we can take.
if(UNIX AND NOT APPLE)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(wx_gritcode PRIVATE ${GTK3_INCLUDE_DIRS})
    target_link_libraries(wx_gritcode PRIVATE ${GTK3_LIBRARIES})

    # libsecret backs the API-key keyring fallback when wxUSE_SECRETSTORE is 0
    # (the system wx package on Ubuntu 24.04 is built without it).
    pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
    target_include_directories(wx_gritcode PRIVATE ${LIBSECRET_INCLUDE_DIRS})
    target_link_libraries(wx_gritcode PRIVATE ${LIBSECRET_LIBRARIES})
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

if(WIN32)
    # Ship the .ico inside the install tree so the Start Menu shortcut can
    # reference it explicitly.
    install(FILES packaging/wx_gritcode.ico DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Icons go next to the exe — wxStandardPaths::GetResourcesDir returns
    # the exe directory on Windows.
    install(DIRECTORY assets/icons
        DESTINATION ${CMAKE_INSTALL_BINDIR}/icons
        FILES_MATCHING PATTERN "*.svg"
    )

    # Walk the installed .exe's PE imports and copy non-system runtime DLLs
    # into bin/ alongside the executable.
    if(DEFINED ENV{MINGW_PREFIX})
        set(_wxg_dll_search_dirs "$ENV{MINGW_PREFIX}/bin")
    else()
        set(_wxg_dll_search_dirs "")
    endif()
    install(CODE "
        cmake_policy(SET CMP0207 NEW)
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:wx_gritcode>\"
            RESOLVED_DEPENDENCIES_VAR _resolved
            UNRESOLVED_DEPENDENCIES_VAR _unresolved
            PRE_EXCLUDE_REGEXES \"api-ms-.*\" \"ext-ms-.*\"
            POST_EXCLUDE_REGEXES \".*[Ss]ystem32.*\" \".*WINDOWS.*\"
            DIRECTORIES \"${_wxg_dll_search_dirs}\"
        )
        foreach(_dep \${_resolved})
            file(INSTALL \"\${_dep}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
        endforeach()
        if(_unresolved)
            message(WARNING \"Unresolved runtime deps: \${_unresolved}\")
        endif()
    ")
endif()

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
set(CPACK_PACKAGE_NAME "wx_gritcode")
set(CPACK_PACKAGE_VENDOR "gritcode")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native wxWidgets AI coding assistant")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/lszl84/wx_gritcode")
set(CPACK_PACKAGE_CONTACT "luke@devmindscape.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "wx_gritcode")
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_MUI_ICON   "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wx_gritcode.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wx_gritcode.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\wx_gritcode.exe")
    set(CPACK_NSIS_DISPLAY_NAME "wx_gritcode")
    set(CPACK_NSIS_PACKAGE_NAME "wx_gritcode")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\wx_gritcode.lnk' '$INSTDIR\\\\bin\\\\wx_gritcode.exe' '' '$INSTDIR\\\\bin\\\\wx_gritcode.ico'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\wx_gritcode.lnk'")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "wx_gritcode")
else()
    # Linux: DEB + tarball.
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Lukasz Sromek <luke@devmindscape.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)
