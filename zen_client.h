#pragma once

#include "chat_provider.h"
#include <wx/event.h>
#include <memory>
#include <vector>

namespace fcn::zen {

class ZenClient : public wxEvtHandler {
public:
  static ZenClient& Instance();

  // Provider management
  void SetProvider(ProviderType type);
  ProviderType GetProviderType() const { return providerType_; }

  bool Connect(const std::string& apiKey = "");
  void Disconnect();
  bool IsConnected() const;

  void FetchModels();
  std::vector<ProviderModelInfo> GetModels() const;
  std::vector<ProviderModelInfo> GetFreeModels() const;

  void SendMessage(const std::string& model, const std::string& message);
  void ClearConversation();

  void SetActiveModel(const std::string& modelId);
  std::string GetActiveModel() const;

  bool IsAnonymous() const;
  bool NeedsApiKey() const;

  void SetJsonLogCallback(JsonLogCallback callback);

private:
  ZenClient();
  ~ZenClient();

  ZenClient(const ZenClient&) = delete;
  ZenClient& operator=(const ZenClient&) = delete;

  void OnModelsReceived(const std::vector<ProviderModelInfo>& models);

  std::unique_ptr<ChatProvider> provider_;
  ProviderType providerType_ = ProviderType::Zen;
  std::string activeModel_;
  bool connected_ = false;

  std::vector<ProviderModelInfo> cachedModels_;
  std::vector<ChatMessage> conversationHistory_;
  JsonLogCallback jsonLogCallback_;
};

wxDECLARE_EVENT(ZEN_MESSAGE_RECEIVED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_STREAM_CHUNK, wxCommandEvent);
wxDECLARE_EVENT(ZEN_ERROR_OCCURRED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_CONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_DISCONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_MODELS_LOADED, wxCommandEvent);

} // namespace fcn::zen
