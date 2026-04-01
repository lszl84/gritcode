#include "zen_client.h"
#include <wx/wx.h>
#include <wx/log.h>

namespace fcn::zen {

wxDEFINE_EVENT(ZEN_MESSAGE_RECEIVED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_STREAM_CHUNK, wxCommandEvent);
wxDEFINE_EVENT(ZEN_ERROR_OCCURRED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_CONNECTED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_DISCONNECTED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_MODELS_LOADED, wxCommandEvent);

ZenClient::ZenClient() {
  provider_ = CreateZenProvider();
}

ZenClient::~ZenClient() {
  Disconnect();
}

ZenClient& ZenClient::Instance() {
  static ZenClient instance;
  return instance;
}

void ZenClient::SetProvider(ProviderType type) {
  if (type == providerType_ && provider_) return;

  if (connected_) Disconnect();

  providerType_ = type;
  provider_ = (type == ProviderType::Zen) ? CreateZenProvider() : CreateClaudeProvider();

  if (jsonLogCallback_) provider_->SetJsonLogCallback(jsonLogCallback_);

  // Keep conversation history across provider switches so context is preserved
}

bool ZenClient::Connect(const std::string& apiKey) {
  wxLogMessage("ZenClient::Connect: provider=%s", provider_->GetDisplayName().c_str());

  if (connected_) {
    wxCommandEvent event(ZEN_CONNECTED);
    event.SetString("Already connected");
    wxPostEvent(this, event);
    return true;
  }

  if (!provider_->Initialize()) {
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Failed to initialize " + provider_->GetDisplayName());
    wxPostEvent(this, event);
    return false;
  }

  if (provider_->NeedsApiKey()) {
    provider_->SetApiKey(apiKey);
  }

  connected_ = true;

  wxCommandEvent event(ZEN_CONNECTED);
  wxString status = wxString::Format("Connected via %s", provider_->GetDisplayName().c_str());
  if (provider_->NeedsApiKey() && provider_->IsAnonymous()) {
    status += " (anonymous)";
  }
  event.SetString(status);
  wxPostEvent(this, event);

  FetchModels();
  return true;
}

void ZenClient::Disconnect() {
  if (provider_) provider_->Shutdown();
  connected_ = false;
  cachedModels_.clear();

  wxCommandEvent event(ZEN_DISCONNECTED);
  wxPostEvent(this, event);
}

bool ZenClient::IsConnected() const {
  return connected_;
}

void ZenClient::FetchModels() {
  if (!connected_) return;

  provider_->FetchModels([this](const std::vector<ProviderModelInfo>& models) {
    this->OnModelsReceived(models);
  });
}

void ZenClient::OnModelsReceived(const std::vector<ProviderModelInfo>& models) {
  cachedModels_ = models;

  wxLogMessage("ZenClient::OnModelsReceived: %zu models from %s",
               models.size(), provider_->GetDisplayName().c_str());

  // Fallback free models for Zen when API returns nothing
  if (cachedModels_.empty() && providerType_ == ProviderType::Zen) {
    wxLogWarning("ZenClient: No models received, using fallback models");
    cachedModels_ = {
      {"big-pickle", "Big Pickle", true, 100},
      {"mimo-v2-flash-free", "MiMo V2 Flash Free", true, 100},
      {"nemotron-3-super-free", "Nemotron 3 Super Free", true, 100},
      {"minimax-m2.5-free", "MiniMax M2.5 Free", true, 100}
    };
  }

  // Pick default model
  if (activeModel_.empty() && !cachedModels_.empty()) {
    if (provider_->IsAnonymous()) {
      auto free = GetFreeModels();
      if (!free.empty()) activeModel_ = free[0].id;
    }
    if (activeModel_.empty()) activeModel_ = cachedModels_[0].id;
  }

  wxCommandEvent event(ZEN_MODELS_LOADED);
  wxPostEvent(this, event);
}

std::vector<ProviderModelInfo> ZenClient::GetModels() const {
  return cachedModels_;
}

std::vector<ProviderModelInfo> ZenClient::GetFreeModels() const {
  std::vector<ProviderModelInfo> freeModels;
  for (const auto& model : cachedModels_) {
    if (model.allowAnonymous ||
        model.id.find("free") != std::string::npos ||
        model.id.find("big-pickle") != std::string::npos) {
      freeModels.push_back(model);
    }
  }
  return freeModels;
}

void ZenClient::SendMessage(const std::string& model, const std::string& message) {
  if (!connected_) {
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Not connected");
    wxPostEvent(this, event);
    return;
  }

  const std::string useModel = model.empty() ? activeModel_ : model;
  wxLogMessage("ZenClient::SendMessage: model='%s', len=%zu, history=%zu",
               useModel.c_str(), message.length(), conversationHistory_.size());

  conversationHistory_.push_back({"user", message});

  provider_->SendMessage(
    useModel, message, conversationHistory_,
    // onChunk
    [this](const std::string& chunk, bool isThinking) {
      wxCommandEvent evt(ZEN_STREAM_CHUNK);
      evt.SetString(wxString::FromUTF8(chunk.c_str(), chunk.length()));
      evt.SetExtraLong(isThinking ? 1 : 0);
      wxPostEvent(this, evt);
    },
    // onComplete
    [this](bool success, const std::string& content, const std::string& error,
           int inputTokens, int outputTokens) {
      if (!success) {
        // Rollback history on error
        if (!conversationHistory_.empty() &&
            conversationHistory_.back().role == "user") {
          conversationHistory_.pop_back();
        }
        wxCommandEvent evt(ZEN_ERROR_OCCURRED);
        evt.SetString(wxString::FromUTF8(error));
        wxPostEvent(this, evt);
        return;
      }

      // Store assistant response in history
      if (!content.empty()) {
        conversationHistory_.push_back({"assistant", content});
        wxLogMessage("ZenClient: History now has %zu messages", conversationHistory_.size());
      }

      wxCommandEvent evt(ZEN_MESSAGE_RECEIVED);
      evt.SetString(wxString::FromUTF8(content.c_str(), content.length()));
      evt.SetExtraLong(inputTokens + outputTokens);
      wxPostEvent(this, evt);
    }
  );
}

void ZenClient::ClearConversation() {
  conversationHistory_.clear();
  if (provider_) provider_->ResetContext();
  wxLogMessage("ZenClient::ClearConversation: Reset");
}

void ZenClient::SetActiveModel(const std::string& modelId) {
  activeModel_ = modelId;
}

std::string ZenClient::GetActiveModel() const {
  return activeModel_;
}

bool ZenClient::IsAnonymous() const {
  return provider_ ? provider_->IsAnonymous() : true;
}

bool ZenClient::NeedsApiKey() const {
  return provider_ ? provider_->NeedsApiKey() : false;
}

void ZenClient::SetJsonLogCallback(JsonLogCallback callback) {
  jsonLogCallback_ = callback;
  if (provider_) provider_->SetJsonLogCallback(callback);
}

} // namespace fcn::zen
