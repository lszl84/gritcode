#include "session_store.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

namespace {

std::string NowIso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string DataRoot() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] != '\0') return std::string(xdg) + "/wx_gritcode";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.local/share/wx_gritcode";
    }
    return ".wx_gritcode";  // fallback (shouldn't happen on Linux/macOS)
}

void AtomicWrite(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << content;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // rename can fail across mount points; copy + remove as fallback.
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

}  // namespace

std::string SessionStore::IdForCwd(const std::string& cwd) {
    // 16 hex chars derived from std::hash. Same approach as gritcode — a
    // cryptographic hash isn't needed; this is just a stable filesystem-safe
    // id derived from a path the user already trusts.
    size_t h = std::hash<std::string>{}(cwd);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf, 16);
}

void SessionStore::Init() {
    root_ = DataRoot();
    sessionsDir_ = root_ + "/sessions";
    indexPath_ = root_ + "/sessions.json";

    std::error_code ec;
    fs::create_directories(sessionsDir_, ec);
    ReadIndex();
}

bool SessionStore::Load(const std::string& cwd,
                        std::vector<nlohmann::json>& outHistory) const {
    std::string path = sessionsDir_ + "/" + IdForCwd(cwd) + ".json";
    std::ifstream f(path);
    if (!f.good()) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }
    if (!j.is_object() || !j.contains("messages") || !j["messages"].is_array())
        return false;

    outHistory.clear();
    outHistory.reserve(j["messages"].size());
    for (const auto& m : j["messages"]) outHistory.push_back(m);
    return true;
}

void SessionStore::Save(const std::string& cwd,
                        const std::vector<nlohmann::json>& history) {
    std::string id = IdForCwd(cwd);
    std::string lastUsed = NowIso();

    nlohmann::json j;
    j["id"] = id;
    j["cwd"] = cwd;
    j["lastUsed"] = lastUsed;
    j["messages"] = history;

    std::string body = j.dump(2, ' ', false,
                              nlohmann::json::error_handler_t::replace);
    AtomicWrite(sessionsDir_ + "/" + id + ".json", body);

    // Update or insert the index entry.
    bool found = false;
    for (auto& e : entries_) {
        if (e.cwd == cwd) {
            e.lastUsed = lastUsed;
            e.id = id;
            found = true;
            break;
        }
    }
    if (!found) entries_.push_back({id, cwd, lastUsed});
    SortEntries();
    WriteIndex();
}

void SessionStore::SetLastActiveCwd(const std::string& cwd) {
    lastActiveCwd_ = cwd;
    WriteIndex();
}

void SessionStore::RegisterCwd(const std::string& cwd) {
    std::string id = IdForCwd(cwd);
    std::string lastUsed = NowIso();
    bool found = false;
    for (auto& e : entries_) {
        if (e.cwd == cwd) {
            e.lastUsed = lastUsed;
            found = true;
            break;
        }
    }
    if (!found) entries_.push_back({id, cwd, lastUsed});
    SortEntries();
    WriteIndex();
}

void SessionStore::SortEntries() {
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) {
                  return a.lastUsed > b.lastUsed;  // ISO strings sort lexically
              });
}

void SessionStore::WriteIndex() const {
    nlohmann::json j;
    j["sessions"] = nlohmann::json::array();
    for (const auto& e : entries_) {
        j["sessions"].push_back({
            {"id", e.id},
            {"cwd", e.cwd},
            {"lastUsed", e.lastUsed},
        });
    }
    if (lastActiveCwd_) j["lastActiveCwd"] = *lastActiveCwd_;
    std::string body = j.dump(2, ' ', false,
                              nlohmann::json::error_handler_t::replace);
    AtomicWrite(indexPath_, body);
}

void SessionStore::ReadIndex() {
    entries_.clear();
    lastActiveCwd_.reset();

    std::ifstream f(indexPath_);
    if (!f.good()) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }
    if (!j.is_object()) return;

    if (j.contains("sessions") && j["sessions"].is_array()) {
        for (const auto& e : j["sessions"]) {
            if (!e.is_object()) continue;
            Entry ent;
            ent.id = e.value("id", std::string{});
            ent.cwd = e.value("cwd", std::string{});
            ent.lastUsed = e.value("lastUsed", std::string{});
            if (ent.cwd.empty()) continue;
            // Backfill id if older index lacks it.
            if (ent.id.empty()) ent.id = IdForCwd(ent.cwd);
            entries_.push_back(std::move(ent));
        }
    }
    if (j.contains("lastActiveCwd") && j["lastActiveCwd"].is_string()) {
        lastActiveCwd_ = j["lastActiveCwd"].get<std::string>();
    }
    SortEntries();
}
