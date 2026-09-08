#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Cross-platform application storage paths. Everything gritcode persists
// lives under one of two directories, resolved here so the rest of the code
// never duplicates platform logic:
//
//   Linux      data:   $XDG_DATA_HOME/gritcode     (default ~/.local/share/gritcode)
//              config: $XDG_CONFIG_HOME/gritcode   (default ~/.config/gritcode)
//   macOS      data/config: ~/Library/Application Support/gritcode
//   Windows    data/config: %APPDATA%\gritcode
//
// MigrateLegacyLayout() also moves any files left behind by older public
// releases (v0.1.0 .. v0.5.0) into the current layout. See that function for
// the exact list of legacy locations.

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

namespace detail {

inline bool Exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && !ec;
}

inline bool IsDir(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec) && !ec;
}

// Move a single file from src to dst, but only if src exists and dst does
// not. Never overwrites existing data. Renames first (same volume); falls
// back to copy when the destination is on another filesystem. Returns true
// if dst exists afterwards.
inline bool MoveFile(const std::string& src, const std::string& dst) {
    std::error_code ec;
    if (!Exists(src) || Exists(dst)) return false;
    std::filesystem::create_directories(
        std::filesystem::path(dst).parent_path(), ec);
    ec.clear();
    std::filesystem::rename(src, dst, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(src, dst, ec);
    }
    return Exists(dst);
}

// Move each child of srcDir into dstDir unless a child of the same name
// already exists there. Child names are collected before any rename so the
// move doesn't invalidate the directory iterator.
inline void MergeDir(const std::string& srcDir, const std::string& dstDir) {
    std::error_code ec;
    if (!IsDir(srcDir)) return;
    std::filesystem::create_directories(dstDir, ec);

    std::vector<std::string> names;
    for (auto& e : std::filesystem::directory_iterator(srcDir, ec)) {
        if (ec) { ec.clear(); break; }
        names.push_back(e.path().filename().string());
    }
    for (const auto& name : names) {
        std::string src = srcDir + "/" + name;
        std::string dst = dstDir + "/" + name;
        if (Exists(dst)) continue;
        ec.clear();
        std::filesystem::rename(src, dst, ec);
        if (ec) {
            // Cross-device: copy the file; leave directories behind (moving a
            // whole tree across filesystems is rare and not worth the risk).
            ec.clear();
            if (!IsDir(src)) std::filesystem::copy_file(src, dst, ec);
        }
    }
}

}  // namespace detail

// One-time, idempotent migration of the on-disk layout used by older public
// releases into the current XDG / macOS layout. Runs before any config or
// data store opens, and never overwrites a file that already exists in the
// destination, so it is safe to call on every startup.
//
// Public release history (all "gritcode"; the pre-v0.1.0 "wx_gritcode" alphas
// were never shipped, so their ~/.wx_gritcode / ~/.config/wx_gritcode.conf
// paths are intentionally not migrated):
//   v0.1.0..v0.5.0  config      ~/.gritcode/gritcode.conf        (Linux)
//   v0.1.0..v0.5.0  config      ~/Library/Application Support/gritcode/gritcode.conf (macOS)
//   v0.1.0..now     sessions    ~/.local/share/gritcode          (Linux AND macOS)
//   v0.1.0..30a4aad run_configs ~/.gritcode/run_configs.json     (Linux)
//   v0.1.0..30a4aad run_configs ~/Library/Application Support/gritcode/run_configs.json (macOS)
inline void MigrateLegacyLayout() {
    std::string home = HomeDir();
    std::string configDir = AppConfigDir();
    std::string dataDir = AppDataDir();

    // 1) Config file: move the legacy Linux dotfile into the XDG config dir.
    //    On macOS the legacy location is already AppConfigDir(), so this is a
    //    no-op there.
    detail::MoveFile(home + "/.gritcode/gritcode.conf",
                     configDir + "/gritcode.conf");

    // 2) run_configs.json: before commit 30a4aad it lived in
    //    ~/.gritcode/run_configs.json on Linux (wxStandardPaths::GetUserDataDir).
    //    Recover it if it was orphaned there.
    detail::MoveFile(home + "/.gritcode/run_configs.json",
                     dataDir + "/run_configs.json");

#ifdef __APPLE__
    // 3) macOS data: sessions/memory/images used to live in
    //    ~/.local/share/gritcode (the Linux convention). Merge them into
    //    ~/Library/Application Support/gritcode, which already holds the
    //    config file. Merge, don't rename the whole dir, so we don't clobber
    //    the existing gritcode.conf.
    detail::MergeDir(home + "/.local/share/gritcode", dataDir);
#endif

    // Best-effort cleanup: remove ~/.gritcode if it's now empty. Fails
    // silently (error_code) if anything is left in it.
    {
        std::error_code ec;
        std::filesystem::remove(home + "/.gritcode", ec);
    }
}

}  // namespace app_paths
