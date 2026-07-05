#include "run_config_store.h"
#include <wx/string.h>
#include <wx/stdpaths.h>
#include <fstream>

namespace fs = std::filesystem;

std::string RunConfigStore::StoragePath() {
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    if (dir.IsEmpty()) {
        // Fallback for dev builds where wx can't determine the path.
        if (const char* home = std::getenv("HOME")) {
            dir = wxString(home) + "/.local/share/wx_gritcode";
        } else {
            dir = "./wx_gritcode_data";
        }
    }
    return (dir + "/run_configs.json").ToStdString(wxConvUTF8);
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
