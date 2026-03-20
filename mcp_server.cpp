#include "mcp_server.h"
#include "zen_client.h"
#include <wx/app.h>
#include <wx/window.h>
#include <wx/log.h>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace fcn::mcp {

wxDEFINE_EVENT(MCP_STDIN_MESSAGE, wxThreadEvent);
wxDEFINE_EVENT(MCP_SEND_MESSAGE_REQUEST, wxCommandEvent);

MCPServer::MCPServer() {
    Bind(MCP_STDIN_MESSAGE, &MCPServer::OnStdinMessage, this);
}

MCPServer::~MCPServer() {
    Stop();
}

MCPServer& MCPServer::Instance() {
    static MCPServer instance;
    return instance;
}

bool MCPServer::Start() {
    if (running_) return true;

    if (isatty(fileno(stdin))) {
        wxLogMessage("MCPServer: stdin is a terminal, MCP stdio server not started");
        return false;
    }

    running_ = true;
    stdinThread_ = std::thread(&MCPServer::StdinReaderThread, this);
    wxLogMessage("MCPServer: MCP stdio server started");
    return true;
}

void MCPServer::Stop() {
    if (!running_) return;
    running_ = false;

    // Can't join the thread - it may be blocking on stdin read.
    // Detach and let the OS clean it up on exit.
    if (stdinThread_.joinable()) {
        stdinThread_.detach();
    }

    wxLogMessage("MCPServer: Server stopped");
}

void MCPServer::StdinReaderThread() {
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        auto* event = new wxThreadEvent(MCP_STDIN_MESSAGE);
        event->SetString(wxString::FromUTF8(line));
        wxQueueEvent(this, event);
    }
    running_ = false;
}

void MCPServer::OnStdinMessage(wxThreadEvent& event) {
    std::string line = event.GetString().ToStdString();

    try {
        json msg = json::parse(line);
        HandleMessage(msg);
    } catch (const json::exception& e) {
        wxLogError("MCPServer: JSON parse error: %s", e.what());
        SendError(nullptr, -32700, std::string("Parse error: ") + e.what());
    }
}

void MCPServer::HandleMessage(const json& msg) {
    std::string method = msg.value("method", "");
    json id = msg.contains("id") ? msg["id"] : json(nullptr);
    json params = msg.value("params", json::object());

    // Notifications (no id) - just acknowledge
    if (!msg.contains("id")) {
        if (method == "notifications/initialized") {
            wxLogMessage("MCPServer: Client sent initialized notification");
        } else if (method == "notifications/cancelled") {
            wxLogMessage("MCPServer: Request cancelled by client");
            if (pendingCallId_.has_value()) {
                pendingCallId_.reset();
            }
        }
        return;
    }

    if (method == "initialize") {
        HandleInitialize(id, params);
    } else if (method == "tools/list") {
        HandleToolsList(id);
    } else if (method == "tools/call") {
        HandleToolsCall(id, params);
    } else if (method == "ping") {
        SendResult(id, json::object());
    } else {
        SendError(id, -32601, "Method not found: " + method);
    }
}

void MCPServer::HandleInitialize(const json& id, const json& /*params*/) {
    mcpInitialized_ = true;

    json result;
    result["protocolVersion"] = "2024-11-05";
    result["capabilities"]["tools"] = json::object();
    result["serverInfo"]["name"] = "fastcode-native";
    result["serverInfo"]["version"] = "1.0.0";

    SendResult(id, result);
}

void MCPServer::HandleToolsList(const json& id) {
    json tools = json::array();

    tools.push_back({
        {"name", "send_message"},
        {"description", "Send a message to the AI agent and wait for the response. "
                        "Returns the AI's reply once generation is complete."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"message", {{"type", "string"}, {"description", "The message to send to the AI"}}}
            }},
            {"required", json::array({"message"})}
        }}
    });

    tools.push_back({
        {"name", "get_status"},
        {"description", "Get the current connection status, model info, and chat state."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
    });

    tools.push_back({
        {"name", "get_models"},
        {"description", "List all available AI models with their IDs and names."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
    });

    tools.push_back({
        {"name", "get_response"},
        {"description", "Get the last AI response received."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
    });

    tools.push_back({
        {"name", "get_chat_history"},
        {"description", "Get the full chat history as a list of messages."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
    });

    tools.push_back({
        {"name", "set_model"},
        {"description", "Set the active AI model by ID."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"model_id", {{"type", "string"}, {"description", "The model ID (e.g. 'big-pickle')"}}}
            }},
            {"required", json::array({"model_id"})}
        }}
    });

    tools.push_back({
        {"name", "connect"},
        {"description", "Connect to the OpenCode Zen API. Anonymous access by default."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"api_key", {{"type", "string"}, {"description", "Optional API key. Omit for anonymous."}}}
            }}
        }}
    });

    tools.push_back({
        {"name", "disconnect"},
        {"description", "Disconnect from the API."},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
    });

    json result;
    result["tools"] = tools;
    SendResult(id, result);
}

void MCPServer::HandleToolsCall(const json& id, const json& params) {
    std::string toolName = params.value("name", "");
    json args = params.value("arguments", json::object());

    if (toolName == "send_message") {
        // Async tool: save the request id, initiate the send,
        // and respond later when OnMessageReceived or OnError fires.
        std::string message = args.value("message", "");
        if (message.empty()) {
            SendResult(id, ToolResult("Error: 'message' argument is required", true));
            return;
        }

        if (pendingCallId_.has_value()) {
            SendResult(id, ToolResult("Error: another message is already pending. "
                                       "Wait for the current response first.", true));
            return;
        }

        if (!connected_) {
            SendResult(id, ToolResult("Error: not connected to the API. "
                                       "Use the 'connect' tool first.", true));
            return;
        }

        pendingCallId_ = id;
        responseReceived_ = false;
        chatHistory_.push_back("You: " + wxString::FromUTF8(message));

        // Post to the main window so the message goes through the UI
        // input box, just like a real user typed it.
        auto* topWindow = wxTheApp->GetTopWindow();
        if (topWindow) {
            auto* evt = new wxCommandEvent(MCP_SEND_MESSAGE_REQUEST);
            evt->SetString(wxString::FromUTF8(message));
            wxQueueEvent(topWindow->GetEventHandler(), evt);
        }

        // Response will be sent from OnMessageReceived() or OnError()
        return;
    }

    json result;
    if (toolName == "get_status") {
        result = ToolGetStatus();
    } else if (toolName == "get_models") {
        result = ToolGetModels();
    } else if (toolName == "get_response") {
        result = ToolGetResponse();
    } else if (toolName == "get_chat_history") {
        result = ToolGetChatHistory();
    } else if (toolName == "set_model") {
        result = ToolSetModel(args);
    } else if (toolName == "connect") {
        result = ToolConnect(args);
    } else if (toolName == "disconnect") {
        result = ToolDisconnect();
    } else {
        SendResult(id, ToolResult("Unknown tool: " + toolName, true));
        return;
    }

    SendResult(id, result);
}

// --- Tool implementations ---

json MCPServer::ToolGetStatus() {
    auto& zen = zen::ZenClient::Instance();

    json status;
    status["connected"] = connected_;
    status["status"] = connectionStatus_.ToStdString();
    status["anonymous"] = zen.IsAnonymous();
    status["models_loaded"] = modelsLoaded_;
    status["model_count"] = static_cast<int>(availableModels_.size());
    status["active_model"] = zen.GetActiveModel();
    status["chat_message_count"] = static_cast<int>(chatHistory_.size());
    status["response_pending"] = pendingCallId_.has_value();

    return ToolResult(status.dump(2));
}

json MCPServer::ToolGetModels() {
    json models = json::array();
    for (const auto& [id, name] : availableModels_) {
        models.push_back({{"id", id}, {"name", name}});
    }
    return ToolResult(models.dump(2));
}

json MCPServer::ToolGetResponse() {
    if (lastResponse_.IsEmpty()) {
        return ToolResult("No response received yet.");
    }
    return ToolResult(lastResponse_.ToStdString());
}

json MCPServer::ToolGetChatHistory() {
    if (chatHistory_.empty()) {
        return ToolResult("Chat history is empty.");
    }

    std::string history;
    for (const auto& msg : chatHistory_) {
        history += msg.ToStdString() + "\n";
    }
    return ToolResult(history);
}

json MCPServer::ToolSetModel(const json& args) {
    std::string modelId = args.value("model_id", "");
    if (modelId.empty()) {
        return ToolResult("Error: 'model_id' argument is required", true);
    }

    auto& zen = zen::ZenClient::Instance();
    zen.SetActiveModel(modelId);
    return ToolResult("Active model set to: " + modelId);
}

json MCPServer::ToolConnect(const json& args) {
    std::string apiKey = args.value("api_key", "");
    auto& zen = zen::ZenClient::Instance();
    zen.Connect(apiKey);
    return ToolResult("Connection initiated" +
                      std::string(apiKey.empty() ? " (anonymous)" : " (with API key)"));
}

json MCPServer::ToolDisconnect() {
    auto& zen = zen::ZenClient::Instance();
    zen.Disconnect();
    return ToolResult("Disconnected from API.");
}

// --- Output helpers ---

json MCPServer::TextContent(const std::string& text) {
    return {{"type", "text"}, {"text", text}};
}

json MCPServer::ToolResult(const std::string& text, bool isError) {
    json result;
    result["content"] = json::array({TextContent(text)});
    if (isError) {
        result["isError"] = true;
    }
    return result;
}

void MCPServer::SendJsonRpc(const json& msg) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    std::cout << msg.dump() << "\n" << std::flush;
}

void MCPServer::SendResult(const json& id, const json& result) {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    SendJsonRpc(response);
}

void MCPServer::SendError(const json& id, int code, const std::string& message) {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"]["code"] = code;
    response["error"]["message"] = message;
    SendJsonRpc(response);
}

// --- Event handlers from ZenClient (called via MainFrame on main thread) ---

void MCPServer::OnMessageReceived(const wxString& message, int tokens) {
    lastResponse_ = message;
    chatHistory_.push_back("AI: " + message);
    responseReceived_ = true;

    wxLogMessage("MCPServer: Response received, tokens=%d", tokens);

    // Complete the pending MCP tool call if one is waiting
    if (pendingCallId_.has_value()) {
        json id = *pendingCallId_;
        pendingCallId_.reset();
        SendResult(id, ToolResult(message.ToStdString()));
    }
}

void MCPServer::OnConnected(const wxString& status) {
    connected_ = true;
    connectionStatus_ = status;
    wxLogMessage("MCPServer: Connected - %s", status);
}

void MCPServer::OnDisconnected() {
    connected_ = false;
    connectionStatus_ = "Disconnected";
    wxLogMessage("MCPServer: Disconnected");
}

void MCPServer::OnError(const wxString& error) {
    chatHistory_.push_back("Error: " + error);
    wxLogMessage("MCPServer: Error - %s", error);

    // Complete the pending MCP tool call with an error
    if (pendingCallId_.has_value()) {
        json id = *pendingCallId_;
        pendingCallId_.reset();
        SendResult(id, ToolResult("Error from AI: " + error.ToStdString(), true));
    }
}

void MCPServer::OnModelsLoaded() {
    auto& zen = zen::ZenClient::Instance();
    auto models = zen.IsAnonymous() ? zen.GetFreeModels() : zen.GetModels();

    availableModels_.clear();
    for (const auto& model : models) {
        availableModels_.push_back({model.id, model.name});
    }

    modelsLoaded_ = true;
    wxLogMessage("MCPServer: Models loaded, count=%zu", availableModels_.size());
}

} // namespace fcn::mcp
