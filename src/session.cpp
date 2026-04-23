// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "session.h"
#include "debug_log.h"
#include <fstream>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <functional>

namespace fs = std::filesystem;
using json = nlohmann::json;

SessionManager::SessionManager() {}

std::string SessionManager::DataDir() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.local/share/gritcode";
}

std::string SessionManager::IndexPath() {
    return DataDir() + "/sessions.json";
}

std::string SessionManager::SessionPath(const std::string& id) {
    return DataDir() + "/sessions/" + id + ".json";
}

std::string SessionManager::HashCwd(const std::string& cwd) {
    // Simple hash → 8 hex chars
    size_t h = std::hash<std::string>{}(cwd);
    char buf[17];
    snprintf(buf, sizeof(buf), "%016zx", h);
    return std::string(buf, 16);
}

std::string SessionManager::NowISO() {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

static json MessageToJson(const ChatMessage& m) {
    json j;
    j["role"] = m.role;
    j["content"] = m.content;
    if (!m.toolCalls.empty()) j["tool_calls"] = m.toolCalls;
    if (!m.toolCallId.empty()) j["tool_call_id"] = m.toolCallId;
    if (!m.reasoningContent.empty()) j["reasoningContent"] = m.reasoningContent;
    if (m.isSummary) j["isSummary"] = true;
    return j;
}

static ChatMessage MessageFromJson(const json& j) {
    ChatMessage m;
    m.role = j.value("role", "");
    m.content = j.value("content", "");
    if (j.contains("tool_calls")) m.toolCalls = j["tool_calls"].get<std::vector<json>>();
    if (j.contains("tool_call_id")) m.toolCallId = j["tool_call_id"].get<std::string>();
    if (j.contains("reasoningContent")) m.reasoningContent = j["reasoningContent"].get<std::string>();
    m.isSummary = j.value("isSummary", false);
    return m;
}

bool SessionManager::LoadForCwd(const std::string& cwd) {
    cwd_ = cwd;
    sessionId_ = HashCwd(cwd);

    // Always start from an empty slate. Previously the "no session file on
    // disk" branch early-returned without touching history_, which meant a
    // workspace switch to a fresh cwd left the previous session's messages
    // hanging in memory — the next Save() would then write them into the
    // new cwd's session file, corrupting it.
    history_.clear();
    provider_.clear();
    model_.clear();
    dirty_ = false;

    std::string path = SessionPath(sessionId_);
    if (!fs::exists(path)) {
        // Register new session in index so workspace dropdown can see it
        fs::create_directories(DataDir());
        UpdateIndex();
        return false;
    }

    try {
        std::ifstream f(path);
        json j = json::parse(f);

        provider_ = j.value("provider", "zen");
        model_ = j.value("model", "");

        if (j.contains("messages") && j["messages"].is_array()) {
            for (auto& msg : j["messages"]) {
                history_.push_back(MessageFromJson(msg));
            }
        }

        return !history_.empty();
    } catch (...) {
        return false;
    }
}

// Atomic write: dump to a temp file, then rename into place.
// If the app crashes during write, the original file is untouched.
static bool AtomicWriteJson(const std::string& path, const json& j) {
    std::string tmp = path + ".tmp";
    try {
        std::ofstream f(tmp);
        if (!f) return false;
        // error_handler_t::replace silently swaps invalid UTF-8 bytes
        // with the Unicode replacement character (U+FFFD) instead of
        // throwing type_error.316 and crashing the app.
        f << j.dump(2, ' ', false, json::error_handler_t::replace);
        if (!f.good()) { f.close(); fs::remove(tmp); return false; }
        f.close();
        fs::rename(tmp, path);
        return true;
    } catch (...) {
        try { fs::remove(tmp); } catch (...) {}
        return false;
    }
}

void SessionManager::Save() {
    if (history_.empty() && !dirty_) return;

    // Ensure directories exist
    fs::create_directories(DataDir() + "/sessions");

    // Build JSON
    json j;
    j["cwd"] = cwd_;
    j["provider"] = provider_;
    j["model"] = model_;
    j["lastUsed"] = NowISO();

    json msgs = json::array();
    for (auto& m : history_) msgs.push_back(MessageToJson(m));
    j["messages"] = msgs;

    if (!AtomicWriteJson(SessionPath(sessionId_), j)) {
        DLOG("SessionManager::Save() FAILED for %s", sessionId_.c_str());
    }

    // Update index (also atomic)
    UpdateIndex();
    dirty_ = false;
}

void SessionManager::NewSession() {
    history_.clear();
    dirty_ = true;
    Save();  // Save empty session to update index
}

void SessionManager::UpdateIndex() {
    json index;

    // Load existing index
    if (fs::exists(IndexPath())) {
        try {
            std::ifstream f(IndexPath());
            index = json::parse(f);
        } catch (...) {
            index = json::object();
        }
    }

    // Update entry for this cwd
    index[cwd_] = {
        {"id", sessionId_},
        {"lastUsed", NowISO()},
        {"provider", provider_},
        {"model", model_},
        {"messageCount", (int)history_.size()}
    };

    // Atomic write so the index is never left empty/corrupted
    AtomicWriteJson(IndexPath(), index);
}

std::vector<SessionInfo> SessionManager::ListSessions() {
    std::vector<SessionInfo> result;

    if (!fs::exists(IndexPath())) return result;

    try {
        std::ifstream f(IndexPath());
        json index = json::parse(f);

        for (auto& [cwd, info] : index.items()) {
            SessionInfo si;
            si.id = info.value("id", "");
            si.cwd = cwd;
            si.lastUsed = info.value("lastUsed", "");
            si.provider = info.value("provider", "");
            si.model = info.value("model", "");
            si.messageCount = info.value("messageCount", 0);
            result.push_back(si);
        }
    } catch (...) {}

    return result;
}
