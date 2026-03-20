#pragma once

#include <wx/event.h>
#include <wx/string.h>
#include <nlohmann/json.hpp>

#include <thread>
#include <mutex>
#include <atomic>
#include <optional>

namespace fastcode::mcp {

using json = nlohmann::json;

wxDECLARE_EVENT(MCP_STDIN_MESSAGE, wxThreadEvent);
wxDECLARE_EVENT(MCP_SEND_MESSAGE_REQUEST, wxCommandEvent);

// MCP Server implementing the Model Context Protocol over stdio.
// When stdin is a pipe (launched by an MCP client), reads JSON-RPC 2.0
// messages from stdin and writes responses to stdout.
class MCPServer : public wxEvtHandler {
public:
    static MCPServer& Instance();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

    // Event handlers from ZenClient (called from main thread)
    void OnMessageReceived(const wxString& message, int tokens);
    void OnConnected(const wxString& status);
    void OnDisconnected();
    void OnError(const wxString& error);
    void OnModelsLoaded();

private:
    MCPServer();
    ~MCPServer();

    MCPServer(const MCPServer&) = delete;
    MCPServer& operator=(const MCPServer&) = delete;

    // Stdin reader thread
    void StdinReaderThread();

    // Main thread message handler
    void OnStdinMessage(wxThreadEvent& event);

    // MCP protocol handlers
    void HandleMessage(const json& msg);
    void HandleInitialize(const json& id, const json& params);
    void HandleToolsList(const json& id);
    void HandleToolsCall(const json& id, const json& params);

    // Tool implementations
    json ToolGetStatus();
    json ToolGetModels();
    json ToolGetResponse();
    json ToolGetChatHistory();
    json ToolSetModel(const json& args);
    json ToolConnect(const json& args);
    json ToolDisconnect();

    // Output helpers
    void SendJsonRpc(const json& msg);
    void SendResult(const json& id, const json& result);
    void SendError(const json& id, int code, const std::string& message);

    // MCP content helpers
    static json TextContent(const std::string& text);
    static json ToolResult(const std::string& text, bool isError = false);

    // Thread management
    std::thread stdinThread_;
    std::atomic<bool> running_{false};
    std::mutex outputMutex_;

    // Pending async tool call (for send_message - response comes later)
    std::optional<json> pendingCallId_;

    // State tracking
    wxString lastResponse_;
    std::vector<wxString> chatHistory_;
    bool responseReceived_ = false;
    bool connected_ = false;
    wxString connectionStatus_;
    std::vector<std::pair<std::string, std::string>> availableModels_;
    bool modelsLoaded_ = false;
    bool mcpInitialized_ = false;
};

} // namespace fastcode::mcp
