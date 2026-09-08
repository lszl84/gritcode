#pragma once
#include <filesystem>
#include <string>

// Cross-platform application storage paths. Everything gritcode persists
// lives under one of two directories, resolved here so the rest of the code
// never duplicates platform logic:
//
//   Linux      data:   $XDG_DATA_HOME/gritcode     (default ~/.local/share/gritcode)
//              config: $XDG_CONFIG_HOME/gritcode   (default ~/.config/gritcode)
//   macOS      data/config: ~/Library/Application Support/gritcode
//   Windows    data/config: %APPDATA%\gritcode
//
// The legacy pre-XDG config dir (~/.gritcode on Unix) is referenced only so
// MigrateLegacyLayout() can move an existing config file forward.

namespace app_paths {

inline std::string HomeDir() {
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE")) {
        if (*home) return home;
    }
    if (const char* d = std::getenv("APPDATA")) return d;  // best effort
    return ".";
#else
    if (const char* home = std::getenv("HOME")) {
        if (*home) return home;
    }
    return "/tmp";
#endif
}

inline std::string AppDataDir() {
#ifdef __APPLE__
    return HomeDir() + "/Library/Application Support/gritcode";
#elif defined(_WIN32)
    if (const char* d = std::getenv("APPDATA")) {
        if (*d) return std::string(d) + "\\gritcode";
    }
    return HomeDir() + "\\AppData\\Roaming\\gritcode";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (*xdg) return std::string(xdg) + "/gritcode";
    }
    return HomeDir() + "/.local/share/gritcode";
#endif
}

inline std::string AppConfigDir() {
#ifdef __APPLE__
    // Keep the config file alongside data in Application Support — the
    // conventional single-app-directory layout for cross-platform apps on
    // macOS (as opposed to ~/.config, which is a Linux convention).
    return HomeDir() + "/Library/Application Support/gritcode";
#elif defined(_WIN32)
    if (const char* d = std::getenv("APPDATA")) {
        if (*d) return std::string(d) + "\\gritcode";
    }
    return HomeDir() + "\\AppData\\Roaming\\gritcode";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return std::string(xdg) + "/gritcode";
    }
    return HomeDir() + "/.config/gritcode";
#endif
}

// Where the pre-XDG config file used to live. Empty on Windows, which never
// used the ~/.gritcode convention.
inline std::string LegacyConfigDir() {
#ifdef _WIN32
    return std::string();
#else
    return HomeDir() + "/.gritcode";
#endif
}

// One-time, idempotent migration of the legacy on-disk layout into the
// current XDG / macOS layout. Runs before any config or data store opens.
inline void MigrateLegacyLayout() {
    std::error_code ec;

    // 1) Config: ~/.gritcode/gritcode.conf -> <config dir>/gritcode.conf.
    // Move (not copy) so the legacy dotfile doesn't linger after upgrade.
    std::string legacyDir = LegacyConfigDir();
    if (!legacyDir.empty()) {
        std::string src = legacyDir + "/gritcode.conf";
        std::string dst = AppConfigDir() + "/gritcode.conf";
        if (std::filesystem::exists(src, ec) &&
            !std::filesystem::exists(dst, ec)) {
            std::filesystem::create_directories(AppConfigDir(), ec);
            std::filesystem::rename(src, dst, ec);
            if (ec) {  // cross-device fallback: copy, leave source behind
                ec.clear();
                std::filesystem::copy_file(src, dst, ec);
            }
        }
    }

    // 2) macOS data: ~/.local/share/gritcode ->
    //    ~/Library/Application Support/gritcode.
#ifdef __APPLE__
    {
        std::string legacyData = HomeDir() + "/.local/share/gritcode";
        std::string newData = AppDataDir();
        if (std::filesystem::is_directory(legacyData, ec) &&
            !std::filesystem::exists(newData, ec)) {
            std::filesystem::create_directories(
                std::filesystem::path(newData).parent_path(), ec);
            std::filesystem::rename(legacyData, newData, ec);
        }
    }
#endif
}

}  // namespace app_paths
