#pragma once

#include <wx/event.h>
#include <wx/string.h>
#include <wx/socket.h>
#include <wx/timer.h>
#include <nlohmann/json.hpp>

namespace zencode::mcp {

using json = nlohmann::json;

// MCP Server with automatic retry and rate limiting protection
class MCPServer : public wxEvtHandler {
public:
  static MCPServer& Instance();
  
  bool Start(int port = 8765);
  void Stop();
  bool IsRunning() const;
  int GetPort() const { return port_; }
  bool WaitForServer(int timeoutMs = 10000);
  
  // Internal event handlers from ZenClient (called from main thread)
  void OnMessageReceived(const wxString& message, int tokens);
  void OnConnected(const wxString& status);
  void OnDisconnected();
  void OnError(const wxString& error);
  void OnModelsLoaded();
  
  // Get current state (thread-safe, called from main thread)
  json GetStatus() const;
  json GetModels() const;
  json GetChatHistory() const;
  json GetResponse() const;
  json GetUIState() const;
  
  // Rate limiting protection
  void SetLastRequestTime();
  bool CanMakeRequest();
  int GetSecondsUntilNextRequest();

private:
  MCPServer();
  ~MCPServer();
  
  MCPServer(const MCPServer&) = delete;
  MCPServer& operator=(const MCPServer&) = delete;
  
  // Socket handling on main thread
  void OnTimer(wxTimerEvent& event);
  void ProcessClientRequest(wxSocketBase* client);
  json ProcessCommand(const json& cmd);
  void SendResponse(wxSocketBase* client, const json& response);
  
  // Command handlers
  json HandleGetStatus();
  json HandleGetModels();
  json HandleSendMessage(const json& params);
  json HandleGetResponse();
  json HandleGetChatHistory();
  json HandleConnect(const json& params);
  json HandleDisconnect();
  json HandleSetModel(const json& params);
  json HandleGetUIState();
  json HandleExecuteTest(const json& params);
  
  // Server components (all on main thread)
  wxSocketServer* server_ = nullptr;
  wxTimer* timer_ = nullptr;
  int port_ = 8765;
  bool running_ = false;
  
  // State tracking
  wxString lastResponse_;
  std::vector<wxString> chatHistory_;
  bool responseReceived_ = false;
  bool connected_ = false;
  wxString connectionStatus_;
  std::vector<std::pair<std::string, std::string>> availableModels_;
  bool modelsLoaded_ = false;
  
  // Rate limiting
  wxLongLong lastRequestTime_ = 0;
  static const int MIN_REQUEST_INTERVAL_MS = 2000; // 2 seconds between requests
};

} // namespace zencode::mcp
