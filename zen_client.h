#pragma once

#include "http_client.h"
#include <wx/event.h>
#include <memory>
#include <vector>

namespace fcn::zen {

class ZenClient : public wxEvtHandler {
public:
  static ZenClient& Instance();
  
  bool Connect(const std::string& apiKey = "");
  void Disconnect();
  bool IsConnected() const;
  
  void FetchModels();
  std::vector<network::ModelInfo> GetModels() const;
  std::vector<network::ModelInfo> GetFreeModels() const;
  
  void SendMessage(const std::string& model, const std::string& message);
  
  void SetActiveModel(const std::string& modelId);
  std::string GetActiveModel() const;
  
  bool IsAnonymous() const;

private:
  ZenClient();
  ~ZenClient();
  
  ZenClient(const ZenClient&) = delete;
  ZenClient& operator=(const ZenClient&) = delete;
  
  void OnModelsReceived(const std::vector<network::ModelInfo>& models);
  void OnChatResponse(const network::ChatResponse& response);
  
  std::unique_ptr<network::HttpClient> httpClient_;
  std::string activeModel_;
  bool connected_ = false;
  
  std::vector<network::ModelInfo> cachedModels_;
};

wxDECLARE_EVENT(ZEN_MESSAGE_RECEIVED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_ERROR_OCCURRED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_CONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_DISCONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_MODELS_LOADED, wxCommandEvent);

} // namespace fcn::zen
