#include "mcp/mcp_server.h"
#include "network/zen_client.h"
#include <wx/wx.h>
#include <wx/log.h>
#include <wx/sckipc.h>
#include <sstream>

namespace zencode::mcp {

MCPServer::MCPServer() {
  timer_ = new wxTimer(this);
  Bind(wxEVT_TIMER, &MCPServer::OnTimer, this);
}

MCPServer::~MCPServer() {
  Stop();
  if (timer_) {
    delete timer_;
  }
}

MCPServer& MCPServer::Instance() {
  static MCPServer instance;
  return instance;
}

bool MCPServer::WaitForServer(int timeoutMs) {
  auto start = wxGetLocalTimeMillis();
  while (!running_) {
    if ((wxGetLocalTimeMillis() - start).GetValue() > timeoutMs) {
      return false;
    }
    wxMilliSleep(100);
  }
  return running_;
}

bool MCPServer::Start(int port) {
  if (running_) {
    wxLogMessage("MCPServer::Start: Already running on port %d", port_);
    return true;
  }
  
  // Try multiple ports
  for (int attempt = 0; attempt < 5; attempt++) {
    int tryPort = port + attempt;
    
    wxLogMessage("MCPServer::Start: Attempting port %d (attempt %d/5)...", tryPort, attempt + 1);
    
    // Create socket server
    wxIPV4address addr;
    addr.Service(tryPort);
    addr.Hostname("127.0.0.1");
    
    if (server_) {
      delete server_;
      server_ = nullptr;
    }
    
    server_ = new wxSocketServer(addr);
    
    if (server_->IsOk()) {
      port_ = tryPort;
      wxLogMessage("MCPServer::Start: ✓ Server listening on port %d", port_);
      
      // Start polling timer
      timer_->Start(100);
      running_ = true;
      return true;
    }
    
    wxLogWarning("MCPServer::Start: Port %d failed, trying next...", tryPort);
    delete server_;
    server_ = nullptr;
    
    // Small delay between attempts
    wxMilliSleep(200);
  }
  
  wxLogError("MCPServer::Start: Failed to start server on any port");
  return false;
}

void MCPServer::Stop() {
  if (!running_) return;
  
  wxLogMessage("MCPServer::Stop: Stopping server...");
  
  timer_->Stop();
  
  if (server_) {
    server_->Destroy();
    server_ = nullptr;
  }
  
  running_ = false;
  wxLogMessage("MCPServer::Stop: Server stopped");
}

bool MCPServer::IsRunning() const {
  return running_;
}

// Rate limiting
void MCPServer::SetLastRequestTime() {
  lastRequestTime_ = wxGetLocalTimeMillis();
}

bool MCPServer::CanMakeRequest() {
  wxLongLong elapsed = wxGetLocalTimeMillis() - lastRequestTime_;
  return elapsed.GetValue() >= MIN_REQUEST_INTERVAL_MS;
}

int MCPServer::GetSecondsUntilNextRequest() {
  wxLongLong elapsed = wxGetLocalTimeMillis() - lastRequestTime_;
  int remaining = (MIN_REQUEST_INTERVAL_MS - elapsed.GetValue()) / 1000;
  return remaining > 0 ? remaining : 0;
}

void MCPServer::OnTimer(wxTimerEvent& event) {
  if (!server_ || !server_->IsOk()) return;
  
  // Accept pending connections
  wxSocketBase* client = server_->Accept(false);
  while (client) {
    wxLogMessage("MCPServer::OnTimer: Client connected");
    
    // Process client immediately
    ProcessClientRequest(client);
    
    // Try to accept more
    client = server_->Accept(false);
  }
}

void MCPServer::ProcessClientRequest(wxSocketBase* client) {
  // Wait for data
  if (!client->WaitForRead(10, 0)) {
    wxLogWarning("MCPServer::OnTimer: Timeout waiting for data");
    client->Destroy();
    return;
  }
  
  // Read request
  char buffer[4096];
  client->SetFlags(wxSOCKET_BLOCK);
  client->Read(buffer, sizeof(buffer) - 1);
  
  if (client->LastCount() == 0) {
    wxLogWarning("MCPServer::OnTimer: No data received");
    client->Destroy();
    return;
  }
  
  buffer[client->LastCount()] = '\0';
  wxString cmdStr(buffer);
  wxLogMessage("MCPServer::OnTimer: Received: %s", cmdStr.Left(200));
  
  // Process and respond
  try {
    json cmdJson = json::parse(cmdStr.ToStdString());
    json response = ProcessCommand(cmdJson);
    std::string respStr = response.dump() + "\n";
    
    wxLogMessage("MCPServer::OnTimer: Sending response (%zu bytes)", respStr.length());
    
    client->Write(respStr.c_str(), respStr.length());
    client->WaitForWrite(5, 0);
    
    wxLogMessage("MCPServer::OnTimer: Response sent");
    
  } catch (const json::exception& e) {
    wxLogError("MCPServer::OnTimer: JSON parse error: %s", e.what());
    json errorJson;
    errorJson["id"] = 0;
    errorJson["success"] = false;
    errorJson["error"] = std::string("JSON parse error: ") + e.what();
    std::string errorStr = errorJson.dump() + "\n";
    client->Write(errorStr.c_str(), errorStr.length());
    client->WaitForWrite(3, 0);
  }
  
  client->Destroy();
}

json MCPServer::ProcessCommand(const json& cmd) {
  json response;
  response["id"] = cmd.value("id", 0);
  response["success"] = true;
  
  std::string method = cmd.value("method", "");
  
  if (method == "getStatus") {
    response["result"] = HandleGetStatus();
  } else if (method == "getModels") {
    response["result"] = HandleGetModels();
  } else if (method == "sendMessage") {
    response["result"] = HandleSendMessage(cmd.value("params", json::object()));
  } else if (method == "getResponse") {
    response["result"] = HandleGetResponse();
  } else if (method == "getChatHistory") {
    response["result"] = HandleGetChatHistory();
  } else if (method == "connect") {
    response["result"] = HandleConnect(cmd.value("params", json::object()));
  } else if (method == "disconnect") {
    response["result"] = HandleDisconnect();
  } else if (method == "setModel") {
    response["result"] = HandleSetModel(cmd.value("params", json::object()));
  } else if (method == "getUIState") {
    response["result"] = HandleGetUIState();
  } else if (method == "executeTest") {
    response["result"] = HandleExecuteTest(cmd.value("params", json::object()));
  } else {
    response["success"] = false;
    response["error"] = "Unknown method: " + method;
  }
  
  return response;
}

// Command handlers
json MCPServer::HandleGetStatus() {
  json result;
  result["connected"] = connected_;
  result["status"] = connectionStatus_.ToStdString();
  result["modelsLoaded"] = modelsLoaded_;
  result["modelCount"] = availableModels_.size();
  result["chatMessageCount"] = chatHistory_.size();
  result["responsePending"] = !responseReceived_ && connected_;
  result["port"] = port_;
  return result;
}

json MCPServer::HandleGetModels() {
  json result = json::array();
  for (const auto& model : availableModels_) {
    json m;
    m["id"] = model.first;
    m["name"] = model.second;
    result.push_back(m);
  }
  return result;
}

json MCPServer::HandleSendMessage(const json& params) {
  json result;
  
  if (!params.contains("message")) {
    result["error"] = "Missing 'message' parameter";
    return result;
  }
  
  // Check rate limiting
  if (!CanMakeRequest()) {
    int waitSec = GetSecondsUntilNextRequest();
    result["error"] = "Rate limited. Please wait " + std::to_string(waitSec) + " seconds";
    result["rateLimited"] = true;
    result["waitSeconds"] = waitSec;
    return result;
  }
  
  std::string message = params["message"];
  responseReceived_ = false;
  chatHistory_.push_back("You: " + wxString::FromUTF8(message));
  
  // Record request time
  SetLastRequestTime();
  
  // Send via ZenClient
  auto& zen = zen::ZenClient::Instance();
  std::string model = zen.GetActiveModel();
  if (model.empty()) model = "big-pickle";
  zen.SendMessage(model, message);
  
  result["sent"] = true;
  result["message"] = message;
  result["rateLimited"] = false;
  return result;
}

json MCPServer::HandleGetResponse() {
  json result;
  result["received"] = responseReceived_;
  result["content"] = lastResponse_.ToStdString();
  result["empty"] = lastResponse_.IsEmpty();
  return result;
}

json MCPServer::HandleGetChatHistory() {
  json result = json::array();
  for (const auto& msg : chatHistory_) {
    result.push_back(msg.ToStdString());
  }
  return result;
}

json MCPServer::HandleConnect(const json& params) {
  json result;
  std::string apiKey = params.value("apiKey", "");
  
  auto& zen = zen::ZenClient::Instance();
  zen.Connect(apiKey);
  
  result["initiated"] = true;
  result["apiKeyUsed"] = !apiKey.empty();
  return result;
}

json MCPServer::HandleDisconnect() {
  json result;
  auto& zen = zen::ZenClient::Instance();
  zen.Disconnect();
  result["disconnected"] = true;
  return result;
}

json MCPServer::HandleSetModel(const json& params) {
  json result;
  
  if (!params.contains("modelId")) {
    result["error"] = "Missing 'modelId' parameter";
    return result;
  }
  
  std::string modelId = params["modelId"];
  auto& zen = zen::ZenClient::Instance();
  zen.SetActiveModel(modelId);
  
  result["set"] = true;
  result["modelId"] = modelId;
  return result;
}

json MCPServer::HandleGetUIState() {
  json result;
  result["connected"] = connected_;
  result["status"] = connectionStatus_.ToStdString();
  result["modelsLoaded"] = modelsLoaded_;
  result["modelCount"] = availableModels_.size();
  result["sendButtonEnabled"] = connected_;
  result["chatMessageCount"] = chatHistory_.size();
  result["lastResponseEmpty"] = lastResponse_.IsEmpty();
  result["responsePending"] = !responseReceived_;
  result["canMakeRequest"] = CanMakeRequest();
  result["secondsUntilNextRequest"] = GetSecondsUntilNextRequest();
  return result;
}

json MCPServer::HandleExecuteTest(const json& params) {
  json result;
  std::string testName = params.value("testName", "unknown");
  
  wxLogMessage("MCPServer::HandleExecuteTest: Running test '%s'", testName.c_str());
  
  if (testName == "basicChat") {
    result["test"] = "basicChat";
    result["description"] = "Send message and verify response";
    result["currentStatus"] = HandleGetStatus();
  } else if (testName == "connection") {
    result["test"] = "connection";
    result["connected"] = connected_;
    result["status"] = connectionStatus_.ToStdString();
  } else {
    result["error"] = "Unknown test: " + testName;
  }
  
  return result;
}

// Event handlers from ZenClient (called from main thread)
void MCPServer::OnMessageReceived(const wxString& message, int tokens) {
  lastResponse_ = message;
  chatHistory_.push_back("AI: " + message);
  responseReceived_ = true;
  wxLogMessage("MCPServer: Response received, tokens=%d", tokens);
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
}

void MCPServer::OnModelsLoaded() {
  auto& zen = zen::ZenClient::Instance();
  auto models = zen.GetFreeModels();
  
  availableModels_.clear();
  for (const auto& model : models) {
    availableModels_.push_back({model.id, model.name});
  }
  
  modelsLoaded_ = true;
  wxLogMessage("MCPServer: Models loaded, count=%zu", availableModels_.size());
}

// Public getters
json MCPServer::GetStatus() const {
  return const_cast<MCPServer*>(this)->HandleGetStatus();
}

json MCPServer::GetModels() const {
  return const_cast<MCPServer*>(this)->HandleGetModels();
}

json MCPServer::GetChatHistory() const {
  return const_cast<MCPServer*>(this)->HandleGetChatHistory();
}

json MCPServer::GetResponse() const {
  return const_cast<MCPServer*>(this)->HandleGetResponse();
}

json MCPServer::GetUIState() const {
  return const_cast<MCPServer*>(this)->HandleGetUIState();
}

} // namespace zencode::mcp
