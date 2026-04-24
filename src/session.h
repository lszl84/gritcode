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

#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct ChatMessage {
    std::string role;
    std::string content;
    std::vector<nlohmann::json> toolCalls;
    std::string toolCallId;
    std::string reasoningContent;  // Thinking/reasoning text from models like Kimi K2.5
    // True when this message replaces a prior head of history that was
    // summarized to fit the context window. Marker prevents the compactor
    // from picking this message as a split point and walking back through it.
    bool isSummary = false;
};

struct SessionInfo {
    std::string id;
    std::string cwd;
    std::string lastUsed;
    std::string provider;
    std::string model;
    int messageCount = 0;
};

class SessionManager {
public:
    SessionManager();

    // Load session for current working directory (returns true if found)
    bool LoadForCwd(const std::string& cwd);

    // Save current session
    void Save();

    // Start a new session (clears history)
    void NewSession();

    // Access history
    std::vector<ChatMessage>& History() { return history_; }
    const std::vector<ChatMessage>& History() const { return history_; }

    // Metadata
    void SetProvider(const std::string& p) { provider_ = p; dirty_ = true; }
    void SetModel(const std::string& m) { model_ = m; dirty_ = true; }
    void SetCwd(const std::string& c) { cwd_ = c; }
    const std::string& Provider() const { return provider_; }
    const std::string& Model() const { return model_; }
    const std::string& SessionId() const { return sessionId_; }
    const std::string& Cwd() const { return cwd_; }
    bool IsDirty() const { return dirty_; }
    void MarkDirty() { dirty_ = true; }

    // List all sessions from index
    static std::vector<SessionInfo> ListSessions();

private:
    std::string cwd_;
    std::string sessionId_;
    std::string provider_;
    std::string model_;
    std::vector<ChatMessage> history_;
    bool dirty_ = false;

    static std::string DataDir();
    static std::string IndexPath();
    static std::string SessionPath(const std::string& id);
    static std::string HashCwd(const std::string& cwd);
    static std::string NowISO();

    void UpdateIndex();
};
