#include "run_config_store.h"
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

std::string RunConfigStore::StoragePath() {
    // Match the DataRoot() convention from session_store.cpp:
    //   $XDG_DATA_HOME/gritcode  or  $HOME/.local/share/gritcode.
    // Don't use wxStandardPaths here — on some systems it resolves to
    // ~/.gritcode instead of ~/.local/share/gritcode, which puts the
    // run config in a different directory than sessions and memory.
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] != '\0') return std::string(xdg) + "/gritcode/run_configs.json";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.local/share/gritcode/run_configs.json";
    }
    return "gritcode_data/run_configs.json";
}

std::map<std::string, RunConfigStore::Config> RunConfigStore::Load() {
    std::map<std::string, Config> map;
    std::string path = StoragePath();
    std::ifstream f(path);
    if (!f.is_open()) return map;
    try {
        nlohmann::json j;
        f >> j;
        for (auto& [cwd, entry] : j.items()) {
            Config c;
            c.command = entry.value("command", "");
            c.discoveredBy = entry.value("discoveredBy", "model");
            c.lastUsed = entry.value("lastUsed", "");
            if (!c.command.empty()) map[cwd] = c;
        }
    } catch (...) {}
    return map;
}

void RunConfigStore::Save(const std::map<std::string, Config>& map) {
    std::string path = StoragePath();
    // Ensure parent directory exists.
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    nlohmann::json j;
    for (auto& [cwd, c] : map) {
        j[cwd] = {
            {"command", c.command},
            {"discoveredBy", c.discoveredBy},
            {"lastUsed", c.lastUsed}
        };
    }
    std::ofstream f(path);
    if (f.is_open()) f << j.dump(2);
}

std::optional<RunConfigStore::Config> RunConfigStore::Get(const std::string& cwd) {
    auto map = Load();
    auto it = map.find(cwd);
    if (it != map.end()) return it->second;
    return std::nullopt;
}

void RunConfigStore::Set(const std::string& cwd, const std::string& command,
                         const std::string& discoveredBy) {
    auto map = Load();
    Config c;
    c.command = command;
    c.discoveredBy = discoveredBy;
    // ISO-8601 with local time.
    time_t now = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    c.lastUsed = buf;
    map[cwd] = c;
    Save(map);
}

void RunConfigStore::Forget(const std::string& cwd) {
    auto map = Load();
    map.erase(cwd);
    Save(map);
}
