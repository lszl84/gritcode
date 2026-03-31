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
  client_ = std::make_unique<claude::ClaudeClient>();
}

ZenClient::~ZenClient() {
  Disconnect();
}

ZenClient& ZenClient::Instance() {
  static ZenClient instance;
  return instance;
}

bool ZenClient::Connect(const std::string& /*apiKey*/) {
  if (connected_) {
    wxCommandEvent event(ZEN_CONNECTED);
    event.SetString("Already connected");
    wxPostEvent(this, event);
    return true;
  }

  if (!client_->Initialize()) {
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Failed to initialize claude client");
    wxPostEvent(this, event);
    return false;
  }

  connected_ = true;

  wxCommandEvent event(ZEN_CONNECTED);
  event.SetString("Connected via ACP (system claude binary)");
  wxPostEvent(this, event);

  FetchModels();
  return true;
}

void ZenClient::Disconnect() {
  if (client_) client_->Shutdown();
  connected_ = false;
  cachedModels_.clear();

  wxCommandEvent event(ZEN_DISCONNECTED);
  wxPostEvent(this, event);
}

bool ZenClient::IsConnected() const {
  return connected_;
}

void ZenClient::FetchModels() {
  cachedModels_ = claude::ClaudeClient::GetAvailableModels();

  if (activeModel_.empty() && !cachedModels_.empty()) {
    activeModel_ = cachedModels_[0].id;
  }

  wxCommandEvent event(ZEN_MODELS_LOADED);
  wxPostEvent(this, event);
}

std::vector<claude::ModelInfo> ZenClient::GetModels() const {
  return cachedModels_;
}

void ZenClient::SendMessage(const std::string& model, const std::string& message) {
  if (!connected_) {
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Not connected");
    wxPostEvent(this, event);
    return;
  }

  const std::string useModel = model.empty() ? activeModel_ : model;
  wxLogMessage("ZenClient::SendMessage: model='%s', len=%zu", useModel.c_str(), message.length());

  client_->SendMessage(
    useModel,
    message,
    [this](const std::string& chunk) {
      wxCommandEvent evt(ZEN_STREAM_CHUNK);
      evt.SetString(wxString::FromUTF8(chunk.c_str(), chunk.length()));
      wxPostEvent(this, evt);
    },
    [this](bool success, const std::string& fullText, int inputTok, int outputTok) {
      if (!success) {
        wxCommandEvent evt(ZEN_ERROR_OCCURRED);
        evt.SetString(wxString::FromUTF8(fullText));
        wxPostEvent(this, evt);
        return;
      }
      wxCommandEvent evt(ZEN_MESSAGE_RECEIVED);
      evt.SetString(wxString::FromUTF8(fullText.c_str(), fullText.length()));
      evt.SetExtraLong(inputTok + outputTok);
      wxPostEvent(this, evt);
    }
  );
}

void ZenClient::SetActiveModel(const std::string& modelId) {
  activeModel_ = modelId;
  // New model means new conversation — reset session so the next message
  // does not resume the previous model's session.
  client_->ResetSession();
}

std::string ZenClient::GetActiveModel() const {
  return activeModel_;
}

void ZenClient::SetJsonLogCallback(claude::JsonLogCallback callback) {
  client_->SetJsonLogCallback(std::move(callback));
}

} // namespace fcn::zen
