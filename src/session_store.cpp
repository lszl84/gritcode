#include "session_store.h"
#include "perf_log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>

namespace fs = std::filesystem;

namespace {
std::string DataRoot() {
#ifdef _WIN32
    // Windows: %APPDATA%\gritcode (matches wxFileConfig's GetUserDataDir()).
    if (const char* appdata = std::getenv("APPDATA")) {
        if (appdata[0] != '\0') return std::string(appdata) + "\\gritcode";
    }
    if (const char* profile = std::getenv("USERPROFILE")) {
        return std::string(profile) + "\\.gritcode";
    }
    return "gritcode";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] != '\0') return std::string(xdg) + "/gritcode";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.local/share/gritcode";
    }
    return ".gritcode";  // fallback (shouldn't happen on Linux/macOS)
#endif
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

std::string SessionStore::NewThreadId() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)rng());
    return std::string(buf, 16);
}

bool SessionStore::Load(const std::string& id,
                        std::vector<nlohmann::json>& outHistory) const {
    PERF_SCOPE("SessionStore::Load");
    std::string path = sessionsDir_ + "/" + id + ".json";
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

std::string SessionStore::NowIso() {
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

void SessionStore::Save(Entry e, const std::vector<nlohmann::json>& history) {
    PERF_SCOPE("SessionStore::Save");
    e.lastUsed = NowIso();
    WriteSessionFile(e, history);
    UpdateIndex(e);
}

void SessionStore::WriteSessionFile(const Entry& e,
                                    const std::vector<nlohmann::json>& history) const {
    nlohmann::json j;
    j["id"] = e.id;
    j["cwd"] = e.cwd;
    j["lastUsed"] = e.lastUsed;
    if (!e.title.empty()) j["title"] = e.title;
    if (!e.parent.empty()) j["parent"] = e.parent;
    j["messages"] = history;

    std::string body = j.dump(2, ' ', false,
                              nlohmann::json::error_handler_t::replace);
    AtomicWrite(sessionsDir_ + "/" + e.id + ".json", body);
}

void SessionStore::UpdateIndex(const Entry& e) {
    auto it = std::ranges::find(entries_, e.id, &Entry::id);
    if (it != entries_.end()) *it = e;
    else entries_.push_back(e);
    SortEntries();
    WriteIndex();
}

void SessionStore::SetLastActive(const std::string& cwd, const std::string& threadId) {
    lastActiveCwd_ = cwd;
    lastActiveThread_ = threadId;
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
    j["threads"] = nlohmann::json::array();
    for (const auto& e : entries_) {
        nlohmann::json row = {
            {"id", e.id},
            {"cwd", e.cwd},
            {"lastUsed", e.lastUsed},
        };
        if (!e.title.empty()) row["title"] = e.title;
        if (e.id == IdForCwd(e.cwd)) {
            j["sessions"].push_back(std::move(row));
        } else {
            if (!e.parent.empty()) row["parent"] = e.parent;
            j["threads"].push_back(std::move(row));
        }
    }
    if (lastActiveCwd_) j["lastActiveCwd"] = *lastActiveCwd_;
    if (lastActiveThread_) j["lastActiveThread"] = *lastActiveThread_;
    std::string body = j.dump(2, ' ', false,
                              nlohmann::json::error_handler_t::replace);
    AtomicWrite(indexPath_, body);
}

void SessionStore::ReadIndex() {
    entries_.clear();
    lastActiveCwd_.reset();
    lastActiveThread_.reset();

    auto addEntry = [this](const nlohmann::json& e) {
        if (!e.is_object()) return;
        Entry ent;
        ent.id = e.value("id", std::string{});
        ent.cwd = e.value("cwd", std::string{});
        ent.lastUsed = e.value("lastUsed", std::string{});
        ent.title = e.value("title", std::string{});
        ent.parent = e.value("parent", std::string{});
        if (ent.cwd.empty()) return;
        // Backfill id if older index lacks it.
        if (ent.id.empty()) ent.id = IdForCwd(ent.cwd);
        if (std::ranges::find(entries_, ent.id, &Entry::id) != entries_.end()) return;
        entries_.push_back(std::move(ent));
    };

    std::ifstream f(indexPath_);
    nlohmann::json j;
    if (f.good()) {
        try { f >> j; } catch (...) { j = nullptr; }
    }
    if (j.is_object()) {
        for (const char* key : {"sessions", "threads"}) {
            if (j.contains(key) && j[key].is_array())
                for (const auto& e : j[key]) addEntry(e);
        }
        if (j.contains("lastActiveCwd") && j["lastActiveCwd"].is_string())
            lastActiveCwd_ = j["lastActiveCwd"].get<std::string>();
        if (j.contains("lastActiveThread") && j["lastActiveThread"].is_string())
            lastActiveThread_ = j["lastActiveThread"].get<std::string>();
    }

    // Session files the index doesn't list: 0.5.x rewrites the index with
    // only `sessions`, dropping `threads`. Their headers (id, cwd, lastUsed,
    // title, parent) are enough; the messages are skipped while parsing.
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(sessionsDir_, ec)) {
        if (file.path().extension() != ".json") continue;
        const std::string id = file.path().stem().string();
        if (std::ranges::find(entries_, id, &Entry::id) != entries_.end()) continue;
        std::ifstream sf(file.path());
        auto skipMessages = [](int depth, nlohmann::json::parse_event_t ev,
                               nlohmann::json& v) {
            return !(depth == 1 && ev == nlohmann::json::parse_event_t::key
                     && v == "messages");
        };
        nlohmann::json header = nlohmann::json::parse(sf, skipMessages, false);
        if (header.is_object() && header.value("id", std::string{}) == id)
            addEntry(header);
    }
    SortEntries();
}
