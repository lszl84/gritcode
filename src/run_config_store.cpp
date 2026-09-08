#include "run_config_store.h"
#include "app_paths.h"
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// Normalize a cwd string so that equivalent paths compare equal:
// expands a leading ~, resolves symlinks and ".." (canonical when the path
// exists, weakly_canonical otherwise), and drops any trailing separator.
// Returns "" when the input is empty after trimming.
std::string NormalizeCwd(const std::string& in) {
    std::string p = in;
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
    if (p.empty()) return p;

    if (p == "~") {
        if (const char* h = std::getenv("HOME")) p = h;
    } else if (p.rfind("~/", 0) == 0) {
        if (const char* h = std::getenv("HOME")) p = std::string(h) + p.substr(1);
    }

    std::error_code ec;
    std::filesystem::path norm = std::filesystem::canonical(p, ec);
    if (ec) {
        ec.clear();
        norm = std::filesystem::weakly_canonical(p, ec);
        if (ec) norm = std::filesystem::path(p).lexically_normal();
    }

    std::string s = norm.string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// True when `path` lives inside `dir` (a proper descendant, not equal).
bool IsDescendant(const std::string& path, const std::string& dir) {
    if (path.size() <= dir.size()) return false;
    if (path.compare(0, dir.size(), dir) != 0) return false;
    return path[dir.size()] == '/';
}

}  // namespace

std::string RunConfigStore::StoragePath() {
    return app_paths::AppDataDir() + "/run_configs.json";
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
            if (c.command.empty()) continue;
            std::string norm = NormalizeCwd(cwd);
            if (!norm.empty()) map[norm] = c;  // last duplicate wins
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
    std::string norm = NormalizeCwd(cwd);
    auto it = map.find(norm);
    if (it != map.end()) return it->second;
    return std::nullopt;
}

std::optional<RunConfigStore::Config> RunConfigStore::GetBest(const std::string& cwd) {
    auto map = Load();
    std::string norm = NormalizeCwd(cwd);

    // Exact project match first.
    if (auto it = map.find(norm); it != map.end()) return it->second;

    // The session root may be a parent of the actual project (e.g. a session
    // rooted at ~/Templates with the project in ~/Templates/myapp). Pick the
    // most recently used config for a project inside the session folder.
    const Config* best = nullptr;
    for (auto& [k, c] : map) {
        if (!IsDescendant(k, norm)) continue;
        if (!best || c.lastUsed > best->lastUsed) best = &c;
    }
    if (best) return *best;
    return std::nullopt;
}

void RunConfigStore::Set(const std::string& cwd, const std::string& command,
                         const std::string& discoveredBy) {
    auto map = Load();
    std::string norm = NormalizeCwd(cwd);
    if (norm.empty()) return;
    Config c;
    c.command = command;
    c.discoveredBy = discoveredBy;
    // ISO-8601 with local time.
    time_t now = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    c.lastUsed = buf;
    map[norm] = c;
    Save(map);
}

void RunConfigStore::Forget(const std::string& cwd) {
    auto map = Load();
    std::string norm = NormalizeCwd(cwd);
    map.erase(norm);
    Save(map);
}
