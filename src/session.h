#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct ChatMessage {
    std::string role;
    std::string content;
    std::vector<nlohmann::json> toolCalls;
    std::string toolCallId;
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
