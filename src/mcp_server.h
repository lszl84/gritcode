#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

// TCP JSON-RPC server for programmatic control of a running wx_gritcode
// instance. The server lives on a background thread; methods are dispatched
// via callbacks the GUI registers, and the callbacks themselves marshal
// onto the GUI thread via wxFrame::CallAfter when they need to mutate UI.
//
// Wire protocol: newline-delimited JSON-RPC 2.0.
// Default port: 8765. If unavailable, falls back to 8766..8770.
struct MCPCallbacks {
    // Returns a JSON status object. Should include at least
    // {streaming: bool, blocks: int}.
    std::function<nlohmann::json()> getStatus;

    // Returns the rendered conversation as an array of
    // {role, type, text} objects.
    std::function<nlohmann::json()> getConversation;

    // Returns {text: <last assistant message>}.
    std::function<nlohmann::json()> getLastAssistant;

    // Inject text as if the user typed it and pressed Enter. Returns
    // {sent: true} on success, or {sent: false, reason: "<why>"} if the
    // message can't be processed right now (e.g. a turn is already streaming).
    std::function<nlohmann::json(const std::string&)> sendMessage;

    // Cancel the in-flight web request (if any).
    std::function<void()> cancelRequest;

    // Returns a JSON array describing the rendered block list — type, sizes,
    // and per-type previews. Used to verify rendering programmatically.
    std::function<nlohmann::json()> getBlocks;

    // Toggle a ToolCall block's expanded state by index. Index out of range or
    // wrong type is silently ignored.
    std::function<void(int)> toggleTool;

    // Returns {sessions: [{id, cwd, lastUsed}, ...], activeCwd}.
    std::function<nlohmann::json()> listSessions;

    // Switch the active session to the given cwd (loads from disk + restores
    // canvas, or seeds a fresh one if the cwd has no saved session yet).
    // Returns {ok: bool, reason?: string}.
    std::function<nlohmann::json(const std::string&)> switchSession;

    // Create a fresh session for $HOME (default cwd) without prompting.
    // Interactive new-session flow goes through the directory dialog instead.
    std::function<nlohmann::json()> newSession;

    // Set the active model by dropdown index (0=OpenCode Free, 1=DeepSeek
    // Flash, 2=DeepSeek Pro). Persists via wxConfig. Returns {ok, modelIndex}.
    // Test hook for driving the provider switch programmatically.
    std::function<nlohmann::json(int)> setModel;

    // Returns {modelIndex, hasDeepseekKey} — non-secret preferences snapshot
    // useful for verifying persistence and key-presence in tests. The actual
    // key value is never returned over MCP.
    std::function<nlohmann::json()> getPreferences;
};

class MCPServer {
public:
    MCPServer();
    ~MCPServer();

    void Start(MCPCallbacks callbacks);
    void Stop();
    int Port() const { return port_; }

private:
    void ServerLoop();
    void HandleClient(int clientFd);
    nlohmann::json HandleRequest(const nlohmann::json& request);

    MCPCallbacks cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int serverFd_ = -1;
    int port_ = 0;
    std::mutex cbMutex_;
};
